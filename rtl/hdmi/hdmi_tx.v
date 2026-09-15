// HDMI 发送通路：视频时序与八色竖条 -> TMDS 编码 -> 送 LVDS TX 做 10:1 串行化
//
// 默认 640x480@60（VESA DMT，像素时钟 25.2MHz，行场同步低有效）。
// 串行化由 Interface Designer 中的 4 路 LVDS TX 完成（半速率、宽度 10），
// 本模块输出的是每路 10 位并行字，时钟为像素时钟 hdmi_tx_slow_clk。
// 板上 HDMI TX 差分对的 P/N 与 FPGA 管脚对调，输出前整体取反。
//
// 子模块取自官方 03_hdmi_tx_demo：color_bar_rgb（v3.0）、dvi_encoder、encode。
`timescale 1ps/1ps
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

wire        vid_hs;
wire        vid_vs;
wire        vid_de;
wire [23:0] vid_rgb;                   // {R, G, B}

color_bar_rgb #(
    .HS_POLORY     (SYNC_POL),
    .VS_POLORY     (SYNC_POL),
    .SYMBOL_WIDTH  (8),
    .SYMBOL_NUM    (3),
    .PAR_PIXEL_NUM (1),
    .HFP           (H_FP),
    .HST           (H_SYNC),
    .HACT          (H_ACTIVE),
    .HBP           (H_BP),
    .VFP           (V_FP),
    .VST           (V_SYNC),
    .VACT          (V_ACTIVE),
    .VBP           (V_BP),
    .TEST_MODE     (2'd2)              // 2'd2：白黄青绿品红蓝黑八色竖条
) u_color_bar (
    .clk        (clk_pixel),
    .rst_n      (rst_n),
    .i_cfg_vid  (24'd0),
    .h_cnt      (),
    .v_cnt      (),
    .hs         (vid_hs),
    .vs         (vid_vs),
    .de         (vid_de),
    .o_vid_data (vid_rgb)
);

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
