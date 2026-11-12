// syn_engine_geom：rtl/engine/core/engine_geom.v 单独综合的顶层
//
// 只给综合用，不进网表。与研究探针及各层单独综合同一套做法：
//   * 128 位 LFSR 造命令激励（含裁剪框）、三个方向的 ready 与 row_free[1:0]；
//     每个输入都要是变量，接成常数的输入会把它后面的逻辑一起折掉（裁剪框踩过一次，见下面例化处）；
//   * 被测的每一个输出按位异或进 128 位观测寄存器，观测寄存器再回灌 LFSR，
//     否则综合会把没人看的逻辑整块删掉（踩过一次：只观测控制位，两个核的输出 FIFO 被剪光）。
// 观测总线 253 位，折进 128 位要数清位宽（探针首版折错过一次）。
`timescale 1ns/1ps
`default_nettype none

module syn_engine_geom (
    input  wire         clk,
    input  wire         rst,
    output reg  [127:0] obs
);
    reg [127:0] lfsr;
    always @(posedge clk) begin
        if (rst) lfsr <= 128'h0123456789ABCDEF_FEDCBA9876543210;
        else     lfsr <= {lfsr[126:0], lfsr[127] ^ lfsr[125] ^ lfsr[124] ^ lfsr[121]};
    end

    reg start_r;
    always @(posedge clk) if (rst) start_r <= 1'b0; else start_r <= (lfsr[23:16] == 8'h5A);

    wire [31:0] rd_addr, wr_addr;
    wire [11:0] rd_beats, wr_beats;
    wire [1:0]  rd_buf;
    wire [3:0]  rd_cidx, wr_cidx, done_cidx;
    wire        rd_valid, wr_valid, wr_last_row, busy, cmd_done;
    wire [15:0] sb_row, sb_py;
    wire [10:0] sb_npx;
    wire [7:0]  sb_beats, sb_op, sb_c;
    wire [1:0]  sb_lead_dst, sb_lead_src;
    wire [3:0]  sb_eq, sb_sfmt;
    wire        sb_key_en, sb_read_dst, sb_valid;
    wire [31:0] sb_key, sb_w4;

    engine_geom u_geom (
        .clk(clk), .rst(rst),
        .start(start_r),
        .op(lfsr[8] ? 8'd1 : 8'd2),
        .eq(lfsr[12:9]),
        .key_en(lfsr[13]),
        .key_w5(lfsr[31:0]),
        .const_w4({lfsr[120:97], lfsr[7:0]}),
        .c(lfsr[31:24]),
        .sfmt(lfsr[16:13]),
        .src_base({lfsr[119:96], 8'h00}), .band_enable(1'b0), .band_y0(16'd0), .src_tex(1'b0),
        .src_w(lfsr[25:10]),
        .src_h(lfsr[41:26]),
        .src_slog2(lfsr[45:41]),
        .dst_base({lfsr[95:72], 8'h00}),
        .dst_w(lfsr[57:42]),
        .dst_h(lfsr[73:58]),
        .dst_slog2(lfsr[77:73]),
        .src_x(lfsr[15:0]),
        .src_y(lfsr[47:32]),
        .dst_x(lfsr[63:48]),
        .dst_y(lfsr[95:80]),
        .w(lfsr[121:106]),
        .h(lfsr[105:90]),
        // 裁剪框四个字段也要由 LFSR 驱动：原来接成常数 (0,0,639,479)，
        // 求交里裁剪框那两级比较恒不成立、被常量折叠，比较链短了两级，
        // 资源与时序都偏乐观（2026-10-01 复核）。整机里它来自控制层的 SET_CLIP 状态。
        .clip_x0(lfsr[111:96]), .clip_y0(lfsr[127:112]),
        .clip_x1(lfsr[79:64]),  .clip_y1(lfsr[95:80]),
        .cidx(lfsr[3:0]),
        .row_free(lfsr[8:7]),
        .busy(busy), .cmd_done(cmd_done), .done_cidx(done_cidx),
        .rd_addr(rd_addr), .rd_beats(rd_beats), .rd_buf(rd_buf),
        .rd_cidx(rd_cidx), .rd_valid(rd_valid), .rd_ready(lfsr[4]),
        .wr_addr(wr_addr), .wr_beats(wr_beats), .wr_cidx(wr_cidx),
        .wr_last_row(wr_last_row), .wr_valid(wr_valid), .wr_ready(lfsr[5]),
        .sb_row(sb_row), .sb_py(sb_py), .sb_npx(sb_npx), .sb_beats(sb_beats),
        .sb_lead_dst(sb_lead_dst), .sb_lead_src(sb_lead_src),
        .sb_op(sb_op), .sb_eq(sb_eq), .sb_key_en(sb_key_en), .sb_key(sb_key),
        .sb_w4(sb_w4), .sb_c(sb_c), .sb_sfmt(sb_sfmt), .sb_read_dst(sb_read_dst),
        .sb_valid(sb_valid), .sb_ready(lfsr[6])
    );

    // 253 位：低位 128 + 高位 125（补 3 位零凑满 128）
    wire [252:0] obsv = {
        busy, cmd_done, done_cidx,
        rd_addr, rd_beats, rd_buf, rd_cidx, rd_valid,
        wr_addr, wr_beats, wr_cidx, wr_last_row, wr_valid,
        sb_row, sb_py, sb_npx, sb_beats, sb_lead_dst, sb_lead_src,
        sb_op, sb_eq, sb_key_en, sb_key, sb_w4, sb_c, sb_sfmt,
        sb_read_dst, sb_valid
    };

    always @(posedge clk) begin
        if (rst) obs <= 128'd0;
        else     obs <= obs ^ obsv[127:0] ^ {3'b0, obsv[252:128]} ^ lfsr;
    end
endmodule

`default_nettype wire
