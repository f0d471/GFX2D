// display_fifo：扫描输出的异步 FIFO（写侧 core_clk，读侧像素时钟）
//
// 里面是仓内已有的厂商源码 rtl/vendor/ddr3_controller/common/efx_fifo_v2.3/efx_fifo_wrapper.v，
// 本模块只做三件事：把定死的参数写在一处、把厂商端口收成扫描输出用得到的那几个、
// 把 rst_busy 在写侧同步一次。手写异步 FIFO 是退路，触发条件见
// Engineering/Todo/hw/扫描输出.md 的 D4（水位或复位语义满足不了 D7，或综合后没推断成片上 RAM 块）。
//
// 定值（2026-09-20 实测，见该文件第 4 节与 2.2 节的「FIFO 语义」表）：
//   DEPTH = 512 拍（约 3 行）、DATA_WIDTH = 128 位、SYNC_CLK = 0（真异步）、
//   OUTPUT_REG = 0（读延迟恰为 1 个 rd 周期，像素侧的对齐 D=1 靠它）、
//   ASYM_WIDTH_RATIO = 4（读写同宽）、RAM_STYLE = "block_ram"、SYNC_STAGE = 2。
//   BYPASS_RESET_SYNC 必须显式写 0：置 1 会把内部复位接到 wrapper 根本没连线的
//   a_wr_rst_i / a_rd_rst_i 上，还把 rst_busy 写死成 0。
//
// 三条实测出来的纪律，用这一层的人必须遵守：
//   1. a_rst_i 是唯一的复位，两侧由厂商内部各一套两级同步器一起复位；wrapper 的
//      a_wr_rst_i / a_rd_rst_i 没有接到 FIFO，不要去接它们。
//   2. 复位窗口里 FIFO 不拦写（wr_en_int = we & ~wr_full，且 full_o 在复位期间为 0），
//      写进去的字会盖掉 ram[0] 而指针被钉在 0。所以复位期间 wr_en 必须由调用方压住。
//   3. rst_busy 是 wr_rst_int | rd_rst_int 的组合，含像素域的寄存器。在 core_clk 里
//      直接用它就是一条没有同步器、SDC 又切掉了的跨域路径（STA 不会报）。
//      本模块给出同步后的 rst_busy_sync，调用方判它连续为低再加本域余量。
//
// 水位语义：两个水位都是 RAM 里的指针差，**不含在途的 AXI 拍**。
//   wr_level（写侧）用滞后的读指针，占用偏高 → 算出来的空位偏小，发行判据保守；
//   rd_level（读侧）用滞后的写指针，占用偏低 → 水位判据量到的是下界。
`timescale 1ns/1ps
`default_nettype none

module display_fifo #(
    parameter integer DEPTH      = 512,
    parameter integer DATA_WIDTH = 128
)(
    // ---- 写侧：core_clk（取数侧写入）----
    input  wire                    wr_clk,
    input  wire                    wr_rst,        // a_rst_i，两侧一起复位；保持 ≥2 个 wr 周期
    input  wire                    wr_en,
    input  wire [DATA_WIDTH-1:0]   wr_data,
    output wire [9:0]              wr_level,      // wr_datacount_o，偏高（保守）
    output wire                    wr_full,
    output wire                    wr_overflow,
    output wire                    rst_busy_sync, // rst_busy 在 wr_clk 里两级同步后的电平

    // ---- 读侧：像素时钟（display_out 弹数）----
    input  wire                    rd_clk,
    input  wire                    rd_en,
    output wire [DATA_WIDTH-1:0]   rd_data,
    output wire                    rd_valid,      // 与 rd_data 同拍
    output wire [9:0]              rd_level,      // rd_datacount_o，偏低
    output wire                    rd_empty,
    output wire                    rd_underflow
);

    wire        rst_busy;
    wire [9:0]  datacount_unused;

    efx_fifo_wrapper #(
        .FAMILY            ("TITANIUM"),
        .SYNC_CLK          (0),
        .BYPASS_RESET_SYNC (0),          // 必须 0，见文件头
        .SYNC_STAGE        (2),
        .MODE              ("STANDARD"),
        .DEPTH             (DEPTH),
        .DATA_WIDTH        (DATA_WIDTH),
        .PIPELINE_REG      (1),
        .OUTPUT_REG        (0),          // 读延迟 1 个 rd 周期
        .ASYM_WIDTH_RATIO  (4),
        .RAM_STYLE         ("block_ram")
    ) u_fifo (
        .a_rst_i        (wr_rst),
        .a_wr_rst_i     (1'b0),          // wrapper 内部没连线，接常量；不要指望它
        .a_rd_rst_i     (1'b0),
        .clk_i          (1'b0),          // SYNC_CLK=0，这个时钟不用
        .wr_clk_i       (wr_clk),
        .rd_clk_i       (rd_clk),
        .wr_en_i        (wr_en),
        .rd_en_i        (rd_en),
        .wdata          (wr_data),
        .almost_full_o  (),
        .prog_full_o    (),
        .full_o         (wr_full),
        .overflow_o     (wr_overflow),
        .wr_ack_o       (),
        .datacount_o    (datacount_unused),
        .wr_datacount_o (wr_level),
        .empty_o        (rd_empty),
        .almost_empty_o (),
        .prog_empty_o   (),
        .underflow_o    (rd_underflow),
        .rd_valid_o     (rd_valid),
        .rdata          (rd_data),
        .rd_datacount_o (rd_level),
        .rst_busy       (rst_busy)
    );

    // rst_busy 的同步（订正 2026-09-20 复核第 1 条）：它是两个时钟域寄存器的组合，
    // 在写侧直接用之前先过两级。这两个 FF 要带 async_reg，综合时不会被搬。
    (* async_reg = "true" *) reg [1:0] rst_busy_sync_r = 2'b00;
    always @(posedge wr_clk) begin
        rst_busy_sync_r <= {rst_busy_sync_r[0], rst_busy};
    end
    assign rst_busy_sync = rst_busy_sync_r[1];

endmodule

`default_nettype wire
