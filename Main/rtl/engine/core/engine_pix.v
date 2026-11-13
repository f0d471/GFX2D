`timescale 1ns/1ps
//==============================================================================================//
// FILE: engine_pix.v
// 行缓冲与 12 核计算阵列：整行接收、按相位对齐、逐拍输出带「不写」标记的写流。
//
// 输入前提（几何/搬运层保证，本层不重复校验）：
//   1. 行非空；sb_beats 与 BLIT 源拍数都在 1~255 内（每半只有 256 个拍地址）。
//   2. 同一半先握手再返回数据；首个数据拍不得与预约同沿；sb_ready=0 时上游保持整组参数。
//   3. rs_buf 指明源/目的与半号，只发往已预约的半，精确返回所需拍数，读拍完整 16 字节。
//   4. 跨描述符分段仍属同一行；rs_tlast 只标段尾，本层不用它结束行。
//   5. 操作码/方程/格式/色键已校验；FILL 的 sb_lead_src 为 0。
//==============================================================================================//
`default_nettype none
`include "engine_feature.vh"

module engine_pix (
    input  wire         clk,
    input  wire         rst,            // 同步复位

    // 每行边带：几何层 → 本层，一行一组
    input  wire [15:0]  sb_row,         // 行号，最低位 = 半
    input  wire [10:0]  sb_npx,         // 本行像素数
    input  wire [7:0]   sb_beats,       // 本行写拍数（拆段时为两段之和）
    input  wire [1:0]   sb_lead_dst,    // 目的相位：行首在拍内的位置
    input  wire [1:0]   sb_lead_src,    // 源相位；FILL 为 0
    input  wire [7:0]   sb_op,          // 操作码，只分 BLIT 与 FILL
    input  wire [3:0]   sb_eq,          // 方程号 0~5
    input  wire         sb_key_en,      // 色键使能
    input  wire [31:0]  sb_key,         // 色键字，比原始 32 位字
    input  wire [31:0]  sb_w4,          // FILL 常量源字
    input  wire [7:0]   sb_c,           // CONST 系数
    input  wire [3:0]   sb_sfmt,        // 源格式，仅 BLIT 有意义
    input  wire         sb_read_dst,    // 是否读目的块
    input  wire         sb_valid,       // 边带有效
    output wire         sb_ready,       // 本半空且队列未满
    output wire [1:0]   row_free,       // 每半一位，1 = 空

    // 读流：搬运层 → 本层，一拍 16 字节
    input  wire [127:0] rs_tdata,       // 一拍 4 个像素
    input  wire [15:0]  rs_tkeep,       // 恒全 1，不用
    input  wire         rs_tlast,       // 只标段尾，不用
    input  wire         rs_tvalid,      // 读数据有效
    output wire         rs_tready,      // 恒 1
    input  wire [1:0]   rs_buf,         // {源/目的, 半}

    // 写流：本层 → 搬运层，无 tlast
    output reg  [127:0] ws_tdata,       // 每拍 4 像素，X 字节最高位 = 不写
    output wire [15:0]  ws_tkeep,       // 恒全 1
    output reg          ws_tvalid,      // 写数据有效
    input  wire         ws_tready,      // 写数据就绪

    // 色键统计：逐槽命中，供控制层计数
    output wire [3:0]   perf_key_lanes, // 逐槽命中，与写数据同拍
    output wire         perf_key_valid  // 写流握手那一拍
);
    localparam [7:0] OP_BLIT = 8'd2;
    localparam [3:0] FMT_XRGB = 4'd0;
    localparam [1:0] PS_IDLE = 2'd0;
    localparam [1:0] PS_PRE = 2'd1;
    localparam [1:0] PS_RUN = 2'd2;
    localparam [1:0] PS_DRAIN = 2'd3;
    localparam [7:0] DRAIN_INIT = 8'd7; // 7~0，共 8 个推进沿

    // row_mask：一拍四槽里哪些像素属于本行
    function [3:0] row_mask;
        input [1:0] lead;
        input [10:0] pixels;
        input [7:0] beat;
        reg [11:0] first, limit, pos;
        integer t;
        begin
            first = {10'd0, lead};
            limit = first + {1'b0, pixels};
            row_mask = 4'd0;
            for (t = 0; t < 4; t = t + 1) begin
                pos = {2'd0, beat, 2'b00} + t;
                row_mask[t] = (pos >= first) && (pos < limit);
            end
        end
    endfunction

    // 跨段事件：开行 row_take、完成 row_done、交接 write_fire
    wire row_take;
    wire row_done;
    wire row_done_half;
    wire write_fire = ws_tvalid && ws_tready;

    // 计算链使能：出口满则整链保持，空则不等 ready
    wire pipe_en = ws_tready || !ws_tvalid; // 出口空着就不等 ready

    wire core_en = pipe_en; // 核与外围同停

    // 恒定输出：ws_tkeep 全 1，rs_tready 恒 1
    assign ws_tkeep = 16'hffff;
    assign rs_tready = 1'b1;

    // 行接收状态：两半各一套参数、计数与读齐位
    reg [1:0] half_occupied;
    reg row_queue [0:1];
    reg row_queue_read_ptr, row_queue_write_ptr;
    reg [1:0] row_queue_count;

    reg half_blit [0:1], half_key_en [0:1], half_read_dst [0:1];
    reg [2:0] half_eq [0:1];
    reg [31:0] half_key [0:1], half_fill [0:1];
    reg [7:0] half_const_coeff [0:1];
    reg [3:0] half_src_format [0:1];
    reg [1:0] half_lead_src [0:1], half_lead_dst [0:1];
    reg [7:0] half_dst_beats [0:1], half_src_beats [0:1];
    reg [3:0] half_first_mask [0:1], half_last_mask [0:1];
    reg [7:0] half_src_count [0:1], half_dst_count [0:1];
    reg half_src_done [0:1], half_dst_done [0:1];

    assign row_free = ~half_occupied;
    assign sb_ready = !rst && !half_occupied[sb_row[0]] && (row_queue_count != 2'd2);
    wire row_accept = sb_valid && sb_ready;

    // 源拍数：ceil((ls+npx)/4)
    wire [11:0] src_span_plus3 = {1'b0, sb_npx} + {10'd0, sb_lead_src} + 12'd3;
    wire [7:0] half_src_beats_next = src_span_plus3[9:2];

    // 接收写口：按半号与各路计数写入
    wire src_we = !rst && rs_tvalid && !rs_buf[1] && half_occupied[rs_buf[0]]; // 未预约的半丢弃
    wire dst_we = !rst && rs_tvalid && rs_buf[1] && half_occupied[rs_buf[0]];
    wire [8:0] src_waddr = {rs_buf[0], half_src_count[rs_buf[0]]};
    wire [8:0] dst_waddr = {rs_buf[0], half_dst_count[rs_buf[0]]};
    wire [95:0] dst_wdata = {rs_tdata[119:96], rs_tdata[87:64], rs_tdata[55:32], rs_tdata[23:0]};

    always @(posedge clk) begin // 不受 pipe_en 控制
        if (rst) begin
            half_occupied <= 2'd0;
            row_queue_read_ptr <= 1'b0;
            row_queue_write_ptr <= 1'b0;
            row_queue_count <= 2'd0;
            half_src_count[0] <= 8'd0;
            half_src_count[1] <= 8'd0;
            half_dst_count[0] <= 8'd0;
            half_dst_count[1] <= 8'd0;
            half_src_done[0] <= 1'b0;
            half_src_done[1] <= 1'b0;
            half_dst_done[0] <= 1'b0;
            half_dst_done[1] <= 1'b0;
        end else begin
            if (row_accept) begin
                half_occupied[sb_row[0]] <= 1'b1;
                row_queue[row_queue_write_ptr] <= sb_row[0];
                row_queue_write_ptr <= ~row_queue_write_ptr;
                half_blit[sb_row[0]] <= (sb_op == OP_BLIT);
                half_key_en[sb_row[0]] <= sb_key_en;
                half_read_dst[sb_row[0]] <= sb_read_dst;
                half_eq[sb_row[0]] <= sb_eq[2:0];
                half_key[sb_row[0]] <= sb_key;
                half_fill[sb_row[0]] <= sb_w4;
                half_const_coeff[sb_row[0]] <= sb_c;
                half_src_format[sb_row[0]] <= sb_sfmt;
                half_lead_src[sb_row[0]] <= sb_lead_src;
                half_lead_dst[sb_row[0]] <= sb_lead_dst;
                half_dst_beats[sb_row[0]] <= sb_beats;
                half_src_beats[sb_row[0]] <= half_src_beats_next;
                half_src_count[sb_row[0]] <= 8'd0;
                half_dst_count[sb_row[0]] <= 8'd0;
                half_src_done[sb_row[0]] <= (sb_op != OP_BLIT);
                half_dst_done[sb_row[0]] <= !sb_read_dst;
                half_first_mask[sb_row[0]] <= row_mask(sb_lead_dst, sb_npx, 8'd0);
                half_last_mask[sb_row[0]] <= row_mask(sb_lead_dst, sb_npx, sb_beats - 8'd1);
            end
            if (src_we) begin
                half_src_count[rs_buf[0]] <= half_src_count[rs_buf[0]] + 8'd1;
                if (half_src_count[rs_buf[0]] + 8'd1 == half_src_beats[rs_buf[0]])
                    half_src_done[rs_buf[0]] <= 1'b1;
            end
            if (dst_we) begin
                half_dst_count[rs_buf[0]] <= half_dst_count[rs_buf[0]] + 8'd1;
                if (half_dst_count[rs_buf[0]] + 8'd1 == half_dst_beats[rs_buf[0]])
                    half_dst_done[rs_buf[0]] <= 1'b1;
            end
            if (row_take) row_queue_read_ptr <= ~row_queue_read_ptr;
            case ({row_accept, row_take})
                2'b10: row_queue_count <= row_queue_count + 2'd1;
                2'b01: row_queue_count <= row_queue_count - 2'd1;
                default: row_queue_count <= row_queue_count;
            endcase
            if (row_done) half_occupied[row_done_half] <= 1'b0;
        end
    end

    wire head_half = row_queue[row_queue_read_ptr];
    wire head_row_ready = (row_queue_count != 2'd0) &&
                          half_src_done[head_half] && half_dst_done[head_half];

    // 行调度与当前行参数：开行时整组装入，整行保持
    reg [1:0] row_state;
    reg [7:0] issue_beat;
    reg issue_valid;
    reg [7:0] drain_count;

    reg row_half;
    reg row_blit, row_read_dst, row_key_en;
    reg [2:0] row_eq; // 直连 12 核，整行不变
    reg [31:0] row_key, row_fill;
    reg [7:0] row_const_coeff, row_dst_beats; // c 也直连核；beats 用于末拍判断
    reg [3:0] row_src_format;
    reg [1:0] row_lead_src, row_lead_dst;
    reg [3:0] row_first_mask, row_last_mask;

    assign row_take = !rst && pipe_en && (row_state == PS_IDLE) && head_row_ready;

    always @(posedge clk) begin // 开行时整组装入
        if (rst) begin
            row_blit <= 1'b0;
            row_key_en <= 1'b0;
            row_read_dst <= 1'b0;
            row_eq <= 3'd0;
            row_key <= 32'd0;
            row_fill <= 32'd0;
            row_const_coeff <= 8'd0;
            row_src_format <= 4'd0;
            row_lead_src <= 2'd0;
            row_lead_dst <= 2'd0;
            row_half <= 1'b0;
            row_dst_beats <= 8'd0;
            row_first_mask <= 4'd0;
            row_last_mask <= 4'd0;
        end else if (row_take) begin
            row_half <= head_half;
            row_blit <= half_blit[head_half];
            row_key_en <= half_key_en[head_half];
            row_read_dst <= half_read_dst[head_half];
            row_eq <= half_eq[head_half];
            row_key <= half_key[head_half];
            row_fill <= half_fill[head_half];
            row_const_coeff <= half_const_coeff[head_half];
            row_src_format <= half_src_format[head_half];
            row_lead_src <= half_lead_src[head_half];
            row_lead_dst <= half_lead_dst[head_half];
            row_dst_beats <= half_dst_beats[head_half];
            row_first_mask <= half_first_mask[head_half];
            row_last_mask <= half_last_mask[head_half];
        end
    end

    // 行状态机：发拍号、工作有效与排空，按 pipe_en 推进
    always @(posedge clk) begin
        if (rst) begin
            row_state <= PS_IDLE;
            issue_beat <= 8'd0;
            drain_count <= 8'd0;
            issue_valid <= 1'b0;
        end else if (pipe_en) begin
            case (row_state)
                PS_IDLE: if (row_take) begin
                    issue_beat <= 8'd0;
                    // 本沿 row_* 还没更新，用队头参数判断预热
                    if (half_lead_src[head_half] >= half_lead_dst[head_half]) begin
                        issue_valid <= 1'b0;
                        row_state <= PS_PRE;
                    end else begin
                        issue_valid <= 1'b1;
                        row_state <= PS_RUN;
                    end
                end
                PS_PRE: begin
                    issue_beat <= 8'd0;
                    issue_valid <= 1'b1;
                    row_state <= PS_RUN;
                end
                PS_RUN: begin
                    if (issue_beat + 8'd1 >= row_dst_beats) begin
                        issue_beat <= 8'd0;
                        issue_valid <= 1'b0;
                        drain_count <= DRAIN_INIT;
                        row_state <= PS_DRAIN;
                    end else issue_beat <= issue_beat + 8'd1;
                end
                PS_DRAIN: begin
                    if (drain_count == 8'd0) row_state <= PS_IDLE;
                    else drain_count <= drain_count - 8'd1;
                end
                default: row_state <= PS_IDLE;
            endcase
        end
    end

    // S1：行 RAM 同步读出，读地址按相位差与 PRE 生成
    reg [127:0] src_mem [0:511];
    reg [95:0] dst_mem [0:511];
    reg [127:0] s1_src;
    reg [95:0] s1_dst;
    reg [7:0] s1_beat;
    reg s1_valid;

    wire src_read_ahead = (row_lead_src >= row_lead_dst);
    wire [7:0] src_read_beat = issue_beat + {7'd0, src_read_ahead};
    wire [8:0] src_raddr = {row_half, (row_state == PS_PRE ? 8'd0 : src_read_beat)};
    wire [8:0] dst_raddr = {row_half, issue_beat};
    wire src_re = pipe_en && (row_state == PS_PRE || row_state == PS_RUN) && row_blit;
    wire dst_re = pipe_en && (row_state == PS_PRE || row_state == PS_RUN) && row_read_dst;

    always @(posedge clk) begin // 写口不看 pipe_en，读口按行开
        if (src_we) src_mem[src_waddr] <= rs_tdata;
        if (dst_we) dst_mem[dst_waddr] <= dst_wdata;
        if (src_re) s1_src <= src_mem[src_raddr];
        if (dst_re) s1_dst <= dst_mem[dst_raddr];
    end

    // S1 事务身份：拍号与 valid 与该拍数据同沿
    always @(posedge clk) begin
        if (rst) begin
            s1_beat <= 8'd0;
            s1_valid <= 1'b0;
        end else if (pipe_en) begin
            s1_beat <= issue_beat;
            s1_valid <= issue_valid; // FILL 也注入有效工作
        end
    end

    // S2：当前拍与前一拍拼成八槽窗口，目的/拍号/valid 同步延迟
    reg [127:0] s2_src_cur, s2_src_prev;
    reg [95:0] s2_dst;
    reg [7:0] s2_beat;
    reg s2_valid;

    always @(posedge clk) begin
        if (rst) begin
            s2_src_cur <= 128'd0;
            s2_src_prev <= 128'd0;
            s2_dst <= 96'd0;
            s2_beat <= 8'd0;
            s2_valid <= 1'b0;
        end else if (pipe_en) begin
            s2_src_cur <= s1_src;
            s2_src_prev <= s2_src_cur;
            s2_dst <= s1_dst;
            s2_beat <= s1_beat;
            s2_valid <= s1_valid;
        end
    end

    // S3：对齐、alpha、色键与掩码在核输入寄存器处截止
    reg [127:0] s3_src;
    reg [95:0] s3_dst;
    reg [31:0] s3_alpha;
    reg [3:0] s3_key_hit, s3_mask;
    reg s3_valid;

    // 八槽窗口：低四槽是前拍，高四槽是当前拍
    wire [255:0] src_window = {s2_src_cur, s2_src_prev};
    wire [2:0] align_start_raw = src_read_ahead
        ? ({1'b0, row_lead_src} - {1'b0, row_lead_dst})
        : ({1'b0, row_lead_src} + 3'd4 - {1'b0, row_lead_dst}); // st = (ls-ld) mod 4
    wire [2:0] align_start = align_start_raw;

    wire [127:0] aligned_src =
        (align_start == 3'd0) ? src_window[127:0]  :
        (align_start == 3'd1) ? src_window[159:32] :
        (align_start == 3'd2) ? src_window[191:64] :
                               src_window[223:96]; // 其他起点按槽 3 兜底
    wire [127:0] s3_src_next = row_blit ? aligned_src : {4{row_fill}}; // FILL 用常量
    wire [95:0] s3_dst_next = row_read_dst ? s2_dst : 96'd0; // 不读目的时清 0

    // 色键：比对齐后的原始 32 位字
    `ifdef GFX_KEY
        wire [3:0] s3_key_hit_next = row_blit && row_key_en ? {
            s3_src_next[127:96] == row_key, s3_src_next[95:64] == row_key,
            s3_src_next[63:32] == row_key, s3_src_next[31:0] == row_key} : 4'd0;
    `else
        wire [3:0] s3_key_hit_next = 4'd0;
    `endif

    // alpha：XRGB 取 255，其余取源字高字节
    wire xrgb_src = row_blit && (row_src_format == FMT_XRGB); // 仅 BLIT/XRGB
    wire [31:0] s3_alpha_next = {
        (xrgb_src ? 8'hff : s3_src_next[127:120]),
        (xrgb_src ? 8'hff : s3_src_next[95:88]),
        (xrgb_src ? 8'hff : s3_src_next[63:56]),
        (xrgb_src ? 8'hff : s3_src_next[31:24])};

    // 首/末拍掩码：只有首末拍可能越界，中间恒满
    wire [3:0] s3_mask_next;
    assign s3_mask_next = (s2_beat == 8'd0) ? row_first_mask :
        (s2_beat == (row_dst_beats - 8'd1)) ? row_last_mask : 4'hf; // beat==0 先判，一拍行用首 mask

    // S3 打拍：所有组合输入在此截止
    always @(posedge clk) begin
        if (rst) begin
            s3_src <= 128'd0;
            s3_dst <= 96'd0;
            s3_alpha <= 32'd0;
            s3_key_hit <= 4'd0;
            s3_mask <= 4'd0;
            s3_valid <= 1'b0;
        end else if (pipe_en) begin
            s3_src <= s3_src_next;
            s3_dst <= s3_dst_next;
            s3_alpha <= s3_alpha_next;
            s3_key_hit <= s3_key_hit_next;
            s3_mask <= s3_mask_next;
            s3_valid <= s2_valid;
        end
    end

    // S4~S6：4 像素 × 3 通道共 12 个核，每个三级
    wire [95:0] s6_color;
    genvar p, ch;
    generate
        for (p = 0; p < 4; p = p + 1) begin : pixel
            for (ch = 0; ch < 3; ch = ch + 1) begin : channel
                engine_blend u_blend (
                    .clk(clk),
                    .en(core_en),
                    .src(s3_src[p*32+ch*8 +: 8]),
                    .dst(s3_dst[p*24+ch*8 +: 8]),
                    .a(s3_alpha[p*8 +: 8]),
                    .c(row_const_coeff),
                    .eq(row_eq),
                    .out(s6_color[p*24+ch*8 +: 8]));
            end
        end
    endgenerate

    // S4 标记：与核第 1 级的乘积寄存器同拍
`ifdef GFX_KEY
    reg [3:0] s4_key_hit;
`endif
    reg [3:0] s4_mask;
    reg s4_valid;
    always @(posedge clk) begin
        if (rst) begin
`ifdef GFX_KEY
            s4_key_hit <= 4'd0;
`endif
            s4_mask <= 4'd0;
            s4_valid <= 1'b0;
        end else if (pipe_en) begin
`ifdef GFX_KEY
            s4_key_hit <= s3_key_hit;
`endif
            s4_mask <= s3_mask;
            s4_valid <= s3_valid;
        end
    end

    // S5 标记：与核第 2 级的分子寄存器同拍
`ifdef GFX_KEY
    reg [3:0] s5_key_hit;
`endif
    reg [3:0] s5_mask;
    reg s5_valid;
    always @(posedge clk) begin
        if (rst) begin
`ifdef GFX_KEY
            s5_key_hit <= 4'd0;
`endif
            s5_mask <= 4'd0;
            s5_valid <= 1'b0;
        end else if (pipe_en) begin
`ifdef GFX_KEY
            s5_key_hit <= s4_key_hit;
`endif
            s5_mask <= s4_mask;
            s5_valid <= s4_valid;
        end
    end

    // S6 标记：与核第 3 级的颜色输出同拍
`ifdef GFX_KEY
    reg [3:0] s6_key_hit;
`endif
    reg [3:0] s6_mask;
    reg s6_valid;
    always @(posedge clk) begin
        if (rst) begin
`ifdef GFX_KEY
            s6_key_hit <= 4'd0;
`endif
            s6_mask <= 4'd0;
            s6_valid <= 1'b0;
        end else if (pipe_en) begin
`ifdef GFX_KEY
            s6_key_hit <= s5_key_hit;
`endif
            s6_mask <= s5_mask;
            s6_valid <= s5_valid;
        end
    end

    // S7：颜色与不写标记合并成写数据
    reg [3:0] ws_key_lanes;
`ifdef GFX_KEY
    wire [3:0] s6_no_write = s6_key_hit | ~s6_mask; // 色键命中或行外
    wire [3:0] ws_key_lanes_next = s6_key_hit & s6_mask; // 只计行内命中
`else
    wire [3:0] s6_no_write = ~s6_mask;
    wire [3:0] ws_key_lanes_next = 4'd0;
`endif
    // 写数据：RGB + 不写标记
    wire [127:0] ws_tdata_next = {
        (s6_no_write[3] ? 8'h80 : 8'h00), s6_color[95:72],
        (s6_no_write[2] ? 8'h80 : 8'h00), s6_color[71:48],
        (s6_no_write[1] ? 8'h80 : 8'h00), s6_color[47:24],
        (s6_no_write[0] ? 8'h80 : 8'h00), s6_color[23:0]};

    // S7 输出寄存器：与写流 valid 同沿
    always @(posedge clk) begin
        if (rst) begin
            ws_tdata <= 128'd0;
            ws_tvalid <= 1'b0;
`ifdef GFX_KEY
            ws_key_lanes <= 4'd0;
`endif
            // 无色键构型复位时不写这一位，首次推进才置零
        end else if (pipe_en) begin
            ws_tdata <= ws_tdata_next;
            ws_tvalid <= s6_valid;
            ws_key_lanes <= ws_key_lanes_next;
        end
    end

    // 色键统计：只计行内命中，握手拍才有效
    assign perf_key_lanes = ws_key_lanes;
    assign perf_key_valid = write_fire;

    // 行完成：末拍交接后释放半
    reg [7:0] out_beat_count;
    assign row_done = write_fire && (out_beat_count == (row_dst_beats - 8'd1)); // 看已交接拍数
    assign row_done_half = row_half;
    always @(posedge clk) begin
        if (rst) out_beat_count <= 8'd0;
        else if (row_take) out_beat_count <= 8'd0;
        else if (write_fire) out_beat_count <= out_beat_count + 8'd1;
    end
endmodule

`default_nettype wire
