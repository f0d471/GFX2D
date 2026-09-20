// HDMI 发送通路：像素 -> TMDS 编码 -> 送 LVDS TX 做 10:1 串行化
//
// 2026-09-20 按 D10 的 B′ 改成纯编码封装（Engineering/Todo/hw/扫描输出.md 2.1 节 D10）：
// 彩条 `color_bar_rgb` 搬进 `rtl/display/display_out.v`，由它给出唯一一份视频时序
// （`vid_hs`/`vid_vs`/`vid_de`）与像素（`vid_rgb`），本模块只负责复位同步、TMDS 编码与
// 板级 P/N 取反。这样「场」只有一个真源，彩条模式与帧缓冲模式共用同一个编码器，
// 两条通路唯一的差别就是像素来源，出问题时锅落在哪一侧一目了然。
//
// `vid_rst_n` 是 PLL 锁定后同步释放的复位，导出去给像素侧共用，避免两个模块各写一套
// 复位同步器而在释放时刻上差几拍。
//
// 行场时序参数仍留在本模块的参数表里（不动对外参数表），但按 B′ 它们已经不再被使用：
// 真正生效的一份在 `display_out`（彩条在那里例化），两处的默认值必须一致。
// 本模块不再例化 `color_bar_rgb`；编码器三个官方文件（`dvi_encoder.v`、`encode.v`）
// 与 P/N 取反一行未动。
//
// 默认 640x480@60（VESA DMT，像素时钟 25.2MHz，行场同步低有效）。
// 串行化由 Interface Designer 中的 4 路 LVDS TX 完成（半速率、宽度 10），
// 本模块输出的是每路 10 位并行字，时钟为像素时钟 hdmi_tx_slow_clk。
// 板上 HDMI TX 差分对的 P/N 与 FPGA 管脚对调，输出前整体取反。
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
