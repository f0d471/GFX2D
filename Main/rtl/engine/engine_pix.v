`timescale 1ns/1ps
//==============================================================================================//
// FILE: engine_pix.v
//
// DESCRIPTION: 计算阵列与片上缓存：每行一条边带加读流入片上行缓冲，两拍窗口把
//   源相位对齐到目的，12 个 engine_blend 算出 4 像素交给写流。只碰像素不碰地址。
//
// NOTE:
//   1. 源块 512×128、目的一块 512×96 两半乒乓；目的 X 字节不进 RAM，读写口只存 RGB
//   2. 延迟 7 拍；色键/掩码/valid/拍号四条延迟像逐级贴着数据链，差一级就错拍
//   3. 整条流水挂一个时钟使能 en = ws_tready || !ws_tvalid；一行发完等 8 拍排空再开下一行
//   4. 一行跨 4KB 时读流有两个 tlast，写指针两段之间不清，只在该半边带握手时清
//   5. 混合核五个输入必须由寄存器直接驱动；注错开关见 engine-manual.md
//==============================================================================================//
`default_nettype none

module engine_pix (
    input  wire         clk,
    input  wire         rst,            // 同步复位，与几何层、搬运层同一个

    // 每行边带（几何层 → 本层）
    input  wire [15:0]  sb_row,         // 命令内行号，最低位 = 这一行用的半
    input  wire [10:0]  sb_npx,         // 本行像素数
    input  wire [7:0]   sb_beats,       // 本行写拍数（拆段时是两段之和）
    input  wire [1:0]   sb_lead_dst,    // 目的行首地址的拍内像素相位 ld
    input  wire [1:0]   sb_lead_src,    // 源行首地址的拍内像素相位 ls（FILL 为 0）
    input  wire [7:0]   sb_op,          // 操作码，只区分 BLIT 与 FILL
    input  wire [3:0]   sb_eq,          // 方程号，0~5
    input  wire         sb_key_en,
    input  wire [31:0]  sb_key,         // 色键字（w5），与源原始存储字整字比较
    input  wire [31:0]  sb_w4,          // FILL 的常量源字
    input  wire [7:0]   sb_c,           // CONST 的系数
    input  wire [3:0]   sb_sfmt,        // 源格式，只对 BLIT 有意义
    input  wire         sb_read_dst,    // 这一行读不读目的
    input  wire         sb_valid,
    output wire         sb_ready,
    output wire [1:0]   row_free,       // 边带的反向：第 h 半两块都空着

    // 读流（搬运层 → 本层）
    input  wire [127:0] rs_tdata,
    input  wire [15:0]  rs_tkeep,       // 对齐描述符恒全 1，不用
    input  wire         rs_tlast,       // 标描述符边界而不是行边界，不用
    input  wire         rs_tvalid,
    output wire         rs_tready,      // 恒 1
    input  wire [1:0]   rs_buf,         // {源/目的, 半}

    // 写流（本层 → 搬运层），没有 tlast
    output reg  [127:0] ws_tdata,       // 每拍 4 像素，X 字节最高位 = 1 表示不写
    output wire [15:0]  ws_tkeep,
    output reg          ws_tvalid,
    input  wire         ws_tready,

    // 色键跳过事件（控制层 PERF_KEY_SKIP 计数源）
    // 与输出写流同拍寄存色键掩码，写流握手时才计数。
    output wire [3:0]   perf_key_lanes,
    output wire         perf_key_valid
);
    localparam [7:0] OP_BLIT = 8'd2;
    localparam [3:0] FMT_XRGB = 4'd0;
    localparam [1:0] PS_IDLE = 2'd0, PS_PRE = 2'd1,
                     PS_RUN = 2'd2, PS_DRAIN = 2'd3;

    // 同一个时钟使能管住每一级，包括行缓冲读口与 12 个核。
`ifdef INJ_EN_NOTREADY
    wire en = 1'b1;                       // 注错：流水不看 tready
`else
    wire en = (ws_tready || !ws_tvalid);
`endif
`ifdef INJ_CORE_NOEN
    wire en_core = 1'b1;                  // 注错：只让 12 个核不看流水停
