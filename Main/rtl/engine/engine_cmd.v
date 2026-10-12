`timescale 1ns/1ps
//==============================================================================================//
// FILE: engine_cmd.v
//
// DESCRIPTION: 命令层：从搬运层取命令表（每批 16 条、两批乒乓、跨 4KB 拆描述符），
//   每条 32 字节记录做十步检查，FILL/BLIT/BLIT_INST 串行下发几何层，
//   NOP/SET_CLIP/END 就地处理，错误按「码/表内索引/实例/tag」上报寄存器层。
//   另含结构谓词 ec_struct_ok 与字段步 ec_fields（检查逻辑权威 sw/lib/gfx/check.c）。
//
// NOTE:
//   1. 非空命令表要求 LIST_BASE 按 32 字节对齐，违例取第 0 条前报 FETCH
//   2. 记录地址按 38 位求值；越过地址空间的记录不取不回卷，逻辑执行需要时报 FETCH
//   3. 预取只缓存：当前批先遇 END 或错误时丢弃预取结果，预取的 SLVERR 不上报
//   4. 一次只下发一条 FILL/BLIT；前一条完成后才检查/提交下一条（构造保证错误有序）
//   5. 读状态按搬运层显式来源位分发：取数来源归取数引擎，像素/TEX 读恒归活动命令
//   6. v1-TEX：BLIT 源允许 space=1，目的必须 DDR；源结构多查对齐与 16KiB 界
//==============================================================================================//
`default_nettype none
`include "engine_regs_offsets.vh"

// 结构谓词：末像素 base + ((h-1)<<stride_log2) + (w-1)*4 + 3 ≤ 0xFFFFFFFF。
// 行偏移最多 47 位，不能截断后再判——33 位截断会把「和的第 32 位恰好回到 0」
// 的越界误判为合法。行偏移高 16 位非 0 直接越界；否则 34 位相加看高 2 位。
module ec_struct_ok(
    input  wire [31:0] s0,   // base
    input  wire [31:0] s1,   // {height, width}
    input  wire [31:0] s2,   // 控制字：[31] valid、[15:12] format、[9:8] space、[4:0] stride_log2
    output wire        ok
);
    wire [15:0] w  = s1[15:0];
    wire [15:0] h  = s1[31:16];
    wire [4:0]  sl = s2[4:0];

    wire base_al = (s0[1:0] == 2'b00);
    wire wh_nz   = (w != 16'd0) && (h != 16'd0);
    wire sl_ok   = (sl >= 5'd2);
    // w*4 <= 1<<sl ⟺ w <= 2^(sl-2)；sl-2 ≥ 16 时上限超出 16 位宽，恒成立
    wire [4:0] t         = sl - 5'd2;
    wire       stride_ok = (t >= 5'd16) || (w <= (16'd1 << t));
    wire [15:0] hm     = h - 16'd1;
    wire [47:0] rowoff = {32'd0, hm} << sl;
    wire        hi_ovf = (rowoff[47:32] != 16'd0);
    wire [15:0] wm     = w - 16'd1;
    wire [33:0] lowsum = {2'b00, s0} + {18'd0, rowoff[31:0]} + {16'd0, wm, 2'b11};
    wire        no_ovf = !hi_ovf && (lowsum[33:32] == 2'b00);
    wire        ctrl_ok = ((s2 & 32'h7fff0ce0) == 32'b0);
    // TEX 结构检查对源、目的一致；合法结构的 TEX 目的随后由 space 步拒绝。
    wire [46:0] tex_size = {31'd0, h} << sl;
    wire [47:0] tex_end = {16'd0, s0} + {1'b0, tex_size};
    wire tex_ok = (s2[9:8] != 2'd1) ||
                  ((s0[3:0] == 4'd0) && (tex_end <= 48'h4000));
    assign ok = s2[31] && base_al && wh_nz && sl_ok && stride_ok && no_ovf && ctrl_ok && tex_ok;
endmodule

// 字段步：按操作码的零字段矩阵 + w0[15:14]、w0[13]（tint）、w6 三句散文
module ec_fields(
    input  wire [255:0] cmd,
    output wire         bad
);
    wire [7:0] op  = cmd[7:0];
    wire [3:0] eq  = cmd[11:8];
    wire       key = cmd[12];

    wire is_nop  = (op == 8'h00);
    wire is_fill = (op == 8'h01);
    wire is_blit = (op == 8'h02);
    wire is_clip = (op == 8'h03);
    wire is_end  = (op == 8'hff);

    wire w1_nz = (cmd[63:32]   != 32'b0);
    wire w2_nz = (cmd[95:64]   != 32'b0);
    wire w3_nz = (cmd[127:96]  != 32'b0);
    wire w4_nz = (cmd[159:128] != 32'b0);
    wire w5_nz = (cmd[191:160] != 32'b0);
    wire w6_nz = (cmd[223:192] != 32'b0);
    wire c_nz  = (cmd[31:24]   != 8'b0);
    wire src_nz = (cmd[19:16]  != 4'b0);
    wire dst_nz = (cmd[23:20]  != 4'b0);
    wire eq_nz  = (eq != 4'b0);
    wire rsvd_nz = (cmd[15:14] != 2'b0);

    wire const_ok = (eq == 4'd3);   // 仅 CONST 允许 c 非零

    wire z_nop_end = eq_nz || key || src_nz || dst_nz || c_nz
                   || w1_nz || w2_nz || w3_nz || w4_nz || w5_nz;
    wire z_fill    = key || src_nz || w1_nz || w5_nz || (!const_ok && c_nz);
    wire z_blit    = w4_nz || (!const_ok && c_nz) || (!key && w5_nz);
    wire z_clip    = eq_nz || key || src_nz || dst_nz || c_nz
                   || w2_nz || w4_nz || w5_nz;

    // tint 位对所有操作码都必须为 0（check.c 的 must_zero_mask 恒含 F_TINT）
    assign bad = rsvd_nz || cmd[13] || w6_nz
              || ((is_nop || is_end) && z_nop_end)
              || (is_fill && z_fill)
              || (is_blit && z_blit)
              || (is_clip && z_clip);
endmodule

module engine_cmd #(parameter integer STRIP_H = 16) (
    input  wire        clk,
    input  wire        rst,           // 控制层复位（soc_system_reset），同步

    // 寄存器层（engine_regs）
    input  wire        kick,          // 一拍：DOORBELL 受理
    input  wire        external_mode,  // 一拍：外部直投单条命令（PB 执行器）
    input  wire [255:0] external_packet,
    input  wire [63:0] external_clip,
    input  wire        preserve_clip,  // 排空返回时保住旧 clip（PB abort 用）
    input  wire [31:0] list_base,
    input  wire [31:0] list_count,
    input  wire        band_mode,
    input  wire [31:0] lstrips,
    input  wire [31:0] strip_log2,
    input  wire [31:0] band_surfs,
    output wire        band_enable,
    output wire        band_start,
    output wire        band_flush,
    output wire        band_abort,
    output reg  [31:0] band_bg_base,
    output reg  [31:0] band_dst_base,
    output reg  [4:0]  band_bg_slog2,
    output reg  [4:0]  band_dst_slog2,
    output wire [15:0] band_y0,
    output reg  [15:0] band_frame_height,
    input  wire        band_start_ready,
    input  wire        band_flush_ready,
    input  wire        band_loaded,
    input  wire        band_flushed,
    input  wire        band_busy,
    input  wire        band_error,
    input  wire [3:0]  band_error_code,
    input  wire        drain_done,
    input  wire        soft_rst,      // 电平：软复位请求举着，立即弃单回空闲
    output wire        busy,          // 已受理的命令表在跑（含收尾排空）
    output reg         err_set,       // 一拍：错误四元组同拍有效
    output reg  [3:0]  err_code,
    output reg  [31:0] err_index,     // 完整表内零基索引（低 4 位上下文在这里重建）
    output reg  [31:0] err_tag,
    output reg         done_set,      // 一拍：整表正常完成
    output reg         commit,        // 一拍：一条命令完成（PERF_CMD；不含 END 与出错命令）
    output wire        suppress_writes, // 活动命令总线错误后，余下写流只发零 strobe

    // 表面表读口（engine_regs 的组合读口，下标来自当前记录）
    output wire [3:0]  surf_ra_id,
    output wire [3:0]  surf_rb_id,
    input  wire [31:0] surf_ra_w0,
    input  wire [31:0] surf_ra_w1,
    input  wire [31:0] surf_ra_w2,
    input  wire [31:0] surf_rb_w0,
    input  wire [31:0] surf_rb_w1,
    input  wire [31:0] surf_rb_w2,

    // 搬运层：命令取数
    output wire        fd_instance,
    input  wire        fs_instance,
    input  wire        rd_stat_instance,
    input  wire        tmo_instance,
    output reg [31:0]  err_instance,
    output reg         perf_instance,
    output wire [31:0] fd_addr,
    output wire [11:0] fd_bytes,
    output wire [3:0]  fd_cidx,
    output wire        fd_valid,
    input  wire        fd_ready,
    input  wire [127:0] fs_tdata,
    input  wire        fs_tlast,
    input  wire        fs_tvalid,
    output wire        fs_tready,
    input  wire [3:0]  fs_cidx,

    // 搬运层：状态与超时
    input  wire        rd_stat_valid,
    input  wire        rd_stat_fetch,  // 显式来源：1 = FD；实例通路另按 rd_stat_instance 分流
    input  wire        rd_stat_err,
    input  wire [3:0]  rd_stat_cidx,
    input  wire        wr_stat_valid,
    input  wire        wr_stat_err,
    input  wire [3:0]  wr_stat_cidx,
    input  wire        wr_stat_last_row,
    input  wire        tmo_evt,

    // 几何层：命令下发与完成
    output reg         start,
    output reg  [7:0]  i1_op,
    output reg  [3:0]  i1_eq,
    output reg         i1_key_en,
    output reg  [31:0] i1_key,
    output reg  [31:0] i1_w4,
    output reg  [7:0]  i1_c,
    output reg  [3:0]  i1_sfmt,
    output reg  [31:0] i1_src_base,
    output reg         i1_src_tex,    // 源在片上 TEX
    output reg  [15:0] i1_src_w,
    output reg  [15:0] i1_src_h,
    output reg  [4:0]  i1_src_slog2,
    output reg  [31:0] i1_dst_base,
    output reg  [15:0] i1_dst_w,
    output reg  [15:0] i1_dst_h,
    output reg  [4:0]  i1_dst_slog2,
    output reg signed [15:0] i1_src_x,
    output reg signed [15:0] i1_src_y,
    output reg signed [15:0] i1_dst_x,
    output reg signed [15:0] i1_dst_y,
    output reg [15:0]  i1_w,
    output reg [15:0]  i1_h,
    output reg signed [15:0] i1_clip_x0,
    output reg signed [15:0] i1_clip_y0,
    output reg signed [15:0] i1_clip_x1,
    output reg signed [15:0] i1_clip_y1,
    output reg [3:0]   i1_cidx,
    input  wire        geom_busy,
    input  wire        geom_cmd_done,
    input  wire [3:0]  geom_done_cidx,

    // 仿真观测（综合悬空）
    output wire        fe_cidx_err    // 取数流上下文低 4 位与期望不符（一致性监视）
);
    localparam [7:0] OP_NOP  = 8'h00;
    localparam [7:0] OP_FILL = 8'h01;
    localparam [7:0] OP_BLIT = 8'h02;
    localparam [7:0] OP_CLIP = 8'h03;
    localparam [7:0] OP_END  = 8'hff;

    // 注错开关见 engine-manual.md。

    // 裁剪框：复位与软复位恢复全游戏区 [0,639]×[0,479]（闭区间）
    reg signed [15:0] clip_x0, clip_y0, clip_x1, clip_y1;
    reg external_r;

    // 命令缓冲：两批乒乓，64 × 128 位，一写口（取数流）一读口（执行）
    reg [127:0] cbuf [0:63];
    reg [127:0] cbuf_rdata;
    wire        fs_fire = fs_tvalid && fs_tready && !(fs_instance === 1'b1);
    // 槽位 = 半区 ×32 + 批内拍号（rq_off+fe_tot 逐字定义在 5 位内：批 ≤32 拍、
    // 逐条回退 off ≤30 加 2 拍也 ≤31，自然回卷等价 (off+tot) mod 32）
    wire [4:0]  cbuf_walo = rq_off + fe_tot[4:0];
    wire [5:0]  cbuf_wa   = {rq_half, cbuf_walo};
    wire [5:0] cbuf_ra = (st == S_HADDR || st == S_HWAIT || st == S_HREAD)
                      ? {3'd0, hdr_beat} : {half, k[3:0], rd_word};

    always @(posedge clk) begin
        if (fs_fire) cbuf[cbuf_wa] <= fs_tdata;
        cbuf_rdata <= cbuf[cbuf_ra];
    end

    // 取数请求：主状态机先摆好 rq_*，再举一拍 fe_start_q；FE 空闲时受理
    reg [31:0] rq_addr;     // 批首地址（32 字节对齐、在地址空间内）
    reg [4:0]  rq_n;        // 记录数（0 = 直接完成）
    reg        rq_half;     // 写入哪一批半区
    reg [4:0]  rq_off;      // 起始拍偏移（逐条回退指向 k 的槽位）
    reg [3:0]  rq_ctx;      // 描述符上下文（批首/记录号的低 4 位）
    reg        rq_pf;       // 本请求是预取批（fe_owner 的归属）
    reg        pf_armed;    // 预取请求已摆好，等 FE 空闲再发脉冲

    // 批长的地址截断：32 位 LIST_COUNT × 32 字节加 32 位基址需 38 位。
    function [4:0] calc_n(input [31:0] bs, input [31:0] bidx, input [31:0] cnt);
        reg [37:0] a0, sp;
        reg [32:0] remain;
        reg [4:0]  nc, na;
        begin
            a0 = {6'd0, bs} + {1'b0, bidx, 5'd0};
            remain = {1'b0, cnt} - {1'b0, bidx};
            nc = (remain >= 33'd16) ? 5'd16 : remain[4:0];
            if (a0 > 38'h00_FFFF_FFE0) begin       // addr0 + 31 越过 2^32-1
                calc_n = 5'd0;
            end else begin
                sp = 38'h01_0000_0000 - a0;
                na = (sp >= 38'd512) ? 5'd16 : sp[9:5];
                calc_n = (nc < na) ? nc : na;
            end
        end
    endfunction

    // 取数引擎 FE：把 rq_n 条记录取进 rq_half 半区；也服务逐条回退（n=1）
    localparam [1:0] F_IDLE = 2'd0, F_ISSUE = 2'd1, F_POLL = 2'd2, F_PULSE = 2'd3;
    reg [1:0]  fe_st;
    reg [1:0]  fe_d;        // 当前描述符（0/1，跨 4KB 拆分）
    reg [5:0]  fe_bd_exp, fe_bd_rcv;   // 当前描述符的拍数期望/已收（满批 32 拍要 6 位）
    reg [5:0]  fe_tot;      // 本批已收总拍数
    reg        fe_stat;     // 当前描述符的状态已见
    reg        fe_err;
    reg        fe_start_q;
    reg [1:0]  fe_owner;    // 0 当前批（主状态机在等）、1 预取批
    reg        fe_cidx_err_r;

    wire fe_ready = (fe_st == F_IDLE);
    wire fe_pulse = fe_start_q && fe_ready;

    // 拆分：批首 32 字节对齐 → 第一笔装本页剩余的整条记录（0x1000-off)>>5 = 128-off/32）
    wire [11:0] rq_pgoff = rq_addr[11:0];
    wire [7:0]  rq_first = (rq_pgoff == 12'd0) ? 8'd128 : 8'd128 - {1'b0, rq_pgoff[11:5]};
    wire [4:0]  rq_n1    = ({1'b0, rq_first} >= {1'b0, rq_n}) ? rq_n : rq_first[4:0];
    wire        rq_split = (rq_n1 != rq_n);
    // 第二笔起点：addr + n1*32。n1 是 5 位，左移 5 位后占 10 位，补零到 32 位相加——
    // 原来写成 {27'd0, rq_n1, 5'd0}（拼接和 37 位）再截回 32 位，数值一样但报 VERI-WARNING 截断。
    wire [31:0] rq_a1    = rq_addr + {22'd0, rq_n1, 5'd0};

    // 实例取数与命令取数共用一条 FD 流：批量取数期间 FE 持有所有权
    wire        inst_fd_valid, inst_fd_ready, inst_fs_ready, inst_busy, inst_valid, inst_error;
    wire [31:0] inst_fd_addr, inst_xy, inst_uv, inst_next_id;
    wire [11:0] inst_fd_bytes;
    wire [3:0]  inst_fd_cidx, inst_error_code;
    reg         inst_go, inst_active;
    reg [31:0]  inst_id;
    reg [15:0]  inst_count;
    reg [255:0] inst_packet;
    wire command_owner = fe_st == F_ISSUE;
    assign fd_instance = !command_owner && inst_fd_valid;
    assign inst_fd_ready = fd_ready && !command_owner;
    engine_inst u_inst (
        .clk(clk), .rst(rst || soft_rst), .cancel(pend_err || soft_rst), .go(inst_go),
        .base(inst_packet[63:32]), .count(inst_packet[79:64]), .stride(inst_packet[168:160]),
        .layout(inst_packet[176]), .fixed_uv(inst_packet[223:192]), .context_id(k[3:0]),
        .busy(inst_busy),
        .fd_addr(inst_fd_addr), .fd_bytes(inst_fd_bytes), .fd_cidx(inst_fd_cidx),
        .fd_valid(inst_fd_valid), .fd_ready(inst_fd_ready),
        .fs_data(fs_tdata), .fs_valid(fs_tvalid && (fs_instance === 1'b1)),
        .fs_last(fs_tlast), .fs_cidx(fs_cidx), .fs_ready(inst_fs_ready),
        .stat_valid(rd_stat_valid && (rd_stat_instance === 1'b1)),
        .stat_error(rd_stat_err), .stat_cidx(rd_stat_cidx),
        .valid(inst_valid), .ready(st == S_INEXT && !inst_error),
        .xy(inst_xy), .uv(inst_uv), .instance_id(inst_next_id),
        .error(inst_error), .error_code(inst_error_code));
    assign fd_addr = command_owner ? ((fe_d == 2'd0) ? rq_addr : rq_a1) : inst_fd_addr;
    assign fd_bytes = command_owner ? ({7'd0, (fe_d == 0) ? rq_n1 : (rq_n - rq_n1)} << 5) : inst_fd_bytes;
    assign fd_cidx = command_owner ? rq_ctx : inst_fd_cidx;
    assign fd_valid = (command_owner || inst_fd_valid) && !soft_rst;
    assign fs_tready = ((fs_instance === 1'b1) ? inst_fs_ready : (fe_st == F_POLL)) && !soft_rst;
    wire fe_done     = (fe_st == F_PULSE);      // 一拍，负载 fe_err
    wire fe_busy_any = (fe_st != F_IDLE);
    assign fe_cidx_err = fe_cidx_err_r;

    // 当前描述符收尾：最后一拍已握手且状态已见（先后都支持）；状态必须带取数来源
    wire bd_cnt_hit = ({1'b0, fe_bd_rcv} + 6'd1 == {1'b0, fe_bd_exp});
    wire fe_stat_in = rd_stat_valid && rd_stat_fetch && !(rd_stat_instance === 1'b1);
    wire desc_done  = (fs_fire && bd_cnt_hit && (fe_stat || fe_stat_in))
                   || (fe_stat_in && !fs_fire && ({1'b0, fe_bd_rcv} == {1'b0, fe_bd_exp}));

    always @(posedge clk) begin
        if (rst || soft_rst) begin
            fe_st         <= F_IDLE;
            fe_err        <= 1'b0;
            fe_cidx_err_r <= 1'b0;
            fe_owner      <= 2'd0;
        end else begin
            if (fe_pulse) fe_owner <= rq_pf;
            else if (fe_done) fe_owner <= 2'd0;

            case (fe_st)
                F_IDLE: if (fe_pulse) begin
                    fe_d      <= 2'd0;
                    fe_bd_rcv <= 5'd0;
                    fe_tot    <= 6'd0;
                    fe_stat   <= 1'b0;
                    fe_err    <= 1'b0;
                    fe_st     <= (rq_n == 5'd0) ? F_PULSE : F_ISSUE;
                end
                F_ISSUE: if (fd_ready) begin
                    fe_bd_exp <= (fe_d == 2'd0) ? {1'b0, rq_n1} << 1
                                                : {1'b0, rq_n - rq_n1} << 1;
                    fe_bd_rcv <= 5'd0;
                    fe_stat   <= 1'b0;
                    fe_st     <= F_POLL;
                end
                F_POLL: begin
                    if (fs_fire) begin
                        fe_tot    <= fe_tot + 6'd1;
                        fe_bd_rcv <= fe_bd_rcv + 5'd1;
                        if (fs_cidx != rq_ctx) fe_cidx_err_r <= 1'b1;
                    end
                    if (fe_stat_in) begin
                        fe_stat <= 1'b1;
                        if (rd_stat_err) fe_err <= 1'b1;
                    end
                    if (desc_done) begin
                        if (rq_split && (fe_d == 2'd0)) begin
                            fe_d  <= 2'd1;
                            fe_st <= F_ISSUE;
                        end else begin
                            fe_st <= F_PULSE;
                        end
                    end
                end
                default: fe_st <= F_IDLE;   // F_PULSE：一拍完成脉冲
            endcase
        end
    end

    // 主状态机
    localparam [4:0] S_IDLE=0, S_FETCH=1, S_RD0=2, S_RD1=3, S_RUN=4,
        S_ISSUE=5, S_ACTIVE=6, S_ADV=7, S_SWITCH=8, S_PERW=9, S_PER_WAIT=10,
        S_TERM=12, S_EXEC=13, S_RD2=14, S_CFG=15, S_HDR=16, S_HADDR=17,
        S_HWAIT=18, S_HREAD=19, S_BNEXT=20, S_BSTART=21, S_BLOAD=22,
        S_BFLUSH=23, S_BWAIT=24, S_CFG_WAIT=25, S_DISPATCH=26, S_INEXT=27;
    reg [4:0] st;
    reg bmode, cfg_phase, cfg_bad_q;
    reg [4:0] chk_q;
    reg [4:0] strip_id;
    reg [5:0] strips_r;
    reg [3:0] bg_id, dst_id;
    reg [31:0] table_base, counts [0:31];
    reg [2:0] hdr_beat;
    assign band_enable = bmode && busy_r;
    assign band_y0 = {11'd0, strip_id} * STRIP_H;
    assign band_start = st == S_BSTART && !soft_rst;
    assign band_flush = st == S_BFLUSH && !soft_rst;
    assign band_abort = st == S_TERM && pend_err && bmode &&
                        !active && !geom_busy && !fe_busy_any;
    wire [31:0] global_k = bmode ? {17'd0, strip_id, 10'd0} + k : k;
    wire [31:0] global_bi = bmode ? {17'd0, strip_id, 10'd0} + bi : bi;
    wire [31:0] global_nbi = bmode ? {17'd0, strip_id, 10'd0} + n_bi : n_bi;

    reg        busy_r;
    reg [31:0] base_r;
    reg [31:0] count_r;
    reg [31:0] bi, k;          // 批首索引、当前记录索引
    reg [4:0]  bn;             // 当前批的记录数（含地址截断；可大于 16 吗——calc_n ≤ 16）
    reg        half;           // 当前批在 cbuf 的半区
    reg        per;            // 逐条回退模式
    reg [127:0] w0, w1;        // 当前记录的低/高 128 位（w0 = 字 0..3）
    reg [31:0] tag_cur;        // 活动命令的 w7
    reg        active;         // FILL/BLIT 已下发未完成
    reg        rd_word;        // cbuf 读口字选
    reg        pend_err, pend_done;
    // 完成语义（驱动契约：DONE/ERR 置位即引擎已空闲，驱动可立即读 PERF）：
    // 检出时只挂 pending，等在途命令收尾、预取流收完（S_TERM）才置 done_set/err_set。

    // 预取槽
    localparam [2:0] N_IDLE = 3'd0, N_INFL = 3'd1, N_OK = 3'd2, N_ERR = 3'd3;
    reg [2:0]  nst;
    reg [31:0] n_bi;
    reg [4:0]  n_bn;
    reg        n_half;

    assign busy = busy_r;
    assign suppress_writes = pend_err && active;

    // 逐条回退的当前记录地址是否可取（越过 32 位空间不取不回卷）
    wire [37:0] k_a0 = {6'd0, base_r} + {1'b0, k, 5'd0};
    wire        k_ok = (k_a0 <= 38'h00_FFFF_FFE0);

    // 读状态按搬运层给的显式来源位分发：带取数来源的归取数引擎，
    // 其余（DDR 像素读与 TEX 本地读）归活动命令的像素通路。

    // 十步检查：单拍组合，逻辑权威 sw/lib/gfx/check.c；第一步取记录归取数状态机
    wire [255:0] cmd_full = {w1, w0};
    wire [7:0]   c_op     = cmd_full[7:0];
    wire [3:0]   c_eq     = cmd_full[11:8];
    wire [3:0]   c_srcid  = cmd_full[19:16];
    wire [3:0]   c_dstid  = cmd_full[23:20];

    assign surf_ra_id = cfg_phase ? bg_id : c_srcid;
    assign surf_rb_id = cfg_phase ? dst_id : c_dstid;

    wire is_fill_c = (c_op == OP_FILL);
    wire is_inst_c = (c_op == 8'h05);
    wire is_blit_c = (c_op == OP_BLIT) || is_inst_c;
    wire op_ok_c   = (c_op == OP_NOP) || is_fill_c || is_blit_c
                   || (c_op == OP_CLIP) || (c_op == OP_END);

    wire scalar_f_bad, f_bad_c;
    ec_fields u_fields(.cmd(cmd_full), .bad(scalar_f_bad));
    wire [15:0] inst_stride = cmd_full[175:160];
    wire [1:0]  inst_layout = cmd_full[177:176];
    wire [24:0] inst_length = (cmd_full[79:64] == 0) ? 25'd0 :
        ({9'd0, cmd_full[79:64]} - 25'd1) * {16'd0, inst_stride[8:0]} + (inst_layout == 0 ? 25'd4 : 25'd8);
    wire [32:0] inst_limit = {1'b0, cmd_full[63:32]} + (({8'd0, inst_length} + 33'd15) & ~33'd15);
    // 实例包专用字段检查，与 engine_packet_check 同口径
    wire inst_fields_bad = cmd_full[15:13] != 0 || cmd_full[63:32] % 16 != 0 ||
        cmd_full[95:80] != 0 || inst_layout > 1 || cmd_full[191:178] != 0 ||
        inst_stride < (inst_layout == 0 ? 16'd4 : 16'd8) || inst_stride > 256 || inst_stride[1:0] != 0 ||
        (inst_layout == 1 && cmd_full[223:192] != 0) ||
        (!cmd_full[12] && cmd_full[159:128] != 0) ||
        (c_eq != 3 && cmd_full[31:24] != 0) || inst_limit > 33'h100000000;
    assign f_bad_c=is_inst_c?inst_fields_bad:scalar_f_bad;
    wire eq_bad_c = (c_eq > 4'd5);

    wire src_ok_c, dst_ok_c;
    ec_struct_ok u_ssrc(.s0(surf_ra_w0), .s1(surf_ra_w1), .s2(surf_ra_w2), .ok(src_ok_c));
    ec_struct_ok u_sdst(.s0(surf_rb_w0), .s1(surf_rb_w1), .s2(surf_rb_w2), .ok(dst_ok_c));

    // v1-TEX：BLIT 源放行 space=1，其余非 DDR 仍报 BAD_SPACE
    wire [1:0] src_space_c = surf_ra_w2[9:8];
    wire sp_src_bad = is_blit_c && (src_space_c == 2'd2 || src_space_c == 2'd3);
    wire sp_dst_bad = (is_fill_c || is_blit_c) && (surf_rb_w2[9:8] != 2'd00);
    wire sp_bad_c   = sp_src_bad || sp_dst_bad;

    wire src_bad_c = is_blit_c && !src_ok_c;
    wire dst_bad_c = (is_fill_c || is_blit_c) && !dst_ok_c;

    wire fmt_src_ok  = (c_eq == 4'd1) ? (surf_ra_w2[15:12] == 4'd0 || surf_ra_w2[15:12] == 4'd1)
                     : (c_eq == 4'd5) ? (surf_ra_w2[15:12] == 4'd0 || surf_ra_w2[15:12] == 4'd2)
                     :                  (surf_ra_w2[15:12] <= 4'd2);
    wire fmt_src_bad = is_blit_c && !fmt_src_ok;
    wire fmt_dst_bad = (is_fill_c || is_blit_c) && (surf_rb_w2[15:12] != 4'd0);
    wire fmt_bad_c   = fmt_src_bad || fmt_dst_bad;

    wire [7:0] a_const = cmd_full[159:152];    // w4[31:24]
    wire premult_bad = is_fill_c && (c_eq == 4'd1)
                    && (cmd_full[151:144] > a_const
                     || cmd_full[143:136] > a_const
                     || cmd_full[135:128] > a_const);

    // 第十步：同表面 BLIT 的矩形重叠（未裁剪半开区间；18 位有符号足够）
    wire [15:0] ow = cmd_full[111:96];
    wire [15:0] oh = cmd_full[127:112];
    wire [17:0] sx18 = {{2{cmd_full[47]}}, cmd_full[47:32]};
    wire [17:0] sy18 = {{2{cmd_full[63]}}, cmd_full[63:48]};
    wire [17:0] dx18 = {{2{cmd_full[79]}}, cmd_full[79:64]};
    wire [17:0] dy18 = {{2{cmd_full[95]}}, cmd_full[95:80]};
    wire ovl_bad_c = (c_op==OP_BLIT) && (c_srcid == c_dstid)
                  && (ow != 16'd0) && (oh != 16'd0)
                  && ($signed(sx18 + {2'b00, ow}) > $signed(dx18))
                  && ($signed(dx18 + {2'b00, ow}) > $signed(sx18))
                  && ($signed(sy18 + {2'b00, oh}) > $signed(dy18))
                  && ($signed(dy18 + {2'b00, oh}) > $signed(sy18));

    // 优先链（注错位 0：对调 space/format 两步，验收必须见红）
    // v2 表面反馈不能因分桶而改变读到的画面；按完整 backing 区间拒绝别名。
    wire [47:0] src_end = {16'd0,surf_ra_w0} +
        (({32'd0,surf_ra_w1[31:16]}-48'd1) << surf_ra_w2[4:0]) +
        {30'd0,surf_ra_w1[15:0],2'd0};
    wire [47:0] dst_end = {16'd0,band_dst_base} +
        (({32'd0,band_frame_height}-48'd1) << band_dst_slog2) + 48'd2560;
    wire feedback_bad = bmode && is_blit_c && src_space_c == 0 &&
        {16'd0,surf_ra_w0} < dst_end && {16'd0,band_dst_base} < src_end;
    wire local_bad = bmode && ((c_op == OP_CLIP) || (c_op == OP_END) ||
        ((is_fill_c || is_blit_c) && (c_dstid != dst_id ||
         w0[95] || w0[95:80] >= STRIP_H ||
         ({1'b0,w0[95:80]} + {1'b0,w0[127:112]}) > STRIP_H)));
    wire [47:0] cfg_bg_end = {16'd0,surf_ra_w0} +
        (({32'd0,surf_ra_w1[31:16]}-48'd1) << surf_ra_w2[4:0]) + 48'd2560;
    wire [47:0] cfg_dst_end = {16'd0,surf_rb_w0} +
        (({32'd0,surf_rb_w1[31:16]}-48'd1) << surf_rb_w2[4:0]) + 48'd2560;
    wire cfg_alias_bad = {16'd0,surf_ra_w0}<cfg_dst_end &&
        {16'd0,surf_rb_w0}<cfg_bg_end &&
        (surf_ra_w0 != surf_rb_w0 || surf_ra_w2[4:0] != surf_rb_w2[4:0]);
    wire cfg_bad = !src_ok_c || !dst_ok_c || surf_ra_w0[3:0]!=0 || surf_rb_w0[3:0]!=0 ||
        surf_ra_w1[15:0]!=640 || surf_rb_w1[15:0]!=640 ||
        surf_ra_w1[31:16]!=surf_rb_w1[31:16] ||
        {16'd0,surf_rb_w1[31:16]} != strips_r*STRIP_H ||
        surf_rb_w1[31:16]>480 || surf_ra_w2[9:8]!=0 || surf_rb_w2[9:8]!=0 ||
        surf_ra_w2[15:12]!=0 || surf_rb_w2[15:12]!=0 || cfg_alias_bad;
    wire [4:0] v_norm = (!op_ok_c || (bmode && is_inst_c))    ? {1'b1, EC_ERR_BAD_OPCODE}
                      : f_bad_c     ? {1'b1, EC_ERR_BAD_FIELD}
                      : eq_bad_c    ? {1'b1, EC_ERR_BAD_EQUATION}
                      : src_bad_c   ? {1'b1, EC_ERR_BAD_SURFACE}
                      : dst_bad_c   ? {1'b1, EC_ERR_BAD_SURFACE}
                      : sp_bad_c    ? {1'b1, EC_ERR_BAD_SPACE}
                      : fmt_bad_c   ? {1'b1, EC_ERR_BAD_FORMAT}
                      : premult_bad ? {1'b1, EC_ERR_BAD_PREMULT}
                      : (ovl_bad_c || feedback_bad) ? {1'b1, EC_ERR_OVERLAP}
                      : local_bad ? {1'b1, EC_ERR_BAD_FIELD}
                      :               {1'b0, EC_ERR_OK};
    wire [4:0] v_swap = !op_ok_c    ? {1'b1, EC_ERR_BAD_OPCODE}
                      : f_bad_c     ? {1'b1, EC_ERR_BAD_FIELD}
                      : eq_bad_c    ? {1'b1, EC_ERR_BAD_EQUATION}
                      : src_bad_c   ? {1'b1, EC_ERR_BAD_SURFACE}
                      : dst_bad_c   ? {1'b1, EC_ERR_BAD_SURFACE}
                      : fmt_bad_c   ? {1'b1, EC_ERR_BAD_FORMAT}
                      : sp_bad_c    ? {1'b1, EC_ERR_BAD_SPACE}
                      : premult_bad ? {1'b1, EC_ERR_BAD_PREMULT}
                      : ovl_bad_c   ? {1'b1, EC_ERR_OVERLAP}
                      :               {1'b0, EC_ERR_OK};
`ifdef INJ_CHK_SWAP
    wire [4:0] chk = v_swap;              // 注错：用对调 space/format 的错误码
