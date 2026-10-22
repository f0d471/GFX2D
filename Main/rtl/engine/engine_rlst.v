`timescale 1ns/1ps
//==============================================================================================//
// FILE: engine_rlst.v
//
// DESCRIPTION: R2 三维列表取数：子表索引流（64 深 u16 FIFO）与 128B 主表记录通道
//   分时共享搬运层 FD（全局一笔在途），产出双缓冲参数槽（槽寄存器在 engine_rast
//   内）与条带边界握手。单 FSM 串行推进：子表突发 → 表头/索引解析 → 记录取回。
//
// NOTE:
//   1. 字节 ABI 与组织见 Main/doc/三维数值契约.md 与 engine-manual.md；记录字段
//      合法性归 engine_rast（消费端校验），本层只查索引/游标/流协议
//   2. FD owner 保持到「状态脉冲已捕获且读流 tlast 已握手」；取消不打断在途
//      描述符：canceled 置位后各状态自行 divert，在途流与状态照常吞完才收尾
//      （AXI 已受理的读不能中途省略余下传输）
//   3. 索引突发发描述符前预留整批逻辑 u16 的 FIFO 空位，防止在途突发堵住 FD
//   4. 主表记录 32B 对齐，包络按 4KB 页界拆两段（96+32/64+64/32+96）；片段内
//      整拍直写槽寄存器（128b/拍）。子表从 16B 对齐基址整流读入，pad 字节在
//      u16 序列化处按逻辑长度丢弃
//   5. 取数/流协议失败 FETCH(9)，索引越界或子表保留位非 0 归 BAD_INDEX(13)；
//      记录字段坏归 rast 的 BAD_FIELD(2)。首错后不再开始新工作，协议照常排空
//   6. 每条带都发 strip_req/strip_done（count=0 的条带只花头部成本）；
//      strip_ack 为电平，帧层备好条带后拉高，排空并写回后撤下才进入下一带
//   7. 注错开关见 engine-manual.md；TRI_COUNT=0 的全帧绕过由帧层不启动本模块
//==============================================================================================//
`default_nettype none
`include "engine_feature.vh"
`include "engine_regs_offsets.vh"

