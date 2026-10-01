// syn_probe_pix：B4 研究探针 probe_pix.v 单独综合的顶层
//
// 只给综合用，不进网表。照 B1 3.8 节、B2 3.13 节、B3（syn_engine_geom.v）的做法：
//   * 128 位 LFSR 造全部输入，包括行缓冲的预填写口（lb_we/lb_waddr/lb_wdata）——
//     写口接常数会让两块 128 位 RAM 被当成永不写入的存储优化掉，14 块的对账就空了；
//   * 被测的每一个输出按位异或进 128 位观测寄存器，观测寄存器再回灌 LFSR，
//     否则综合把没人看的逻辑整块删掉（B2 踩过：只观测控制位，核的输出 FIFO 被剪光）；
//   * 写流是 151 位（128 数据 + 16 keep + tlast/tvalid），折叠时数清位宽。
`timescale 1ns/1ps
`default_nettype none

module syn_probe_pix (
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

    wire [127:0] ws_tdata;
    wire [15:0]  ws_tkeep;
    wire         ws_tlast, ws_tvalid, busy;
    wire [1:0]   dbg_off, dbg_st;

    probe_pix_path u_path (
        .clk(clk), .rst(rst),
        .start(start_r),
        .half(lfsr[1:0]),
        .is_blit(lfsr[2]),
        .eq(lfsr[5:3]),
        .key_en(lfsr[6]),
        .key_w5(lfsr[31:0]),
        .w4({lfsr[120:113], lfsr[47:24]}),
        .c(lfsr[55:48]),
        .sfmt(lfsr[57:56]),
        .read_dst(lfsr[58]),
        .npx({1'b0, lfsr[68:59]}),
        .lead_src(lfsr[70:69]),
        .lead_dst(lfsr[72:71]),
        .seg1_beats(lfsr[80:73]),
        .seg2_beats(lfsr[88:81]),
        .lb_we(lfsr[89]),
        .lb_src_sel(lfsr[90]),
        .lb_waddr({lfsr[92:91], lfsr[100:94]}),
        .lb_wdata(lfsr[127:0] ^ {lfsr[63:0], lfsr[63:0]}),
        .ws_tdata(ws_tdata), .ws_tkeep(ws_tkeep), .ws_tlast(ws_tlast),
        .ws_tvalid(ws_tvalid), .ws_tready(lfsr[101]),
        .busy(busy), .dbg_off(dbg_off), .dbg_st(dbg_st));

    // 151 位：128 数据 + 16 keep + tlast + tvalid + busy + 2 dbg
    wire [150:0] obsv = {ws_tdata, ws_tkeep, ws_tlast, ws_tvalid, busy, dbg_off, dbg_st};

    always @(posedge clk) begin
        if (rst) obs <= 128'd0;
        else     obs <= obs ^ obsv[127:0] ^ {23'b0, obsv[150:128]} ^ lfsr;
    end
endmodule

`default_nettype wire
