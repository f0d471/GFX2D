// display_top：扫描输出（display）对外的唯一入口
//
// 里面三块（见 Engineering/Todo/hw/扫描输出.md 2.2 节）：
//   display_fetch  取数侧，core_clk 域：AXI 读主机 + 按水位取数 + APB 寄存器 + 统计
//   display_fifo   跨时钟的异步 FIFO（厂商标本封装），写侧 core_clk、读侧像素时钟
//   display_out    像素侧，像素时钟域：唯一的视频时序发生器（color_bar_rgb）、拍拆像素、
//                  D=1 对齐、SRC 二选一、水位与欠载观测、帧快照
//
// 对顶层露四样：APB 从口、AXI 读主机口（Axi_Mux 的 S0）、中断，以及给 hdmi_tx 的像素与同步。
// TMDS 由 hdmi_tx 出（D10 取 B′：hdmi_tx 是纯编码封装，彩条搬到了 display_out）。
// 顶层管脚与 fpga 的 peri.xml 都不变。
//
// 跨域全部走结构化的通道，没有裸打拍：
//   core -> pixel：src_sel（一个电平，只在场起始变化，两级同步后在像素侧再锁存一次）、
//                  clear_toggle（CTRL.CLEAR，toggle 加两级同步，清像素侧的累计值）
//   pixel -> core：fs_toggle（场起始，toggle）、snap_toggle + 快照总线（toggle/ack）
// 像素侧的复位 rst_pixel_n 由 hdmi_tx 的 vid_rst_n 给，两个模块共用一个复位源。
`timescale 1ns/1ps
`default_nettype none