module engine_rlst (
    input  wire         clk,
    input  wire         rst,
    input  wire         cancel,
    input  wire         go,

    // 帧配置（R3 门铃锁存；单元仿真直接驱动）
    input  wire [31:0]  tri_base,       // 主表基址，32B 对齐
    input  wire [31:0]  sub_base,       // 子表基址，16B 对齐
    input  wire [23:0]  sub_bytes,      // 子表逻辑长度（不含 pad，恒为偶数）
    input  wire [15:0]  tri_count,

    // 搬运层 FD/FS（光栅表类；单元仿真由 FD 替身驱动）
    output wire [31:0]  fd_addr,
    output wire [11:0]  fd_bytes,
    output wire [3:0]   fd_cidx,
    output wire         fd_raster,
    output wire         fd_valid,
    input  wire         fd_ready,
    input  wire [127:0] fs_data,
    input  wire         fs_valid,
    input  wire         fs_last,
    input  wire [15:0]  fs_keep,
    input  wire [3:0]   fs_cidx,
    output wire         fs_ready,
    input  wire         st_valid,       // 光栅表读状态（mover 的 rd_stat_raster）
    input  wire         st_error,
    input  wire [3:0]   st_cidx,

    // 参数槽出口（槽寄存器在 engine_rast 内；128b/拍直写 4 个连续词）
    output wire         rp_we,
    output wire [1:0]   rp_slot,
    output wire [4:0]   rp_wa,          // 槽内首词号（4 词对齐）
    output wire [127:0] rp_wd,
    output reg          rp_valid,       // 一条记录完整交付（单拍脉冲，末写次拍起）
    output reg  [1:0]   rp_slot_v,
    output reg  [15:0]  rp_index,
    output reg  [4:0]   rp_strip,
    output reg  [15:0]  rp_ordinal,
    input  wire [1:0]   rp_free,        // 电平，位 per 槽：1=该槽无已交付未启动、
                                        //   也无在途三角形（rast 从 rp_valid 起占用）
    // 条带边界（strip_req 后帧层备好该条带并拉高 strip_ack；strip_done 后撤下）
    output reg          strip_req,
    output reg  [4:0]   cur_strip,
    input  wire         strip_ack,
    output reg          strip_done,
    output reg          frame_done,
    output reg          error,
    output reg  [3:0]   error_code,
    output reg  [15:0]  error_index,    // 完整主表索引；头部错误用 16'hFFFF 哨兵
    output reg  [31:0]  error_tag,      // {11'd0, strip[4:0], ref_ordinal[15:0]}
    output wire         busy
);
    localparam [4:0] S_IDLE = 0, S_SUB_REQ = 1, S_SUB_RECV = 2, S_CNT0 = 3, S_CNT1 = 4,
                     S_RSVD0 = 5, S_RSVD1 = 6, S_REQA = 7, S_REQB = 8, S_IDX = 9,
                     S_REC_PREP = 10, S_REC_REQ = 11, S_REC_RECV = 12, S_REC_NEXT = 13,
                     S_REC_LAST = 14, S_STRIP_DONE = 15, S_HALT = 16,
                     S_STRIP_ACK_LOW = 17;

    // go 期配置检查用的包络总长（含 pad；sub_bytes ≤ 2^24，和至多加 15）
    function [24:0] env_of;
        input [23:0] bytes;
        begin
            env_of = ({1'b0, bytes} + 25'd15) & 25'h1fffff0;
        end
    endfunction

    // 错误锁存（首错优先；error 保持到下一次 go）
    task fail_latch;
        input [3:0]  code;
        input [15:0] idx;
        input [4:0]  st;
        input [15:0] ord;
        begin
            if (!error) begin
                error       <= 1'b1;
                error_code  <= code;
                error_index <= idx;
                error_tag   <= {11'd0, st, ord};
            end
        end
    endtask

    // ---------------------------------------------------------------- 子表 FIFO
    reg  [15:0]  fifo_mem [0:63];
    reg  [6:0]   fifo_wp, fifo_rp;   // 7 位：64 项满时 wp-rp=64，与空（0）可区分
    reg  [127:0] b_data;               // 拍缓存：一拍 128b 最多 8 个 u16 串行落 FIFO
    reg  [23:0]  b_rel;                // 本拍首字节在逻辑流内的偏移（恒为偶数）
    reg  [2:0]   b_k, b_last;
    reg          b_v;
    wire [6:0]   f_pending = b_v ? {4'd0, b_last - b_k} + 7'd1 : 7'd0;
    wire [6:0]   fifo_used = (fifo_wp - fifo_rp) + f_pending;
    wire         fifo_has  = fifo_wp != fifo_rp;
    wire [15:0]  fifo_q    = fifo_mem[fifo_rp[5:0]];

    // ---------------------------------------------------------------- 子表突发计划
    reg  [1:0]   sub_state;            // 1 = 子表描述符在途（恰为 S_SUB_RECV 期间）
    reg  [24:0]  env_total, env_left;  // 包络总长/剩余（含 pad）
    reg  [31:0]  sub_p;                // 当前突发物理起始（完成时按已发拍数推进）
    reg  [3:0]   s_beats, s_recv;
    reg  [23:0]  s_rel;
    reg          s_stat_seen, s_stat_err, s_last_seen, s_proto_bad;
    wire [12:0]  s_page_left = 13'd4096 - {1'b0, sub_p[11:0]};
    wire [24:0]  s_cap_env   = env_left;
    wire [24:0]  s_cap_page  = {13'd0, s_page_left};
    wire [24:0]  s_limit     = (s_cap_env < 25'd128) ? s_cap_env : 25'd128;
    wire [24:0]  s_chunk     = (s_cap_page < s_limit) ? s_cap_page : s_limit;
    wire [3:0]   s_chunk_beats = s_chunk[7:4];
    wire [23:0]  s_logic_done  = env_total[23:0] - env_left[23:0];
    wire [24:0]  s_logic_left25 = {1'b0, sub_bytes} - {1'b0, s_logic_done};
    wire [24:0]  s_chunk_valid = (s_logic_left25 < s_chunk) ? s_logic_left25 : s_chunk;
`ifdef INJ_RLST_CREDIT
    wire         s_reserve_ok = 1'b1;
`else
    wire         s_reserve_ok  = ({1'b0, fifo_used} + s_chunk_valid[7:1]) <= 8'd64;
`endif

    // ---------------------------------------------------------------- 表头/游走
    reg  [4:0]   strip;
    reg  [15:0]  count_lo, head_count, head_left, rsvd_acc, ref_ord, cur_idx;
    reg  [23:0]  parse_bytes;

    // ---------------------------------------------------------------- 记录通道
    reg  [31:0]  r_addr;
    reg  [7:0]   r_frag_off, r_frag_len;
    reg          r_frag_final;
    reg  [3:0]   r_beats, r_recv;
    reg  [1:0]   r_slot;
    reg  [15:0]  r_index, r_ord;
    reg  [4:0]   r_strip;
    reg          r_slot_ok, r_stat_seen, r_stat_err, r_last_seen, r_bad;
    reg          r_pending_valid;
    wire [31:0]  rec_addr_cand = tri_base + {cur_idx, 7'd0};
    wire [32:0]  rec_end33     = {1'b0, tri_base} + {10'd0, cur_idx, 7'd0} + 33'd127;
    wire [12:0]  r_page_left   = 13'd4096 - {1'b0, rec_addr_cand[11:0]};
    wire [7:0]   r_frag1       = (r_page_left < 12'd128) ? r_page_left[7:0] : 8'd128;
    wire         slot_any_free = rp_free[1] || rp_free[0];

    // ---------------------------------------------------------------- 公共
    reg  [4:0]   state;
    reg  [4:0]   ret_state;         // 弹出点续接态：FIFO 见空时先取下一段子表
    reg          started, canceled;

    // ========================================================================
    // 组合输出：FD / FS / rp
    // ========================================================================
    wire grant_rec   = (state == S_REC_REQ) && r_slot_ok && !canceled && !cancel;
    wire grant_sub   = (state == S_SUB_REQ) && s_reserve_ok && !grant_rec && !canceled && !cancel;
    assign fd_valid  = grant_rec || grant_sub;
    assign fd_addr   = grant_rec ? r_addr : sub_p;
    assign fd_bytes  = grant_rec ? {4'd0, r_frag_len[7:4], 4'd0}
                                 : {1'b0, s_chunk_beats, 4'd0};
    assign fd_cidx   = 4'd0;
    assign fd_raster = 1'b1;
    wire rec_fd_fire = grant_rec && fd_ready;
    wire sub_fd_fire = grant_sub && fd_ready;

    // 取消不掉 fs_ready：在途描述符必须吞完（owner 纪律）
    assign fs_ready = ((state == S_REC_RECV) || (state == S_SUB_RECV)) && !b_v;
    wire fs_acc     = fs_valid && fs_ready;
    wire rec_beat   = (state == S_REC_RECV) && fs_acc;
    wire sub_beat   = (state == S_SUB_RECV) && fs_acc;

    assign rp_we   = rec_beat && r_recv < r_beats && !canceled && !cancel;
    assign rp_slot = r_slot;
    assign rp_wa   = r_frag_off[6:2] + {r_recv[2:0], 2'b00};
    assign rp_wd   = fs_data;

    assign busy = started;

    // 下一拍的拍缓存基偏移与本拍有效 u16 数（sub_bytes 偶、b_rel 偶 → 全 u16）
`ifdef INJ_RLST_SUB_OFFSET
    wire [23:0]  b_rel_next = s_rel + {s_recv, 5'd0};
`else
    wire [23:0]  b_rel_next = s_rel + {s_recv, 4'd0};
`endif
    wire [24:0]  rel_to_go  = {1'b0, sub_bytes} - {1'b0, b_rel_next};
    wire [3:0]   beat_cnt   = rel_to_go[24] ? 4'd0 :
                              (rel_to_go >= 25'd16) ? 4'd8 :
                              {1'd0, rel_to_go[3:1]};

    // ========================================================================
    // 主时序（单进程单 FSM）
    // ========================================================================
    always @(posedge clk) begin
        if (rst) begin
            state     <= S_IDLE;
            started   <= 1'b0;
            canceled  <= 1'b0;
            sub_state <= 2'd0;
            b_v       <= 1'b0;
            rp_valid  <= 1'b0;
            r_pending_valid <= 1'b0;
            strip_req <= 1'b0; strip_done <= 1'b0; frame_done <= 1'b0;
            error <= 1'b0;
        end else begin
            // 单拍脉冲默认值
            rp_valid   <= 1'b0;
            strip_req  <= 1'b0;
            strip_done <= 1'b0;
            frame_done <= 1'b0;
            if (r_pending_valid && !cancel && !canceled) begin
                rp_valid    <= 1'b1;
                rp_slot_v   <= r_slot;
                rp_index    <= r_index;
                rp_strip    <= r_strip;
                rp_ordinal  <= r_ord;
                r_pending_valid <= 1'b0;
            end
            // 取消只举旗，不打断在途描述符（各状态自行 divert）
            if (cancel && started && !canceled) canceled <= 1'b1;

            case (state)
                S_IDLE: if (go && !started) begin
                    started     <= 1'b1;
                    canceled    <= 1'b0;
                    ret_state   <= S_CNT0;
                    env_total   <= env_of(sub_bytes);
                    env_left    <= env_of(sub_bytes);
                    sub_p       <= sub_base;
                    sub_state   <= 2'd0;
                    strip       <= 5'd0;
                    ref_ord     <= 16'd0;
                    parse_bytes <= 24'd0;
                    b_v         <= 1'b0;
                    fifo_wp     <= 6'd0;
                    fifo_rp     <= 6'd0;
                    error       <= 1'b0;
                    error_code  <= 4'd0;
                    error_index <= 16'hFFFF;
                    error_tag   <= 32'd0;
                    // 配置静态检查：对齐与 33b 终点
                    if (tri_count == 16'd0) state <= S_HALT;
                    else if (sub_base[3:0] != 4'd0 || tri_base[4:0] != 5'd0 ||
                        sub_bytes[0] || sub_bytes < 24'd120 ||
                        ({1'b0, tri_base} + {10'd0, tri_count, 7'd0}) > 33'h100000000 ||
                        ({2'b00, sub_base} + {9'd0, env_of(sub_bytes)}) > 34'h1_0000_0000) begin
                        error <= 1'b1; error_code <= EC_ERR_BAD_FIELD;
                        state <= S_HALT;
                    end else state <= (tri_count == 16'd0) ? S_HALT : S_SUB_REQ;
                end

                // ---------------------------------------------------- 子表通道
                S_SUB_REQ: if (canceled || cancel) state <= S_HALT;
                else if (sub_fd_fire) begin
                    sub_state   <= 2'd1;
                    s_beats     <= s_chunk_beats;
                    s_recv      <= 4'd0;
                    s_rel       <= s_logic_done;
                    s_stat_seen <= 1'b0;
                    s_last_seen <= 1'b0;
                    s_stat_err  <= 1'b0;
                    s_proto_bad <= 1'b0;
                    state       <= S_SUB_RECV;
                end
                S_SUB_RECV: begin
                    if (sub_beat) begin
                        b_data   <= fs_data;
                        b_rel    <= b_rel_next;
                        b_v      <= (beat_cnt != 4'd0);
                        b_k      <= 3'd0;
                        b_last   <= beat_cnt[2:0] - 3'd1;
                        s_recv   <= s_recv + 4'd1;
                        if (fs_last) s_last_seen <= 1'b1;
                        if (fs_keep != 16'hffff || fs_cidx != 4'd0 ||
                            fs_last != (s_recv == s_beats - 4'd1)) s_proto_bad <= 1'b1;
                    end
                    if (st_valid) begin
                        s_stat_seen <= 1'b1;
                        if (st_error || st_cidx != 4'd0) s_stat_err <= 1'b1;
                    end
`ifdef INJ_RLST_OWNER_EARLY
                    if (s_stat_seen) begin
`else
                    if ((s_recv == s_beats || s_last_seen) && !b_v && s_stat_seen) begin
`endif
                        sub_state <= 2'd0;
                        sub_p     <= sub_p + {s_beats, 4'd0};
                        env_left  <= env_left - {s_beats, 4'd0};
                        if (canceled || error || s_proto_bad || s_stat_err) begin
                            if (s_proto_bad || s_stat_err)
                                fail_latch(EC_ERR_FETCH, 16'hFFFF, strip, ref_ord);
                            state <= S_HALT;
                        end else begin
                            state <= ret_state;
                        end
                    end
                end

                // ---------------------------------------------------- 表头解析
                S_CNT0: if (canceled) state <= S_HALT;
                        else if (fifo_has) begin
                    head_count <= fifo_q;
                    head_left  <= fifo_q;
                    fifo_rp    <= fifo_rp + 7'd1;
                    parse_bytes <= parse_bytes + 24'd2;
                    state      <= S_RSVD0;
                end else if (env_left != 25'd0) begin
                    ret_state <= S_CNT0;
                    state     <= S_SUB_REQ;
                end else begin
                    fail_latch(EC_ERR_FETCH, 16'hFFFF, strip, ref_ord);  // 子表截断
                    state <= S_HALT;
                end

                S_RSVD0: if (canceled) state <= S_HALT;
                         else if (fifo_has) begin
                    fifo_rp <= fifo_rp + 7'd1;
                    parse_bytes <= parse_bytes + 24'd2;
                    if (fifo_q != 16'd0) begin
                        fail_latch(EC_ERR_BAD_INDEX, 16'hFFFF, strip, ref_ord);  // 保留位非 0
                        state <= S_HALT;
                    end else begin
                        state <= S_REQA;
                    end
                end else if (env_left != 25'd0) begin
                    ret_state <= S_RSVD0;
                    state     <= S_SUB_REQ;
                end else begin
                    fail_latch(EC_ERR_FETCH, 16'hFFFF, strip, ref_ord);
                    state <= S_HALT;
                end


                // ---------------------------------------------------- 条带握手
                S_REQA: if (canceled || cancel) state <= S_HALT;
                else begin
                    strip_req <= 1'b1;
                    cur_strip <= strip;
                    state     <= S_REQB;
                end
                S_REQB: if (canceled) state <= S_HALT;
                        else if (strip_ack) begin
                    ref_ord   <= 16'd0;
                    head_left <= head_count;
                    state     <= (head_count == 16'd0) ? S_STRIP_DONE : S_IDX;
                end

                // ---------------------------------------------------- 索引与记录
                S_IDX: if (canceled) state <= S_HALT;
                       else if (fifo_has && slot_any_free) begin
                    cur_idx <= fifo_q;
                    fifo_rp <= fifo_rp + 7'd1;
                    parse_bytes <= parse_bytes + 24'd2;
                    state   <= S_REC_PREP;
                end else if (!fifo_has && env_left != 25'd0) begin
                    ret_state <= S_IDX;
                    state     <= S_SUB_REQ;
                end else if (!fifo_has) begin
                    fail_latch(EC_ERR_FETCH, 16'hFFFF, strip, ref_ord);  // 子表截断
                    state <= S_HALT;
                end
                S_REC_PREP: begin
                    if (canceled) begin
                        state <= S_HALT;
                    end else if (rec_end33[32] || cur_idx >= tri_count) begin
                        fail_latch(rec_end33[32] ? EC_ERR_FETCH : EC_ERR_BAD_INDEX,
                                   cur_idx, strip, ref_ord);
                        state <= S_HALT;
                    end else if (!slot_any_free) begin
                        state <= S_REC_PREP;
                    end else begin
                        r_addr       <= rec_addr_cand;
                        r_index      <= cur_idx;
                        r_strip      <= strip;
                        r_ord        <= ref_ord;
                        ref_ord      <= ref_ord + 16'd1;
                        head_left    <= head_left - 16'd1;
                        r_frag_off   <= 8'd0;
                        r_frag_len   <= r_frag1;
                        r_frag_final <= (r_frag1 == 8'd128);
                        r_beats      <= r_frag1[7:4];
                        r_slot       <= rp_free[0] ? 2'd0 : 2'd1;
                        r_slot_ok    <= 1'b1;
                        r_recv       <= 4'd0;
                        r_stat_seen  <= 1'b0;
                        r_last_seen  <= 1'b0;
                        r_stat_err   <= 1'b0;
                        r_bad        <= 1'b0;
                        state        <= S_REC_REQ;
                    end
                end
                S_REC_REQ: if (canceled) state <= S_HALT;
                           else if (rec_fd_fire) state <= S_REC_RECV;
                S_REC_RECV: begin
                    if (st_valid) begin
                        r_stat_seen <= 1'b1;
                        if (st_error || st_cidx != 4'd0) r_stat_err <= 1'b1;
                    end
                    if (rec_beat) begin
                        r_recv <= r_recv + 4'd1;
                        if (fs_last) r_last_seen <= 1'b1;
                        if (fs_keep != 16'hffff || fs_cidx != 4'd0 ||
                            fs_last != (r_recv == r_beats - 4'd1)) r_bad <= 1'b1;
                    end
                    if ((r_recv == r_beats || r_last_seen) && r_stat_seen && !rec_beat) begin
                        state     <= S_REC_NEXT;
                    end
                end
                S_REC_NEXT: begin
                    if (canceled) begin
                        state <= S_HALT;
                    end else if (r_bad || r_stat_err) begin
                        fail_latch(EC_ERR_FETCH, r_index, r_strip, r_ord);
                        state <= S_HALT;
                    end else if (!r_frag_final
`ifdef INJ_RLST_DROP_TAIL
                                 && 1'b0
`endif
                    ) begin
                        r_addr       <= r_addr + {24'd0, r_frag_len};
                        r_frag_off   <= r_frag_len;
                        r_frag_len   <= 8'd128 - r_frag_len;
                        r_frag_final <= 1'b1;
                        r_beats      <= (8'd128 - r_frag_len) >> 4;
                        r_recv       <= 4'd0;
                        r_stat_seen  <= 1'b0;
                        r_last_seen  <= 1'b0;
                        r_stat_err   <= 1'b0;
                        r_bad        <= 1'b0;
                        state        <= S_REC_REQ;
                    end else begin
                        r_pending_valid <= 1'b1;
                        state           <= S_REC_LAST;
                    end
                end
                S_REC_LAST: if (error || canceled || cancel) state <= S_HALT;
                            else if (!r_pending_valid && !rp_valid && !rp_free[r_slot[0]]) begin
                                if (head_left == 16'd0) state <= S_STRIP_DONE;
                                else state <= S_IDX;
                            end

                // ---------------------------------------------------- 条带收尾
                S_STRIP_DONE: if (canceled || cancel) state <= S_HALT;
                else if (rp_free == 2'b11 && !rp_valid && !r_pending_valid) begin
                    strip_done <= 1'b1;
                    state <= S_STRIP_ACK_LOW;
                end
                S_STRIP_ACK_LOW: if (canceled || cancel) state <= S_HALT;
                else if (!strip_ack) begin
                    if (strip == 5'd29) begin
                        if (parse_bytes != sub_bytes)
                            fail_latch(EC_ERR_FETCH, 16'hffff, strip, ref_ord);
                        state <= S_HALT;
                    end
                    else begin
                        strip <= strip + 5'd1;
                        state <= S_CNT0;
                    end
                end

                // ---------------------------------------------------- 收尾
                S_HALT: if (!b_v) begin
                    frame_done <= !canceled && !cancel;
                    r_pending_valid <= 1'b0;
                    started    <= 1'b0;
                    state      <= S_IDLE;
                end
                default: state <= S_IDLE;
            endcase

            // 拍缓存序列化：每拍最多落一个 u16（b_v 恒含至少一个有效 u16）
            if (b_v) begin
                fifo_mem[fifo_wp[5:0]] <= b_data[16*b_k +: 16];
                fifo_wp <= fifo_wp + 7'd1;
                b_k     <= b_k + 3'd1;
                if (b_k == b_last) b_v <= 1'b0;
            end
        end
    end

endmodule
`default_nettype wire
