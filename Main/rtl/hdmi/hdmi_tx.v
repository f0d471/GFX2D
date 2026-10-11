`timescale 1ps/1ps
//==============================================================================================//
// FILE: hdmi_tx.v
//
// DESCRIPTION: HDMI 发送通路：像素 → TMDS 编码（官方 dvi_encoder，一行未动）→
//   板级 P/N 取反。彩条与时序在 display_out，本模块只做复位同步、编码与取反。
//
// NOTE:
//   1. vid_rst_n 是 PLL 锁定后同步释放的复位，导出给像素侧共用，避免两套同步器释放时刻差拍
//   2. 行场时序参数留在本模块参数表但已不被使用，生效的一份在 display_out，两处默认值必须一致
//   3. 默认 640x480@60（VESA DMT，25.2MHz，同步低有效）；串行化由 4 路 LVDS TX 完成，
//      本模块输出每路 10 位并行字；板上 TX 差分对 P/N 对调，输出前整体取反
//==============================================================================================//
module hdmi_tx #(
    parameter H_ACTIVE = 640,
    parameter H_FP     = 16,
    parameter H_SYNC   = 96,
    parameter H_BP     = 48,
    parameter V_ACTIVE = 480,
    parameter V_FP     = 10,
    parameter V_SYNC   = 2,
    parameter V_BP     = 33,
    parameter SYNC_POL = 1'b0          // 1'b0：同步脉冲低有效；1'b1：高有效
)(
    input        clk_pixel,            // 像素时钟，接 hdmi_tx_slow_clk
    input        pll_locked,           // HDMI PLL 锁定；为低时整条通路保持复位
    // 像素来源：由 display_out 给，四个信号同拍（{R,G,B}，vid_de 为有效区）
    input  [23:0] vid_rgb,             // 接 display_out 的 vid_rgb
    input        vid_hs,
    input        vid_vs,
    input        vid_de,
    output       vid_rst_n,            // 复位同步结果，给像素侧共用
    output [9:0] tmds_clk,             // 接 tmds_tx_clk_TX_DATA
    output [9:0] tmds_data0,           // 接 tmds_tx_data0_TX_DATA（蓝，携带 HS/VS）
    output [9:0] tmds_data1,           // 接 tmds_tx_data1_TX_DATA（绿）
    output [9:0] tmds_data2            // 接 tmds_tx_data2_TX_DATA（红）
);

// 复位同步：PLL 失锁时异步复位，锁定后在像素时钟下同步释放
(* async_reg = "true" *) reg [2:0] rst_sync = 3'b000;
always @(posedge clk_pixel or negedge pll_locked) begin
    if (!pll_locked)
        rst_sync <= 3'b000;
    else
        rst_sync <= {rst_sync[1:0], 1'b1};
end
wire rst_n = rst_sync[2];
assign vid_rst_n = rst_n;

wire [9:0] enc_clk;
wire [9:0] enc_data0;
wire [9:0] enc_data1;
wire [9:0] enc_data2;

dvi_encoder u_dvi_encoder (
    .pixelclk   (clk_pixel),
    .rstin      (~rst_n),
    .blue_din   (vid_rgb[7:0]),
    .green_din  (vid_rgb[15:8]),
    .red_din    (vid_rgb[23:16]),
    .hsync      (vid_hs),
    .vsync      (vid_vs),
    .de         (vid_de),
    .tmds_data0 (enc_data0),
    .tmds_data1 (enc_data1),
    .tmds_data2 (enc_data2),
    .tmds_clk   (enc_clk)
);

// 板上 P/N 对调，取反后正好还原
assign tmds_clk   = ~enc_clk;
assign tmds_data0 = ~enc_data0;
assign tmds_data1 = ~enc_data1;
assign tmds_data2 = ~enc_data2;

endmodule