module display_top #(
    parameter H_ACTIVE = 640,
    parameter H_FP     = 16,
    parameter H_SYNC   = 96,
    parameter H_BP     = 48,
    parameter V_ACTIVE = 480,
    parameter V_FP     = 10,
    parameter V_SYNC   = 2,
    parameter V_BP     = 33,
    parameter SYNC_POL = 1'b0,
    parameter [31:0]  FRONT_BASE_DEF = 32'h0200_0000,
    parameter [31:0]  BACK_BASE_DEF  = 32'h0240_0000,
    parameter integer LINE_BEATS     = 160,
    parameter integer DEPTH          = 512
)(
    input  wire         clk_core,       // core_clk（100MHz）
    input  wire         rst_core,       // 同步复位，高有效（顶层的 soc_system_reset）
    input  wire         clk_pixel,      // hdmi_tx_slow_clk（25.2MHz）
    input  wire         rst_pixel_n,    // hdmi_tx 的 vid_rst_n

    // ---- APB 从口（窗口内 12 位偏移）----
    input  wire [11:0]  paddr,
    input  wire         psel,
    input  wire         penable,
    input  wire         pwrite,
    input  wire [31:0]  pwdata,
    output wire [31:0]  prdata,
    output wire         pready,
    output wire         pslverr,

    // ---- AXI 读主机口（接 Axi_Mux 的 S0）----
    output wire [31:0]  ar_addr,
    output wire [7:0]   ar_len,
    output wire [2:0]   ar_size,
    output wire [1:0]   ar_burst,
    output wire [1:0]   ar_lock,
    output wire [7:0]   ar_id,
    output wire         ar_valid,
    input  wire         ar_ready,
    input  wire [7:0]   r_id,
    input  wire [127:0] r_data,
    input  wire         r_last,
    input  wire [1:0]   r_resp,
    input  wire         r_valid,
    output wire         r_ready,

    // ---- 中断（场起始，接 SoC 的 userInterruptA）----
    output wire         irq,

    // ---- 给 hdmi_tx 的像素与同步（同拍）----
    output wire [23:0]  vid_rgb,
    output wire         vid_hs,
    output wire         vid_vs,
    output wire         vid_de
);

    // FIFO 两侧
    wire         fifo_wr_en;
    wire [127:0] fifo_wr_data;
    wire [9:0]   fifo_wr_level;
    wire         fifo_wr_full;
    wire         fifo_wr_overflow;
    wire         fifo_rst;
    wire         fifo_rst_busy;

    wire         rd_en;
    wire [127:0] rd_data;
    wire         rd_valid;
    wire [9:0]   rd_level;
    wire         rd_empty;
    wire         rd_underflow;

    // 跨域握手
    wire         fs_toggle;
    wire         src_sel;
    wire         clear_toggle;
    wire         snap_toggle;
    wire         snap_ack;
    wire [15:0]  snap_frame;
    wire [9:0]   snap_min_level;
    wire [31:0]  snap_zero_hits;
    wire [31:0]  snap_underflow_beats;
    wire [31:0]  snap_frame_beats;
    wire [9:0]   snap_end_level;

    display_fifo #(
        .DEPTH      (DEPTH),
        .DATA_WIDTH (128)
    ) u_fifo (
        .wr_clk        (clk_core),
        .wr_rst        (fifo_rst),
        .wr_en         (fifo_wr_en),
        .wr_data       (fifo_wr_data),
        .wr_level      (fifo_wr_level),
        .wr_full       (fifo_wr_full),
        .wr_overflow   (fifo_wr_overflow),
        .rst_busy_sync (fifo_rst_busy),
        .rd_clk        (clk_pixel),
        .rd_en         (rd_en),
        .rd_data       (rd_data),
        .rd_valid      (rd_valid),
        .rd_level      (rd_level),
        .rd_empty      (rd_empty),
        .rd_underflow  (rd_underflow)
    );

    display_fetch #(
        .FRONT_BASE_DEF (FRONT_BASE_DEF),
        .BACK_BASE_DEF  (BACK_BASE_DEF),
        .V_ACTIVE       (V_ACTIVE),
        .LINE_BEATS     (LINE_BEATS),
        .DEPTH          (DEPTH)
    ) u_fetch (
        .clk        (clk_core),
        .rst        (rst_core),
        .paddr      (paddr),
        .psel       (psel),
        .penable    (penable),
        .pwrite     (pwrite),
        .pwdata     (pwdata),
        .prdata     (prdata),
        .pready     (pready),
        .pslverr    (pslverr),
        .ar_addr    (ar_addr),
        .ar_len     (ar_len),
        .ar_size    (ar_size),
        .ar_burst   (ar_burst),
        .ar_lock    (ar_lock),
        .ar_id      (ar_id),
        .ar_valid   (ar_valid),
        .ar_ready   (ar_ready),
        .r_id       (r_id),
        .r_data     (r_data),
        .r_last     (r_last),
        .r_resp     (r_resp),
        .r_valid    (r_valid),
        .r_ready    (r_ready),
        .fifo_wr_en      (fifo_wr_en),
        .fifo_wr_data    (fifo_wr_data),
        .fifo_wr_level   (fifo_wr_level),
        .fifo_wr_full    (fifo_wr_full),
        .fifo_wr_overflow(fifo_wr_overflow),
        .fifo_rst        (fifo_rst),
        .fifo_rst_busy   (fifo_rst_busy),
        .fs_toggle       (fs_toggle),
        .src_sel         (src_sel),
        .clear_toggle    (clear_toggle),
        .snap_toggle     (snap_toggle),
        .snap_ack        (snap_ack),
        .snap_frame      (snap_frame),
        .snap_min_level  (snap_min_level),
        .snap_zero_hits  (snap_zero_hits),
        .snap_underflow_beats (snap_underflow_beats),
        .snap_frame_beats     (snap_frame_beats),
        .snap_end_level  (snap_end_level),
        .irq             (irq)
    );

    display_out #(
        .H_ACTIVE (H_ACTIVE),
        .H_FP     (H_FP),
        .H_SYNC   (H_SYNC),
        .H_BP     (H_BP),
        .V_ACTIVE (V_ACTIVE),
        .V_FP     (V_FP),
        .V_SYNC   (V_SYNC),
        .V_BP     (V_BP),
        .SYNC_POL (SYNC_POL)
    ) u_out (
        .clk        (clk_pixel),
        .rst_n      (rst_pixel_n),
        .src_sel      (src_sel),
        .clear_toggle (clear_toggle),
        .rd_data    (rd_data),
        .rd_level   (rd_level),
        .rd_empty   (rd_empty),
        .rd_en      (rd_en),
        .vid_rgb    (vid_rgb),
        .vid_hs     (vid_hs),
        .vid_vs     (vid_vs),
        .vid_de     (vid_de),
        .fs_toggle  (fs_toggle),
        .snap_toggle(snap_toggle),
        .snap_ack   (snap_ack),
        .snap_frame (snap_frame),
        .snap_min_level (snap_min_level),
        .snap_zero_hits (snap_zero_hits),
        .snap_underflow_beats (snap_underflow_beats),
        .snap_frame_beats     (snap_frame_beats),
        .snap_end_level (snap_end_level)
    );

endmodule

`default_nettype wire