`else
    wire en_core = en;
`endif
    assign ws_tkeep = 16'hffff;
    assign rs_tready = 1'b1;

    // 首末拍掩码，边带握手那一拍算：第 beat 拍的四个槽谁属于本行。
    // 一拍的行把行尾也算进首拍掩码，所以像素通路只在首拍/末拍/满拍三者里选。
    function [3:0] row_mask;
        input [1:0] lead;
        input [10:0] pixels;
        input [7:0] beat;
        reg [11:0] first, limit, pos;
        integer t;
        begin
            first = {10'd0, lead};
            limit = first + {1'b0, pixels};
            row_mask = 4'b0000;
            for (t = 0; t < 4; t = t + 1) begin
                pos = {2'd0, beat, 2'b00} + t;
                row_mask[t] = (pos >= first) && (pos < limit);
            end
        end
    endfunction

    // 行队列按边带握手先后记下半号；一半放出前不会再收边带，队列两项总是不同的半
    reg [1:0] occupied;
    reg row_queue [0:1];
    reg q_rd, q_wr;
    reg [1:0] q_count;
    wire sb_fire = sb_valid && sb_ready;
    assign row_free = ~occupied;
    assign sb_ready = !rst && !occupied[sb_row[0]] && (q_count != 2'd2);

    // 每一半的行参数与读入计数，边带握手时装入
    reg h_blit [0:1], h_key_en [0:1], h_read_dst [0:1];
    reg [2:0] h_eq [0:1];
    reg [31:0] h_key [0:1], h_w4 [0:1];
    reg [7:0] h_c [0:1];
    reg [3:0] h_sfmt [0:1];
    reg [1:0] h_ls [0:1], h_ld [0:1];
    reg [7:0] h_beats [0:1], h_src_need [0:1];
    reg [7:0] h_src_count [0:1], h_dst_count [0:1];
    reg h_src_done [0:1], h_dst_done [0:1];
    reg [3:0] h_first_mask [0:1], h_last_mask [0:1];
    // 源要读的拍数 = ceil((ls + npx) / 4)，与几何层源覆盖区间的拍数相同
    wire [11:0] src_sum = {1'b0, sb_npx} + {10'd0, sb_lead_src} + 12'd3;
    wire [7:0] src_need = src_sum[9:2];
    wire next_half = row_queue[q_rd];
    reg [1:0] pst;
    // 开行条件：队头那一半要用的源、目的都已读齐（FILL 不等源，不读目的的行不等目的）
    wire row_start = !rst && en && (pst == PS_IDLE) &&
                     (q_count != 2'd0) && h_src_done[next_half] &&
                     h_dst_done[next_half];

    // 行缓冲：一行跨 4KB 时读流有两个 tlast，两段共用同一个写指针，读齐按拍数判
    wire src_we = !rst && rs_tvalid && !rs_buf[1] && occupied[rs_buf[0]];
    wire dst_we = !rst && rs_tvalid && rs_buf[1] && occupied[rs_buf[0]];
    wire [8:0] src_waddr = {rs_buf[0], h_src_count[rs_buf[0]]};
    wire [8:0] dst_waddr = {rs_buf[0], h_dst_count[rs_buf[0]]};
    // 目的只存每个像素的 RGB，X 字节不进 RAM
    wire [95:0] dst_wdata = {rs_tdata[119:96], rs_tdata[87:64],
                             rs_tdata[55:32], rs_tdata[23:0]};
    reg [127:0] src_mem [0:511];
    reg [95:0] dst_mem [0:511];
    reg [127:0] q_src;
    reg [95:0] q_dst;
    reg [8:0] src_raddr, dst_raddr;
    // FILL 不读源块，COPY 不读目的块（读出值不进通路，关掉读使能省功耗）
    wire src_re = en && (pst == PS_PRE || pst == PS_RUN) && r_blit;
    wire dst_re = en && (pst == PS_PRE || pst == PS_RUN) && r_read_dst;
    always @(posedge clk) begin
        if (src_we) src_mem[src_waddr] <= rs_tdata;
        if (dst_we) dst_mem[dst_waddr] <= dst_wdata;
        if (src_re) q_src <= src_mem[src_raddr];
        if (dst_re) q_dst <= dst_mem[dst_raddr];
    end

    // 当前行的参数，开行时从该半装入，整行不变；排空之后才换下一行的
    reg r_blit, r_key_en, r_read_dst;
    reg [2:0] r_eq;
    reg [31:0] r_key, r_w4;
    reg [7:0] r_c;
    reg [3:0] r_sfmt;
    reg [1:0] r_ls, r_ld;
    reg r_half;
    reg [7:0] r_beats;
    reg [3:0] r_first_mask, r_last_mask;
    reg [7:0] cnt_k, out_count, drain_count;
    reg running;

    // 周期 0：地址
    wire rot_off = (r_ls >= r_ld);
    wire [2:0] st_raw = rot_off ? ({1'b0, r_ls} - {1'b0, r_ld})
                                 : ({1'b0, r_ls} + 3'd4 - {1'b0, r_ld});
`ifdef INJ_ROT_OFF
    wire [2:0] st_use = (st_raw + 3'd1);  // 注错：旋转错一个位置
`else
    wire [2:0] st_use = st_raw;
`endif
    wire [7:0] src_k = cnt_k + {7'd0, rot_off};
    always @* begin
        src_raddr = {r_half, (pst == PS_PRE ? 8'd0 : src_k)};
        dst_raddr = {r_half, cnt_k};
    end

    // 周期 2：数据级
    // 两拍窗口：r_prev 是前拍、r_cur 是当前拍；r_k2 与 r_cur 同级（拍号链多一级）
    reg [127:0] r_cur, r_prev;
    reg [95:0] r_dst;
    reg [7:0] r_k, r_k2;
    reg r_v1;
    always @(posedge clk) begin
        if (rst) begin
            r_cur <= 128'd0; r_prev <= 128'd0; r_dst <= 96'd0;
            r_k <= 8'd0; r_k2 <= 8'd0; r_v1 <= 1'b0;
        end else if (en) begin
            r_cur <= q_src;
            r_prev <= r_cur;
            r_dst <= q_dst;
            r_k <= cnt_k;
            r_k2 <= r_k;
            r_v1 <= running;
        end
    end

    // 周期 3：旋转、色键、a 选择、掩码（组合，落寄存器）
    wire [31:0] w [0:7];
    assign w[0] = r_prev[31:0];  assign w[1] = r_prev[63:32];
    assign w[2] = r_prev[95:64]; assign w[3] = r_prev[127:96];
    assign w[4] = r_cur[31:0];   assign w[5] = r_cur[63:32];
    assign w[6] = r_cur[95:64];  assign w[7] = r_cur[127:96];
    wire [31:0] px0 = (st_use == 3'd0) ? w[0] : (st_use == 3'd1) ? w[1] :
                      (st_use == 3'd2) ? w[2] : w[3];
    wire [31:0] px1 = (st_use == 3'd0) ? w[1] : (st_use == 3'd1) ? w[2] :
                      (st_use == 3'd2) ? w[3] : w[4];
    wire [31:0] px2 = (st_use == 3'd0) ? w[2] : (st_use == 3'd1) ? w[3] :
                      (st_use == 3'd2) ? w[4] : w[5];
    wire [31:0] px3 = (st_use == 3'd0) ? w[3] : (st_use == 3'd1) ? w[4] :
                      (st_use == 3'd2) ? w[5] : w[6];
    // FILL 的源是常量，不读源块（DMA2D 的 R2M 同法）
    wire [31:0] s0 = r_blit ? px0 : r_w4;
    wire [31:0] s1 = r_blit ? px1 : r_w4;
    wire [31:0] s2 = r_blit ? px2 : r_w4;
    wire [31:0] s3 = r_blit ? px3 : r_w4;
`ifdef INJ_SKIP_KEY
    wire [3:0] keys = 4'd0;               // 注错：跳过色键，命中的像素照写
`else
    wire [3:0] keys = r_blit && r_key_en ?
                      {s3 == r_key, s2 == r_key, s1 == r_key, s0 == r_key} : 4'd0;
`endif
    // a：BLIT 的 XRGB 源取 255；FILL 一律取 w4[31:24]（规范条款 §4.2）
    wire [7:0] a0 = (r_blit && r_sfmt == FMT_XRGB) ? 8'hff : s0[31:24];
    wire [7:0] a1 = (r_blit && r_sfmt == FMT_XRGB) ? 8'hff : s1[31:24];
    wire [7:0] a2 = (r_blit && r_sfmt == FMT_XRGB) ? 8'hff : s2[31:24];
    wire [7:0] a3 = (r_blit && r_sfmt == FMT_XRGB) ? 8'hff : s3[31:24];
    wire [3:0] mask_now;
`ifdef INJ_DROP_MASK
    assign mask_now = (r_k2 == 8'd0) ? 4'hf :        // 注错：丢首拍掩码
                      (r_k2 == (r_beats - 8'd1)) ? r_last_mask : 4'hf;
`else
    assign mask_now = (r_k2 == 8'd0) ? r_first_mask :
                      (r_k2 == (r_beats - 8'd1)) ? r_last_mask : 4'hf;
`endif

    // 混合核的五个输入全由寄存器直接驱动：旋转与色键的组合逻辑不能挂在核第一级前面
    reg [127:0] r_px;
    reg [95:0] r_dst2;
    reg [31:0] r_a;
    reg [3:0] r_keys, r_mask;
    reg r_v2;
    always @(posedge clk) begin
        if (rst) begin
            r_px <= 128'd0; r_dst2 <= 96'd0; r_a <= 32'd0;
            r_keys <= 4'd0; r_mask <= 4'd0; r_v2 <= 1'b0;
        end else if (en) begin
            r_px <= {s3, s2, s1, s0};
            r_dst2 <= r_read_dst ? r_dst : 96'd0;
            r_a <= {a3, a2, a1, a0};
            r_keys <= keys;
            r_mask <= mask_now;
            r_v2 <= r_v1;
        end
    end

    // 周期 4~6：12 个混合核（4 像素 × RGB）
    wire [95:0] color;
    genvar p, ch;
    generate
        for (p = 0; p < 4; p = p + 1) begin : pixel
            for (ch = 0; ch < 3; ch = ch + 1) begin : channel
                engine_blend u_blend (
                    .clk(clk), .en(en_core),
                    .src(r_px[p*32+ch*8 +: 8]),
                    .dst(r_dst2[p*24+ch*8 +: 8]),
                    .a(r_a[p*8 +: 8]), .c(r_c), .eq(r_eq),
                    .out(color[p*24+ch*8 +: 8]));
            end
        end
    endgenerate

    // 色键、掩码与 valid 的延迟像，与混合核的三级寄存器逐级对齐
    reg [3:0] key_d1, key_d2, key_d3, mask_d1, mask_d2, mask_d3;
    reg v_d1, v_d2, v_d3, v_d4;
    always @(posedge clk) begin
        if (rst) begin
            key_d1 <= 0; key_d2 <= 0; key_d3 <= 0;
            mask_d1 <= 0; mask_d2 <= 0; mask_d3 <= 0;
            v_d1 <= 0; v_d2 <= 0; v_d3 <= 0; v_d4 <= 0;
        end else if (en) begin
            key_d1 <= r_keys; key_d2 <= key_d1; key_d3 <= key_d2;
            mask_d1 <= r_mask; mask_d2 <= mask_d1; mask_d3 <= mask_d2;
            v_d1 <= r_v2; v_d2 <= v_d1; v_d3 <= v_d2; v_d4 <= v_d3;
        end
    end

    // 周期 7：合并「不写」标记，输出 X 字节只有标记位
    wire [3:0] no_write = key_d3 | ~mask_d3;
    // 计数掩码与 ws_tdata 同拍寄存，否则握手时看到的是下一拍
    reg [3:0] ws_key_lanes;
    assign perf_key_lanes = ws_key_lanes;
    assign perf_key_valid = ws_tvalid && ws_tready;
    wire [127:0] next_data = {
        (no_write[3] ? 8'h80 : 8'h00), color[95:72],
        (no_write[2] ? 8'h80 : 8'h00), color[71:48],
        (no_write[1] ? 8'h80 : 8'h00), color[47:24],
        (no_write[0] ? 8'h80 : 8'h00), color[23:0]};
    always @(posedge clk) begin
        if (rst) begin
            ws_tdata <= 128'd0; ws_tvalid <= 1'b0; ws_key_lanes <= 4'd0;
        end else if (en) begin
            ws_tdata <= next_data; ws_tvalid <= v_d4;
            ws_key_lanes <= key_d3 & mask_d3;
        end
    end

    // 本行写流的末拍被写核收走：放出这一半
    wire last_out = ws_tvalid && ws_tready &&
                    (out_count == (r_beats - 8'd1));
    always @(posedge clk) begin
        if (rst) out_count <= 8'd0;
        else if (row_start) out_count <= 8'd0;
        else if (ws_tvalid && ws_tready) out_count <= out_count + 8'd1;
    end

    // 每一半的占用、行参数与读入计数
    // 读齐按本行对齐后的总拍数判，不看 rs_tlast（拆段时一行有两个）
    always @(posedge clk) begin
        if (rst) begin
            occupied <= 2'b00;
            q_rd <= 1'b0; q_wr <= 1'b0; q_count <= 2'd0;
            h_src_count[0] <= 0; h_src_count[1] <= 0;
            h_dst_count[0] <= 0; h_dst_count[1] <= 0;
            h_src_done[0] <= 0; h_src_done[1] <= 0;
            h_dst_done[0] <= 0; h_dst_done[1] <= 0;
        end else begin
            if (sb_fire) begin
                occupied[sb_row[0]] <= 1'b1;
                row_queue[q_wr] <= sb_row[0]; q_wr <= ~q_wr;
                h_blit[sb_row[0]] <= (sb_op == OP_BLIT);
                h_key_en[sb_row[0]] <= sb_key_en;
                h_read_dst[sb_row[0]] <= sb_read_dst;
                h_eq[sb_row[0]] <= sb_eq[2:0];
                h_key[sb_row[0]] <= sb_key; h_w4[sb_row[0]] <= sb_w4;
                h_c[sb_row[0]] <= sb_c; h_sfmt[sb_row[0]] <= sb_sfmt;
                h_ls[sb_row[0]] <= sb_lead_src; h_ld[sb_row[0]] <= sb_lead_dst;
                h_beats[sb_row[0]] <= sb_beats;
                h_src_need[sb_row[0]] <= src_need;
                h_src_count[sb_row[0]] <= 8'd0;
                h_dst_count[sb_row[0]] <= 8'd0;
                h_src_done[sb_row[0]] <= (sb_op != OP_BLIT);
                h_dst_done[sb_row[0]] <= !sb_read_dst;
                h_first_mask[sb_row[0]] <= row_mask(sb_lead_dst, sb_npx, 8'd0);
                h_last_mask[sb_row[0]] <= row_mask(sb_lead_dst, sb_npx, sb_beats - 8'd1);
            end
            if (src_we) begin
                h_src_count[rs_buf[0]] <= h_src_count[rs_buf[0]] + 8'd1;
                if (h_src_count[rs_buf[0]] + 8'd1 == h_src_need[rs_buf[0]])
                    h_src_done[rs_buf[0]] <= 1'b1;
            end
            if (dst_we) begin
                h_dst_count[rs_buf[0]] <= h_dst_count[rs_buf[0]] + 8'd1;
                if (h_dst_count[rs_buf[0]] + 8'd1 == h_beats[rs_buf[0]])
                    h_dst_done[rs_buf[0]] <= 1'b1;
            end
            if (row_start) q_rd <= ~q_rd;
            case ({sb_fire, row_start})
                2'b10: q_count <= q_count + 2'd1;
                2'b01: q_count <= q_count - 2'd1;
                default: q_count <= q_count;
            endcase
            if (last_out) occupied[r_half] <= 1'b0;
        end
    end

    // 行状态机：发完等 8 拍排空再回 PS_IDLE（下一行参数寄存器要等它），另一半读入照常
    always @(posedge clk) begin
        if (rst) begin
            pst <= PS_IDLE; cnt_k <= 8'd0; drain_count <= 8'd0;
            running <= 1'b0;
            r_blit <= 0; r_key_en <= 0; r_read_dst <= 0; r_eq <= 0;
            r_key <= 0; r_w4 <= 0; r_c <= 0; r_sfmt <= 0;
            r_ls <= 0; r_ld <= 0; r_half <= 0;
            r_beats <= 0; r_first_mask <= 0; r_last_mask <= 0;
        end else if (en) begin
            case (pst)
                PS_IDLE: if (row_start) begin
                    r_half <= next_half;
                    r_blit <= h_blit[next_half];
                    r_key_en <= h_key_en[next_half];
                    r_read_dst <= h_read_dst[next_half];
                    r_eq <= h_eq[next_half];
                    r_key <= h_key[next_half];
                    r_w4 <= h_w4[next_half];
                    r_c <= h_c[next_half];
                    r_sfmt <= h_sfmt[next_half];
                    r_ls <= h_ls[next_half];
                    r_ld <= h_ld[next_half];
                    r_beats <= h_beats[next_half];
                    r_first_mask <= h_first_mask[next_half];
                    r_last_mask <= h_last_mask[next_half];
                    cnt_k <= 8'd0;
                    // off = 1 的行先预热一拍
                    if (h_ls[next_half] >= h_ld[next_half]) begin
                        running <= 1'b0; pst <= PS_PRE;
                    end else begin
                        running <= 1'b1; pst <= PS_RUN;
                    end
                end
                PS_PRE: begin
                    cnt_k <= 8'd0; running <= 1'b1; pst <= PS_RUN;
                end
                PS_RUN: begin
                    if (cnt_k + 8'd1 >= r_beats) begin
                        cnt_k <= 8'd0; running <= 1'b0;
                        drain_count <= 8'd7; pst <= PS_DRAIN;
                    end else cnt_k <= cnt_k + 8'd1;
                end
                PS_DRAIN: begin
                    if (drain_count == 8'd0) pst <= PS_IDLE;
                    else drain_count <= drain_count - 8'd1;
                end
                default: pst <= PS_IDLE;
            endcase
        end
    end
endmodule

`default_nettype wire