`else
    wire [4:0] chk = v_norm;
`endif

    // SET_CLIP 的新框：x1 = x + w − 1。x 有符号、w 无符号，和的范围
    // [−32768, 98301]，用 18 位有符号求值再饱和到 16 位；17 位零扩展会把
    // 负原点 −4 抬成 65532，裁剪框永远不空
    wire [15:0] cw  = w0[111:96];
    wire [15:0] ch  = w0[127:112];
    // 三项各自先取成 18 位有符号再相加，不把拼接结果直接写进和式（位宽膨胀报截断）
    wire signed [17:0] cx0  = $signed({{2{w0[47]}}, w0[47:32]});
    wire signed [17:0] cw18 = $signed({2'b00, cw});
    wire signed [17:0] cy0  = $signed({{2{w0[63]}}, w0[63:48]});
    wire signed [17:0] ch18 = $signed({2'b00, ch});
    wire signed [17:0] cx1  = cx0 + cw18 - 18'sd1;
    wire signed [17:0] cy1  = cy0 + ch18 - 18'sd1;

    // 主时序
    integer i;
    always @(posedge clk) begin
        if (rst || soft_rst) begin
            st <= S_IDLE; busy_r <= 1'b0;
            external_r<=0;
            inst_go<=0;inst_active<=0;inst_id<=0;inst_count<=0;inst_packet<=0;
            err_instance<=32'hffffffff;perf_instance<=0;
            bmode<=0;cfg_phase<=0;cfg_bad_q<=0;chk_q<=0;strip_id<=0;strips_r<=0;bg_id<=0;dst_id<=0;hdr_beat<=0;
            band_bg_base<=0;band_dst_base<=0;band_bg_slog2<=0;band_dst_slog2<=0;
            band_frame_height<=0;
            err_set <= 1'b0; done_set <= 1'b0; commit <= 1'b0;
            start <= 1'b0; active <= 1'b0;
            bi <= 32'd0; k <= 32'd0; bn <= 5'd0; half <= 1'b0; per <= 1'b0;
            nst <= N_IDLE; n_bi <= 32'd0; n_bn <= 5'd0; n_half <= 1'b0;
            fe_start_q <= 1'b0; rd_word <= 1'b0;
            pf_armed <= 1'b0; rq_pf <= 1'b0;
            pend_err <= 1'b0; pend_done <= 1'b0;
            if(rst || !(preserve_clip === 1'b1))begin
                clip_x0 <= 16'sd0; clip_y0 <= 16'sd0;
                clip_x1 <= 16'sd639; clip_y1 <= 16'sd479;
            end
        end else begin
            err_set    <= 1'b0;
            done_set   <= 1'b0;
            commit     <= 1'b0;
            start      <= 1'b0;
            fe_start_q <= 1'b0;
            inst_go<=0;perf_instance<=0;

            // 预取请求等 FE 空闲再发（S_FETCH/S_SWITCH 出口那一拍 FE 可能还在 F_PULSE）
            if (pf_armed && fe_ready) begin
                pf_armed   <= 1'b0;
                fe_start_q <= 1'b1;
            end

            // 预取完成 → 预取槽落定：错误也落为 N_ERR，由 S_SWITCH 处理；
            // END/错误收尾只等流收完，丢弃结果、预取的 SLVERR 不上报
            if (fe_done && fe_owner == 2'd1)
                nst <= fe_err ? N_ERR : N_OK;

            case (st)
                // IDLE
                S_IDLE: if (kick) begin
                    external_r<=(external_mode === 1'b1);
                    inst_active<=0;err_instance<=32'hffffffff;
                    busy_r  <= 1'b1;
                    base_r  <= list_base;
                    count_r <= list_count;
                    bmode <= (band_mode === 1'b1);
                    cfg_phase <= (band_mode === 1'b1);
                    strip_id <= 0;
                    strips_r <= lstrips[5:0];
                    bg_id <= band_surfs[7:4]; dst_id <= band_surfs[3:0];
                    table_base <= list_base;
                    if(external_mode === 1'b1)begin
                        bmode<=0;cfg_phase<=0;count_r<=1;bi<=0;k<=0;bn<=1;nst<=N_IDLE;
                        pend_err<=0;pend_done<=0;pf_armed<=0;
                        w0<=external_packet[127:0];w1<=external_packet[255:128];st<=S_RUN;
                    end else if (band_mode === 1'b1) begin
                        if (list_base[14:0]!=0 || lstrips==0 || lstrips>30 ||
                            strip_log2!=$clog2(STRIP_H) || band_surfs[31:8]!=0 ||
                            ({1'b0,list_base}+((lstrips+33'd1)<<15))>33'h100000000) begin
                            pend_err<=1;err_code<=EC_ERR_BAD_FIELD;err_index<=0;err_tag<=0;st<=S_TERM;
                        end else st<=S_CFG;
                    end else if (list_count == 32'd0) begin
                        pend_done <= 1'b1;              // COUNT=0：直接成功
                        st        <= S_TERM;
                    end else if (list_base[4:0] != 5'd0) begin
                        pend_err  <= 1'b1;              // 非对齐：取第 0 条前报 FETCH
                        err_code  <= EC_ERR_FETCH;
                        err_index <= 32'd0;
                        err_tag   <= 32'd0;
                        st        <= S_TERM;
                    end else begin
                        bi   <= 32'd0;
                        k    <= 32'd0;
                        half <= 1'b0;
                        per  <= 1'b0;
                        bn   <= calc_n(list_base, 32'd0, list_count);
                        // 取第一批（calc_n 为 0 时第一批不可取，执行时报 FETCH）
                        rq_addr <= list_base;
                        rq_n    <= calc_n(list_base, 32'd0, list_count);
                        rq_half <= 1'b0;
                        rq_off  <= 5'd0;
                        rq_ctx  <= 4'd0;
                        rq_pf   <= 1'b0;
                        // 首批不可取（地址空间截断到 0）时不发请求：
                        // S_FETCH 会永远等不到 fe_done，直接进执行路径报 FETCH
                        if (calc_n(list_base, 32'd0, list_count) != 5'd0)
                            fe_start_q <= 1'b1;
                        st <= (calc_n(list_base, 32'd0, list_count) != 5'd0) ? S_FETCH : S_EXEC;
                    end
                end

                // 等当前批取数
                S_FETCH: if (tmo_evt) begin
                    // 取数方向超过门限：只报告不放弃；迟到响应到达后 S_TERM 收尾
                    pend_err  <= 1'b1;
                    err_code  <= EC_ERR_TIMEOUT;
                    err_index <= global_bi;
                    err_tag   <= 32'd0;
                    st        <= S_TERM;
                end else if (fe_done && fe_owner == 2'd0) begin
`ifdef INJ_NO_FB
                    if (fe_err) begin
                        err_set   <= 1'b1;              // 注错：关掉逐条回退
                        err_code  <= EC_ERR_FETCH;
                        err_index <= global_bi;
                        err_tag   <= 32'd0;
                        st        <= S_TERM;
                    end else if (fe_err) begin
`else
                    if (fe_err) begin
`endif
                        k   <= bi;                      // 整批失败：从批首逐条回退
                        per <= 1'b1;
                        st  <= S_PERW;
                    end else begin
                        // 触发下一批预取（rq_* 此时可改：FE 已空闲）
                        if ({1'b0, bi} + {1'b0, bn} < {1'b0, count_r}) begin
                            n_bi   <= bi + bn;
                            n_bn   <= calc_n(base_r, bi + bn, count_r);
                            n_half <= ~half;
                            nst    <= N_INFL;
                            if (calc_n(base_r, bi + bn, count_r) != 5'd0) begin
                                rq_addr <= base_r + ((bi + bn) << 5);
                                rq_n    <= calc_n(base_r, bi + bn, count_r);
                                rq_half <= ~half;
                                rq_off  <= 5'd0;
                                rq_ctx  <= bi[3:0] + bn[3:0];   // (bi+bn) mod 16
                                rq_pf   <= 1'b1;
                                pf_armed <= 1'b1;   // FE 可能还在 F_PULSE，等空闲再发
                            end else begin
                                rq_pf <= 1'b0;
                                nst   <= N_OK;      // 下一批不可取：执行到那再报 FETCH
                            end
                        end else begin
                            nst <= N_IDLE;
                        end
                        st <= S_EXEC;
                    end
                end

                // 读记录：EX 发地址 0、RD0 发地址 1，RD1/RD2 拍依次捕进 w0/w1
                S_EXEC: begin
                    rd_word <= 1'b0;
                    st      <= S_RD0;
                end
                S_RD0: begin
                    rd_word <= 1'b1;
                    st      <= S_RD1;
                end
                S_RD1: begin
                    w0 <= cbuf_rdata;
                    st <= S_RD2;
                end
                S_RD2: begin
                    w1 <= cbuf_rdata;
                    st <= S_RUN;
                end

                // 判定与就地处理
                S_RUN: begin chk_q<=chk;st<=S_DISPATCH;end
                S_DISPATCH: begin
                    if ({1'b0, k} >= {1'b0, bi} + {1'b0, bn}) begin
                        // 记录没有取到（地址空间截断）：逻辑执行需要它，报 FETCH
                        pend_err  <= 1'b1;
                        err_code  <= EC_ERR_FETCH;
                        err_index <= global_k;
                        err_tag   <= 32'd0;
                        st        <= S_TERM;
                    end else if (chk_q[4]) begin
                        pend_err  <= 1'b1;
                        err_code  <= chk_q[3:0];
                        err_instance<=inst_active?inst_id:32'hffffffff;
                        err_index <= global_k;
                        err_tag   <= w1[127:96];
                        st        <= S_TERM;
                    end else begin
                        case (c_op)
                            8'h05: begin
                                if(w0[95:64]==0)begin commit<=1;st<=S_ADV;end
                                else begin inst_packet<=cmd_full;inst_count<=w0[79:64];
                                           inst_active<=1;inst_go<=1;st<=S_INEXT;end
                            end
                            OP_NOP: begin
                                commit <= 1'b1;
                                st     <= S_ADV;
                            end
                            OP_CLIP: begin
                                clip_x0 <= $signed(w0[47:32]);
                                clip_y0 <= $signed(w0[63:48]);
                                if (cw == 16'd0 || ch == 16'd0) begin
                                    clip_x1 <= 16'sd0 - 16'sd1;     // 空矩形全裁
                                    clip_y1 <= 16'sd0 - 16'sd1;
                                end else begin
                                    clip_x1 <= (cx1 > 18'sd32767) ? 16'sd32767 : cx1[15:0];
                                    clip_y1 <= (cy1 > 18'sd32767) ? 16'sd32767 : cy1[15:0];
                                end
                                commit <= 1'b1;
                                st     <= S_ADV;
                            end
                            OP_END: begin
                                pend_done <= 1'b1;          // END 后不读取（spec §5）
                                st        <= S_TERM;
                            end
                            default: st <= S_ISSUE;         // FILL/BLIT
                        endcase
                    end
                end

                S_INEXT: begin
                    if(inst_error || tmo_evt)begin
                        pend_err<=1;err_code<=tmo_evt?EC_ERR_TIMEOUT:inst_error_code;
                        err_index<=global_k;err_tag<=inst_packet[255:224];
                        err_instance<=inst_next_id;st<=S_TERM;
                    end else if(inst_valid)begin
`ifdef INJ_INST_XYUV
                        w0<={inst_packet[127:96],inst_uv,inst_xy,inst_packet[31:8],8'h02};
`else
                        w0<={inst_packet[127:96],inst_xy,inst_uv,inst_packet[31:8],8'h02};
`endif
                        w1<={inst_packet[255:224],32'd0,inst_packet[159:128],32'd0};
                        inst_id<=inst_next_id;st<=S_RUN;
                    end
                end

                // 下发几何层
                S_ISSUE: if (!geom_busy && !active) begin
                    i1_op      <= w0[7:0];
                    i1_eq      <= w0[11:8];
                    i1_key_en  <= w0[12];
                    i1_key     <= w1[63:32];
                    i1_w4      <= w1[31:0];
                    i1_c       <= w0[31:24];
                    i1_sfmt    <= (w0[7:0] == OP_BLIT) ? surf_ra_w2[15:12] : 4'd0;
                    i1_src_base   <= surf_ra_w0;
                    i1_src_tex    <= (w0[7:0] == OP_BLIT) && (surf_ra_w2[9:8] == 2'd1);
                    i1_src_w      <= surf_ra_w1[15:0];
                    i1_src_h      <= surf_ra_w1[31:16];
                    i1_src_slog2  <= surf_ra_w2[4:0];
                    i1_dst_base   <= surf_rb_w0;
                    i1_dst_w      <= surf_rb_w1[15:0];
                    i1_dst_h      <= surf_rb_w1[31:16];
                    i1_dst_slog2  <= surf_rb_w2[4:0];
                    i1_src_x   <= $signed(w0[47:32]);
                    i1_src_y   <= $signed(w0[63:48]);
                    i1_dst_x   <= $signed(w0[79:64]);
                    `ifdef INJ_STRIP_LOCAL_Y
                    i1_dst_y <= $signed(w0[95:80]);
`else
                    i1_dst_y <= bmode ? $signed(w0[95:80] + band_y0) : $signed(w0[95:80]);
`endif
                    i1_w       <= w0[111:96];
                    i1_h       <= w0[127:112];
                    // PB 生产者保持 packet/clip 到 command_done，几何层在 start 锁存。
                    i1_clip_x0 <= external_r?external_clip[15:0]:(bmode ? 16'sd0 : clip_x0);
                    i1_clip_y0 <= external_r?external_clip[31:16]:(bmode ? 16'sd0 : clip_y0);
                    i1_clip_x1 <= external_r?external_clip[47:32]:(bmode ? 16'sd639 : clip_x1);
                    i1_clip_y1 <= external_r?external_clip[63:48]:(bmode ? 16'sd479 : clip_y1);
                    i1_cidx    <= k[3:0];
                    start      <= 1'b1;
                    perf_instance<=inst_active;
                    active     <= 1'b1;
                    tag_cur    <= w1[127:96];
                    st         <= S_ACTIVE;
                end

                // 等活动命令完成
                S_ACTIVE: begin
`ifdef INJ_INST_EARLY_DONE
                    if(inst_active && geom_busy)done_set<=1;
`endif
`ifdef INJ_INST_STATE
                    if(inst_active)inst_packet[127:96]<=0;
`endif
                    if (wr_stat_valid && wr_stat_err) begin
                        pend_err  <= 1'b1;
                        err_code  <= EC_ERR_BUS_WRITE;
                        err_index <= global_k;
                        err_tag   <= tag_cur;
                        err_instance<=inst_active?inst_id:32'hffffffff;
                        // 末行的写状态本身就是完成回报：错误与完成同拍时也要撤 active，
                        // 否则 S_TERM 等一个已经来过的 last_row，永远收不了尾
                        if (wr_stat_last_row)
                            active <= 1'b0;
                        st        <= S_TERM;
                    end else if (rd_stat_valid && rd_stat_err && !rd_stat_fetch) begin
                        pend_err  <= 1'b1;
                        err_code  <= EC_ERR_BUS_READ;
                        err_index <= global_k;
                        err_tag   <= tag_cur;
                        err_instance<=inst_active?inst_id:32'hffffffff;
                        st        <= S_TERM;
                    end else if (tmo_evt) begin
                        pend_err  <= 1'b1;
                        err_code  <= EC_ERR_TIMEOUT;
                        err_index <= global_k;
                        err_tag   <= tag_cur;
                        err_instance<=inst_active?((tmo_instance === 1'b1)?inst_next_id:inst_id):32'hffffffff;
                        st        <= S_TERM;
                    end else if (
                               (wr_stat_valid && wr_stat_last_row)
                              || (geom_cmd_done && geom_done_cidx == k[3:0])) begin
                        // 非空命令的末行写状态 / 空命令的 cmd_done
                        active<=0;
                        if(inst_active && inst_id+32'd1!={16'd0,inst_count})st<=S_INEXT;
                        else begin commit<=1;st<=S_ADV;end
                    end
                end

                // 推进
                S_ADV: begin
                    inst_active<=0;
                    k <= k + 32'd1;
                    if ({1'b0, k} + 33'd1 >= {1'b0, bi} + {1'b0, bn})
                        st <= S_SWITCH;
                    else
                        st <= per ? S_PERW : S_EXEC;
                end

                // 批切换
                S_SWITCH: begin
                    if ({1'b0, bi} + {1'b0, bn} >= {1'b0, count_r}) begin
                        if(bmode) st<=S_BFLUSH;
                        else begin pend_done<=1'b1;st<=S_TERM;end
                    end else if (per) begin
                        // 逐条回退消耗完失败批：整批取下一批，回到批量模式。
                        // k 在 S_ADV 已推进到 bi+bn = 下一批的批首。
                        per     <= 1'b0;
                        bn      <= calc_n(base_r, bi + bn, count_r);
                        rq_addr <= base_r + ((bi + bn) << 5);
                        rq_n    <= calc_n(base_r, bi + bn, count_r);
                        rq_half <= half;
                        rq_off  <= 5'd0;
                        rq_ctx  <= bi[3:0] + bn[3:0];           // (bi+bn) mod 16
                        rq_pf   <= 1'b0;
                        if (calc_n(base_r, bi + bn, count_r) != 5'd0) begin
                            fe_start_q <= 1'b1;     // FE 此时必空闲（本批刚收完）
                            st <= S_FETCH;
                        end else begin
                            st <= S_EXEC;           // 不可取：执行到那再报 FETCH
                        end
                        bi <= bi + bn;              // 批首推进（NBA 用旧 bn）
                    end else if (nst == N_OK) begin
                        half <= n_half;
                        bi   <= n_bi;
                        bn   <= n_bn;
                        k    <= n_bi;
                        // 再预取下一批
                        if ({1'b0, n_bi} + {1'b0, n_bn} < {1'b0, count_r}) begin
                            n_bi <= n_bi + n_bn;
                            n_bn <= calc_n(base_r, n_bi + n_bn, count_r);
                            n_half <= ~n_half;
                            nst <= N_INFL;
                            if (calc_n(base_r, n_bi + n_bn, count_r) != 5'd0) begin
                                rq_addr <= base_r + ((n_bi + n_bn) << 5);
                                rq_n    <= calc_n(base_r, n_bi + n_bn, count_r);
                                rq_half <= ~n_half;
                                rq_off  <= 5'd0;
                                rq_ctx  <= n_bi[3:0] + n_bn[3:0];   // (n_bi+n_bn) mod 16
                                rq_pf   <= 1'b1;
                                pf_armed <= 1'b1;
                            end else begin
                                rq_pf <= 1'b0;
                                nst   <= N_OK;
                            end
                        end else begin
                            nst <= N_IDLE;
                        end
                        st <= S_EXEC;
                    end else if (nst == N_ERR) begin
                        // 预取的下一批取数失败：从它的批首逐条回退
                        bi  <= n_bi;
                        bn  <= n_bn;
                        k   <= n_bi;
                        per <= 1'b1;
                        nst <= N_IDLE;
                        rq_pf <= 1'b0;
`ifdef INJ_NO_FB
                        pend_err  <= 1'b1;      // 注错：关掉逐条回退
                        err_code  <= EC_ERR_FETCH;
                        err_index <= global_nbi;
                        err_tag   <= 32'd0;
                        st        <= S_TERM;
`else
                        st <= S_PERW;
`endif
                    end
                    // N_INFL：等预取收尾（不能停 fs_tready）
                end

                // 逐条回退取一条
                S_PERW: if (!k_ok) begin
                    pend_err  <= 1'b1;                      // 越过地址空间：报 FETCH
                    err_code  <= EC_ERR_FETCH;
                    err_index <= global_k;
                    err_tag   <= 32'd0;
                    st        <= S_TERM;
                end else if (fe_ready) begin
                    rq_addr <= base_r + (k << 5);
                    rq_n    <= 5'd1;
                    rq_half <= half;
                    rq_off  <= {k[3:0] - bi[3:0], 1'b0};    // (k-bi) mod 16 的槽位×2 拍
                    rq_ctx  <= k[3:0];
                    rq_pf   <= 1'b0;
                    fe_start_q <= 1'b1;
                    st <= S_PER_WAIT;
                end

                S_PER_WAIT: if (tmo_evt) begin
                    pend_err  <= 1'b1;
                    err_code  <= EC_ERR_TIMEOUT;
                    err_index <= global_k;
                    err_tag   <= 32'd0;
                    st        <= S_TERM;
                end else if (fe_done && fe_owner == 2'd0) begin
                    if (fe_err) begin
                        pend_err  <= 1'b1;
                        err_code  <= EC_ERR_FETCH;
                        err_index <= global_k;
                        err_tag   <= 32'd0;
                        st        <= S_TERM;
                    end else begin
                        st <= S_EXEC;
                    end
                end

                // 条带相位：表头收 4 拍，跨条带不预取；正常 flush 后才开放下一段
                S_CFG: begin cfg_bad_q<=cfg_bad;st<=S_CFG_WAIT;end
                S_CFG_WAIT: begin
                    if(cfg_bad_q) begin
                        pend_err<=1;err_code<=EC_ERR_BAD_SURFACE;err_index<=0;err_tag<=0;st<=S_TERM;
                    end else begin
                        cfg_phase<=0;
                        band_bg_base<=surf_ra_w0;band_dst_base<=surf_rb_w0;
                        band_bg_slog2<=surf_ra_w2[4:0];band_dst_slog2<=surf_rb_w2[4:0];
                        band_frame_height<=surf_rb_w1[31:16];
                        rq_addr<=table_base;rq_n<=4;rq_half<=0;rq_off<=0;rq_ctx<=0;rq_pf<=0;
                        fe_start_q<=1;st<=S_HDR;
                    end
                end
                S_HDR: if(tmo_evt) begin
                    pend_err<=1;err_code<=EC_ERR_TIMEOUT;err_index<=0;err_tag<=0;st<=S_TERM;
                end else if(fe_done) begin
                    if(fe_err) begin pend_err<=1;err_code<=EC_ERR_FETCH;err_index<=0;err_tag<=0;st<=S_TERM;end
                    else begin hdr_beat<=0;st<=S_HADDR;end
                end
                S_HADDR: st<=S_HWAIT;
                S_HWAIT: st<=S_HREAD;
                S_HREAD: begin
                    for(i=0;i<4;i=i+1) counts[{hdr_beat,2'b00}+i]<=cbuf_rdata[32*i +:32];
                    if(hdr_beat==7) st<=S_BNEXT;
                    else begin hdr_beat<=hdr_beat+1'b1;st<=S_HADDR;end
                end
                S_BNEXT: begin
                    if(counts[strip_id]>1024) begin
                        pend_err<=1;err_code<=EC_ERR_BAD_FIELD;err_index<={17'd0,strip_id,10'd0};
                        err_tag<=0;st<=S_TERM;
                    end else begin
                        `ifdef INJ_STRIP_HEADER_OFFSET
                        base_r<=table_base+{27'd0,strip_id}*32768;
`else
                        base_r<=table_base+({27'd0,strip_id}+32'd1)*32768;
`endif
                        count_r<=counts[strip_id];bi<=0;k<=0;half<=0;per<=0;nst<=N_IDLE;
                        st<=S_BSTART;
                    end
                end
                S_BSTART: if(band_start_ready) st<=S_BLOAD;
                S_BLOAD: if(band_error || tmo_evt) begin
                    pend_err<=1;err_code<=tmo_evt ? EC_ERR_TIMEOUT : EC_ERR_BUS_READ;
                    err_index<={17'd0,strip_id,10'd0};err_tag<=0;st<=S_TERM;
                end else if(band_loaded) begin
                    if(count_r==0) st<=S_BFLUSH;
                    else begin
                        bn<=calc_n(base_r,0,count_r);rq_addr<=base_r;
                        rq_n<=calc_n(base_r,0,count_r);rq_half<=0;rq_off<=0;rq_ctx<=0;rq_pf<=0;
                        fe_start_q<=1;st<=S_FETCH;
                    end
                end
                S_BFLUSH: if(band_flush_ready) st<=S_BWAIT;
                S_BWAIT: if(band_error || tmo_evt) begin
                    pend_err<=1;err_code<=tmo_evt ? EC_ERR_TIMEOUT : EC_ERR_BUS_WRITE;
                    err_index<={17'd0,strip_id,10'd0};err_tag<=0;st<=S_TERM;
                end else if(band_flushed) begin
                    if({1'b0,strip_id}+6'd1==strips_r) begin pend_done<=1;st<=S_TERM;end
                    else begin strip_id<=strip_id+1'b1;st<=S_BNEXT;end
                end

                // 收尾：等在途命令与预取流全部静止才置 done_set/err_set——DONE/ERR 置位即引擎已空闲
                S_TERM: begin
                    if ((wr_stat_valid && wr_stat_last_row)
                     || (geom_cmd_done && geom_done_cidx == k[3:0]))
                        active <= 1'b0;
                    if (!active && !fe_busy_any && !inst_busy && !geom_busy &&
                        (!bmode || (pend_err ? drain_done : !band_busy))) begin
                        err_set  <= pend_err;
                        done_set <= pend_done;
                        pend_err <= 1'b0;
                        pend_done <= 1'b0;
                        busy_r   <= 1'b0;
                        nst      <= N_IDLE;
                        per      <= 1'b0;
                        st       <= S_IDLE;
                    end
                end

                default: st <= S_IDLE;
            endcase
        end
    end
endmodule
`default_nettype wire
