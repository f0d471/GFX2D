// gfx2d 顶层
//
// 组成：
//   u_soc        Sapphire RISC-V SoC（IP 生成，fpga/gfx2d_ti60/ip/soc/soc.v）
//   u_ddr3       DDR3 软核控制器（厂商加密源码，rtl/vendor/ddr3_controller/）
//   u_apb_probe  APB 探针寄存器，占位并验证 APB 通路（rtl/bus/apb_probe.v）
//   u_hdmi_tx    HDMI 发送，640x480@60 八色竖条（rtl/hdmi/hdmi_tx.v）
//
// 时钟（全部由 Interface Designer 里的 PLL 产生，见 fpga/gfx2d_ti60/README.md）：
//   core_clk           100MHz   PLL_TL0 <- clk_25m    SoC、DDR3 控制器 AXI 侧、APB
//   sdram_clk 等四个   400MHz   PLL_TR0 <- clk_25m    DDR3 PHY
//   hdmi_tx_slow_clk   25.2MHz  PLL_BL0 <- clk_27m    HDMI 像素时钟
//
// 连接：
//   SoC io_ddrA（AXI4，128 位）-> DDR3 控制器的 AXI 从口
//   SoC APB Slave 0（CPU 地址 0xF810_0000，64KB 窗口）-> apb_probe
//   HDMI 通路目前与 SoC、DDR3 没有数据交互，画面来自 hdmi_tx 内部的彩条发生器
//
// 端口名与 fpga/gfx2d_ti60/gfx2d_ti60.peri.xml 中的信号名一一对应，改一边要同步另一边。
`include "../vendor/ddr3_controller/ddr3_parameter.vh"

`timescale 1ps/1ps
`default_nettype none

module gfx2d_top (
    // PLL 输出与锁定标志
    input  wire                        core_clk,
    input  wire                        sdram_clk,
    input  wire                        rx_cal_clk,
    input  wire                        tx_cal_clk,
    input  wire                        tx_cal_clk_90edge,
    input  wire                        pll_locked,             // PLL_TR0（DDR3）
    input  wire                        user_pll_locked,        // PLL_TL0（core_clk）
    input  wire                        hdmi_pll_locked,        // PLL_BL0（HDMI）
    input  wire                        hdmi_tx_slow_clk,

    // DDR3 PHY 对 PLL_TR0 的动态调相
    output wire [2:0]                  pll_shift,
    output wire [4:0]                  pll_shift_sel,
    output wire                        pll_shift_ena,

    // UART0：板上 FT4232 的 C 通道，115200 8N1
    output wire                        system_uart_0_io_txd,
    input  wire                        system_uart_0_io_rxd,

    // GPIO0：4 位，接板上 LED0~3
    input  wire [3:0]                  soc_gpio_IN,
    output wire [3:0]                  soc_gpio_OUT,
    output wire [3:0]                  soc_gpio_OE,

    // SPI0：接配置 Flash（顶层只引出 data0/1，四线模式用不了）
    input  wire                        system_spi_0_io_data_0_IN,
    input  wire                        system_spi_0_io_data_1_IN,
    output wire                        system_spi_0_io_ss,
    output wire                        system_spi_0_io_sclk_write,
    output wire                        system_spi_0_io_data_0_OUT,
    output wire                        system_spi_0_io_data_0_OE,
    output wire                        system_spi_0_io_data_1_OUT,
    output wire                        system_spi_0_io_data_1_OE,

    // 硬 JTAG 用户 TAP（JTAG_USER1），供 OpenOCD 调试 CPU
    input  wire                        jtag_inst1_CAPTURE,
    input  wire                        jtag_inst1_DRCK,
    input  wire                        jtag_inst1_RESET,
    input  wire                        jtag_inst1_RUNTEST,
    input  wire                        jtag_inst1_SEL,
    input  wire                        jtag_inst1_SHIFT,
    input  wire                        jtag_inst1_TCK,
    input  wire                        jtag_inst1_TDI,
    input  wire                        jtag_inst1_TMS,
    input  wire                        jtag_inst1_UPDATE,
    output wire                        jtag_inst1_TDO,

    // DDR3 颗粒接口
    output wire                        ddr_ck_hi,
    output wire                        ddr_ck_lo,
    output wire                        ddr_reset_n,
    output wire [`CKE_WIDTH-1:0]       ddr_cke,
    output wire [`ROW_WIDTH-1:0]       ddr_addr,
    output wire [`BANK_WIDTH-1:0]      ddr_ba,
    output wire                        ddr_cas_n,
    output wire [`CS_WIDTH-1:0]        ddr_cs_n,
    output wire                        ddr_ras_n,
    output wire                        ddr_we_n,
    input  wire [`DQS_WIDTH-1:0]       ddr_dqs_in_hi,
    input  wire [`DQS_WIDTH-1:0]       ddr_dqs_in_lo,
    input  wire [`DQ_WIDTH-1:0]        ddr_dq_in_hi,
    input  wire [`DQ_WIDTH-1:0]        ddr_dq_in_lo,
    output wire [`DQS_WIDTH-1:0]       ddr_dqs_oe,
    output wire [`DQS_WIDTH-1:0]       ddr_dqs_oe_n,
    output wire [`DQ_WIDTH-1:0]        ddr_dq_oe,
    output wire [`DQS_WIDTH-1:0]       ddr_dqs_out_hi,
    output wire [`DQS_WIDTH-1:0]       ddr_dqs_out_lo,
    output wire [`DQ_WIDTH-1:0]        ddr_dq_out_hi,
    output wire [`DQ_WIDTH-1:0]        ddr_dq_out_lo,
    output wire [`DM_WIDTH-1:0]        ddr_dm_hi,
    output wire [`DM_WIDTH-1:0]        ddr_dm_lo,
    output wire [`ODT_WIDTH-1:0]       ddr_odt,

    // HDMI TX：4 路 LVDS TX 的并行口，串行化在 Interface Designer 里完成
    output wire [9:0]                  tmds_tx_clk_TX_DATA,
    output wire                        tmds_tx_clk_TX_OE,
    output wire                        tmds_tx_clk_TX_RST,
    output wire [9:0]                  tmds_tx_data0_TX_DATA,
    output wire                        tmds_tx_data0_TX_OE,
    output wire                        tmds_tx_data0_TX_RST,
    output wire [9:0]                  tmds_tx_data1_TX_DATA,
    output wire                        tmds_tx_data1_TX_OE,
    output wire                        tmds_tx_data1_TX_RST,
    output wire [9:0]                  tmds_tx_data2_TX_DATA,
    output wire                        tmds_tx_data2_TX_OE,
    output wire                        tmds_tx_data2_TX_RST
);

// ---------------------------------------------------------------------------
// 复位
// ---------------------------------------------------------------------------
// SoC 与 DDR3 控制器都挂在 core_clk 上，两个 PLL 任意一个没锁定都保持复位。
// DDR3 控制器再多等 DDR_RST_DELAY 个 core_clk 周期（3ms），与官方参考设计相同。
localparam [19:0] DDR_RST_DELAY = 20'd300_000;

wire pll_all_locked = pll_locked & user_pll_locked;

reg [19:0] ddr_rst_cnt;
always @(posedge core_clk or negedge pll_all_locked) begin
    if (!pll_all_locked)
        ddr_rst_cnt <= 20'd0;
    else if (ddr_rst_cnt != DDR_RST_DELAY)
        ddr_rst_cnt <= ddr_rst_cnt + 20'd1;
end

wire ddr_rstn = (ddr_rst_cnt == DDR_RST_DELAY);

// SoC 输出的同步复位，给 APB 从机用
wire soc_system_reset;

// ---------------------------------------------------------------------------
// SoC 与 DDR3 控制器之间的 AXI4
// ---------------------------------------------------------------------------
// 两侧位宽不同，下面两处是有意的截断，与官方参考设计一致：
//   地址  SoC 32 位 -> 控制器 28 位。DDR3 只有 256MB，[31:28] 恒为 0；
//         CPU 看到的 DDR 窗口是 0x0000_1000~0x0FFF_FFFF，超出部分会地址回绕（实验 E2）。
//   ID    SoC 8 位 -> 控制器 4 位，读写响应回来时高 4 位补 0。
//         SoC 实际发出的 ID 是否会超过 15 还没有查证，接引擎前要确认（实验 E2 一并做）。
wire [31:0]                    soc_ddr_awaddr;
wire [7:0]                     soc_ddr_awid;
wire [7:0]                     soc_ddr_awlen;
wire [2:0]                     soc_ddr_awsize;
wire [1:0]                     soc_ddr_awburst;
wire                           soc_ddr_awlock;
wire                           soc_ddr_awvalid;
wire                           soc_ddr_awready;
wire [2:0]                     soc_ddr_awprot;
wire [3:0]                     soc_ddr_awcache;

wire [`AXI_DATA_WIDTH-1:0]     soc_ddr_wdata;
wire [`AXI_DATA_WIDTH/8-1:0]   soc_ddr_wstrb;
wire                           soc_ddr_wlast;
wire                           soc_ddr_wvalid;
wire                           soc_ddr_wready;

wire [`AXI_ID_WIDTH-1:0]       ddr_bid;
wire [1:0]                     ddr_bresp;
wire                           ddr_bvalid;
wire                           soc_ddr_bready;

wire [31:0]                    soc_ddr_araddr;
wire [7:0]                     soc_ddr_arid;
wire [7:0]                     soc_ddr_arlen;
wire [2:0]                     soc_ddr_arsize;
wire [1:0]                     soc_ddr_arburst;
wire                           soc_ddr_arlock;
wire                           soc_ddr_arvalid;
wire                           soc_ddr_arready;
wire [2:0]                     soc_ddr_arprot;
wire [3:0]                     soc_ddr_arcache;

wire [`AXI_ID_WIDTH-1:0]       ddr_rid;
wire [`AXI_DATA_WIDTH-1:0]     ddr_rdata;
wire [1:0]                     ddr_rresp;
wire                           ddr_rlast;
wire                           ddr_rvalid;
wire                           soc_ddr_rready;

// ---------------------------------------------------------------------------
// APB Slave 0
// ---------------------------------------------------------------------------
wire [15:0] apb_paddr;
wire        apb_psel;
wire        apb_penable;
wire        apb_pwrite;
wire [31:0] apb_pwdata;
wire [31:0] apb_prdata;
wire        apb_pready;
wire        apb_pslverr;

// ---------------------------------------------------------------------------
// DDR3 软核控制器
// ---------------------------------------------------------------------------
ddr3_top u_ddr3 (
    .axi_clk                (core_clk),
    .core_clk               (core_clk),
    .sdram_clk              (sdram_clk),
    .rx_cal_clk             (rx_cal_clk),
    .tx_cal_clk             (tx_cal_clk),
    .tx_cal_clk_90edge      (tx_cal_clk_90edge),
    .rstn                   (ddr_rstn),

    .pll_shift              (pll_shift),
    .pll_shift_sel          (pll_shift_sel),
    .pll_shift_ena          (pll_shift_ena),

    .ddr_ck_hi              (ddr_ck_hi),
    .ddr_ck_lo              (ddr_ck_lo),
    .ddr_cke                (ddr_cke),
    .ddr_reset_n            (ddr_reset_n),
    .ddr_cs_n               (ddr_cs_n),
    .ddr_ras_n              (ddr_ras_n),
    .ddr_cas_n              (ddr_cas_n),
    .ddr_we_n               (ddr_we_n),
    .ddr_addr               (ddr_addr),
    .ddr_ba                 (ddr_ba),
    .ddr_dqs_oe             (ddr_dqs_oe),
    .ddr_dqs_oe_n           (ddr_dqs_oe_n),
    .ddr_dq_oe              (ddr_dq_oe),
    .ddr_dqs_in_hi          (ddr_dqs_in_hi),
    .ddr_dqs_in_lo          (ddr_dqs_in_lo),
    .ddr_dq_in_hi           (ddr_dq_in_hi),
    .ddr_dq_in_lo           (ddr_dq_in_lo),
    .ddr_dqs_out_hi         (ddr_dqs_out_hi),
    .ddr_dqs_out_lo         (ddr_dqs_out_lo),
    .ddr_dq_out_hi          (ddr_dq_out_hi),
    .ddr_dq_out_lo          (ddr_dq_out_lo),
    .ddr_dm_hi              (ddr_dm_hi),
    .ddr_dm_lo              (ddr_dm_lo),
    .ddr_odt                (ddr_odt),

    // 自刷新、手动刷新、ZQ 校准这三个请求没有用到
    .app_sr_req             (1'b0),
    .app_ref_req            (1'b0),
    .app_zq_req             (1'b0),
    .app_sr_active          (),
    .app_ref_ack            (),
    .app_zq_ack             (),

    .s_axi_awid             (soc_ddr_awid[`AXI_ID_WIDTH-1:0]),
    .s_axi_awaddr           (soc_ddr_awaddr[`AXI_ADDR_WIDTH-1:0]),
    .s_axi_awlen            (soc_ddr_awlen),
    .s_axi_awsize           (soc_ddr_awsize),
    .s_axi_awburst          (soc_ddr_awburst),
    .s_axi_awlock           (soc_ddr_awlock),
    .s_axi_awcache          (soc_ddr_awcache),
    .s_axi_awprot           (soc_ddr_awprot),
    .s_axi_awqos            (4'h0),
    .s_axi_awvalid          (soc_ddr_awvalid),
    .s_axi_awready          (soc_ddr_awready),

    .s_axi_wdata            (soc_ddr_wdata),
    .s_axi_wstrb            (soc_ddr_wstrb),
    .s_axi_wlast            (soc_ddr_wlast),
    .s_axi_wvalid           (soc_ddr_wvalid),
    .s_axi_wready           (soc_ddr_wready),

    .s_axi_bid              (ddr_bid),
    .s_axi_bresp            (ddr_bresp),
    .s_axi_bvalid           (ddr_bvalid),
    .s_axi_bready           (soc_ddr_bready),

    .s_axi_arid             (soc_ddr_arid[`AXI_ID_WIDTH-1:0]),
    .s_axi_araddr           (soc_ddr_araddr[`AXI_ADDR_WIDTH-1:0]),
    .s_axi_arlen            (soc_ddr_arlen),
    .s_axi_arsize           (soc_ddr_arsize),
    .s_axi_arburst          (soc_ddr_arburst),
    .s_axi_arlock           (soc_ddr_arlock),
    .s_axi_arcache          (soc_ddr_arcache),
    .s_axi_arprot           (soc_ddr_arprot),
    .s_axi_arqos            (4'h0),
    .s_axi_arvalid          (soc_ddr_arvalid),
    .s_axi_arready          (soc_ddr_arready),

    .s_axi_rid              (ddr_rid),
    .s_axi_rdata            (ddr_rdata),
    .s_axi_rresp            (ddr_rresp),
    .s_axi_rlast            (ddr_rlast),
    .s_axi_rvalid           (ddr_rvalid),
    .s_axi_rready           (soc_ddr_rready),

    // 校准与训练的调试观测口，当前没有引出
    .wrlvl_dq_check         (),
    .rd_level_dqs_check     (),
    .rdlvl_shift            (),
    .wrlvl_shift            (),
    .init_cur_state         (),
    .idelay_ld              (),
    .mpr_rdlvl_dly          (),
    .ddr_debug_port         (),
    .cal_done               ()
);

// ---------------------------------------------------------------------------
// Sapphire SoC
// ---------------------------------------------------------------------------
soc u_soc (
    .io_asyncReset                      (~pll_all_locked),
    .io_systemClk                       (core_clk),
    .io_memoryClk                       (core_clk),
    .io_systemReset                     (soc_system_reset),
    .io_memoryReset                     (),

    // UART0
    .system_uart_0_io_txd               (system_uart_0_io_txd),
    .system_uart_0_io_rxd               (system_uart_0_io_rxd),

    // GPIO0
    .system_gpio_0_io_write             (soc_gpio_OUT),
    .system_gpio_0_io_read              (soc_gpio_IN),
    .system_gpio_0_io_writeEnable       (soc_gpio_OE),

    // SPI0
    .system_spi_0_io_sclk_write         (system_spi_0_io_sclk_write),
    .system_spi_0_io_ss                 (system_spi_0_io_ss),
    .system_spi_0_io_data_0_read        (system_spi_0_io_data_0_IN),
    .system_spi_0_io_data_0_write       (system_spi_0_io_data_0_OUT),
    .system_spi_0_io_data_0_writeEnable (system_spi_0_io_data_0_OE),
    .system_spi_0_io_data_1_read        (system_spi_0_io_data_1_IN),
    .system_spi_0_io_data_1_write       (system_spi_0_io_data_1_OUT),
    .system_spi_0_io_data_1_writeEnable (system_spi_0_io_data_1_OE),
    // data_2/3 顶层没有引出，四线模式用不了
    .system_spi_0_io_data_2_read        (1'b0),
    .system_spi_0_io_data_2_write       (),
    .system_spi_0_io_data_2_writeEnable (),
    .system_spi_0_io_data_3_read        (1'b0),
    .system_spi_0_io_data_3_write       (),
    .system_spi_0_io_data_3_writeEnable (),

    // 外部存储 AXI4：接 DDR3 控制器
    .io_ddrA_aw_payload_addr            (soc_ddr_awaddr),
    .io_ddrA_aw_payload_id              (soc_ddr_awid),
    .io_ddrA_aw_payload_len             (soc_ddr_awlen),
    .io_ddrA_aw_payload_size            (soc_ddr_awsize),
    .io_ddrA_aw_payload_burst           (soc_ddr_awburst),
    .io_ddrA_aw_payload_lock            (soc_ddr_awlock),
    .io_ddrA_aw_payload_cache           (soc_ddr_awcache),
    .io_ddrA_aw_payload_prot            (soc_ddr_awprot),
    .io_ddrA_aw_payload_qos             (),
    .io_ddrA_aw_payload_region          (),
    .io_ddrA_aw_valid                   (soc_ddr_awvalid),
    .io_ddrA_aw_ready                   (soc_ddr_awready),

    .io_ddrA_w_payload_data             (soc_ddr_wdata),
    .io_ddrA_w_payload_strb             (soc_ddr_wstrb),
    .io_ddrA_w_payload_last             (soc_ddr_wlast),
    .io_ddrA_w_valid                    (soc_ddr_wvalid),
    .io_ddrA_w_ready                    (soc_ddr_wready),

    .io_ddrA_b_payload_id               ({4'b0, ddr_bid}),
    .io_ddrA_b_payload_resp             (ddr_bresp),
    .io_ddrA_b_valid                    (ddr_bvalid),
    .io_ddrA_b_ready                    (soc_ddr_bready),

    .io_ddrA_ar_payload_addr            (soc_ddr_araddr),
    .io_ddrA_ar_payload_id              (soc_ddr_arid),
    .io_ddrA_ar_payload_len             (soc_ddr_arlen),
    .io_ddrA_ar_payload_size            (soc_ddr_arsize),
    .io_ddrA_ar_payload_burst           (soc_ddr_arburst),
    .io_ddrA_ar_payload_lock            (soc_ddr_arlock),
    .io_ddrA_ar_payload_cache           (soc_ddr_arcache),
    .io_ddrA_ar_payload_prot            (soc_ddr_arprot),
    .io_ddrA_ar_payload_qos             (),
    .io_ddrA_ar_payload_region          (),
    .io_ddrA_ar_valid                   (soc_ddr_arvalid),
    .io_ddrA_ar_ready                   (soc_ddr_arready),

    .io_ddrA_r_payload_id               ({4'b0, ddr_rid}),
    .io_ddrA_r_payload_data             (ddr_rdata),
    .io_ddrA_r_payload_resp             (ddr_rresp),
    .io_ddrA_r_payload_last             (ddr_rlast),
    .io_ddrA_r_valid                    (ddr_rvalid),
    .io_ddrA_r_ready                    (soc_ddr_rready),

    // APB Slave 0
    .io_apbSlave_0_PADDR                (apb_paddr),
    .io_apbSlave_0_PSEL                 (apb_psel),
    .io_apbSlave_0_PENABLE              (apb_penable),
    .io_apbSlave_0_PWRITE               (apb_pwrite),
    .io_apbSlave_0_PWDATA               (apb_pwdata),
    .io_apbSlave_0_PRDATA               (apb_prdata),
    .io_apbSlave_0_PREADY               (apb_pready),
    .io_apbSlave_0_PSLVERROR            (apb_pslverr),

    // 硬 JTAG 用户 TAP
    .jtagCtrl_tck                       (jtag_inst1_TCK),
    .jtagCtrl_tdi                       (jtag_inst1_TDI),
    .jtagCtrl_tdo                       (jtag_inst1_TDO),
    .jtagCtrl_enable                    (jtag_inst1_SEL),
    .jtagCtrl_capture                   (jtag_inst1_CAPTURE),
    .jtagCtrl_shift                     (jtag_inst1_SHIFT),
    .jtagCtrl_update                    (jtag_inst1_UPDATE),
    .jtagCtrl_reset                     (jtag_inst1_RESET),

    // 下面几组在 IP 里生成了但本设计没有使用，输入一律接常量，避免悬空。
    // 计划在裁剪 SoC 时关掉，见 doc/开工前准备情况.md。
    .userInterruptA                     (1'b0),

    .system_i2c_0_io_scl_read           (1'b1),
    .system_i2c_0_io_sda_read           (1'b1),
    .system_i2c_0_io_scl_write          (),
    .system_i2c_0_io_sda_write          (),

    .axiA_awready                       (1'b0),
    .axiA_wready                        (1'b0),
    .axiA_bvalid                        (1'b0),
    .axiA_bid                           (8'h0),
    .axiA_bresp                         (2'b00),
    .axiA_arready                       (1'b0),
    .axiA_rvalid                        (1'b0),
    .axiA_rdata                         (32'h0),
    .axiA_rid                           (8'h0),
    .axiA_rresp                         (2'b00),
    .axiA_rlast                         (1'b0),
    .axiAInterrupt                      (1'b0),
    .axiA_awaddr                        (),
    .axiA_awid                          (),
    .axiA_awlen                         (),
    .axiA_awsize                        (),
    .axiA_awburst                       (),
    .axiA_awlock                        (),
    .axiA_awcache                       (),
    .axiA_awprot                        (),
    .axiA_awqos                         (),
    .axiA_awregion                      (),
    .axiA_awvalid                       (),
    .axiA_wdata                         (),
    .axiA_wstrb                         (),
    .axiA_wlast                         (),
    .axiA_wvalid                        (),
    .axiA_bready                        (),
    .axiA_araddr                        (),
    .axiA_arid                          (),
    .axiA_arlen                         (),
    .axiA_arsize                        (),
    .axiA_arburst                       (),
    .axiA_arlock                        (),
    .axiA_arcache                       (),
    .axiA_arprot                        (),
    .axiA_arqos                         (),
    .axiA_arregion                      (),
    .axiA_arvalid                       (),
    .axiA_rready                        (),

    // 时钟故意不接：这条外部主机进 DDR 的通路没有使用，一旦给了时钟，SoC 里的
    // 位宽转换、写仲裁和两个队列就无法被优化掉，实测多占约 600 个 XLR 与 5 块片上 RAM。
    .io_ddrMasters_0_clk                (),
    .io_ddrMasters_0_reset              (),
    .io_ddrMasters_0_aw_valid           (1'b0),
    .io_ddrMasters_0_aw_payload_addr    (32'h0),
    .io_ddrMasters_0_aw_payload_id      (4'h0),
    .io_ddrMasters_0_aw_payload_len     (8'h0),
    .io_ddrMasters_0_aw_payload_size    (3'b000),
    .io_ddrMasters_0_aw_payload_burst   (2'b01),
    .io_ddrMasters_0_aw_payload_lock    (1'b0),
    .io_ddrMasters_0_aw_payload_cache   (4'h0),
    .io_ddrMasters_0_aw_payload_prot    (3'b000),
    .io_ddrMasters_0_aw_payload_qos     (4'h0),
    .io_ddrMasters_0_aw_payload_region  (4'h0),
    .io_ddrMasters_0_aw_ready           (),
    .io_ddrMasters_0_w_valid            (1'b0),
    .io_ddrMasters_0_w_payload_data     (32'h0),
    .io_ddrMasters_0_w_payload_strb     (4'h0),
    .io_ddrMasters_0_w_payload_last     (1'b0),
    .io_ddrMasters_0_w_ready            (),
    .io_ddrMasters_0_b_ready            (1'b1),
    .io_ddrMasters_0_b_valid            (),
    .io_ddrMasters_0_b_payload_id       (),
    .io_ddrMasters_0_b_payload_resp     (),
    .io_ddrMasters_0_ar_valid           (1'b0),
    .io_ddrMasters_0_ar_payload_addr    (32'h0),
    .io_ddrMasters_0_ar_payload_id      (4'h0),
    .io_ddrMasters_0_ar_payload_len     (8'h0),
    .io_ddrMasters_0_ar_payload_size    (3'b000),
    .io_ddrMasters_0_ar_payload_burst   (2'b01),
    .io_ddrMasters_0_ar_payload_lock    (1'b0),
    .io_ddrMasters_0_ar_payload_cache   (4'h0),
    .io_ddrMasters_0_ar_payload_prot    (3'b000),
    .io_ddrMasters_0_ar_payload_qos     (4'h0),
    .io_ddrMasters_0_ar_payload_region  (4'h0),
    .io_ddrMasters_0_ar_ready           (),
    .io_ddrMasters_0_r_ready            (1'b1),
    .io_ddrMasters_0_r_valid            (),
    .io_ddrMasters_0_r_payload_id       (),
    .io_ddrMasters_0_r_payload_data     (),
    .io_ddrMasters_0_r_payload_resp     (),
    .io_ddrMasters_0_r_payload_last     ()
);

// ---------------------------------------------------------------------------
// APB 探针寄存器
// ---------------------------------------------------------------------------
apb_probe u_apb_probe (
    .clk        (core_clk),
    .rst        (soc_system_reset),
    .paddr      (apb_paddr),
    .psel       (apb_psel),
    .penable    (apb_penable),
    .pwrite     (apb_pwrite),
    .pwdata     (apb_pwdata),
    .prdata     (apb_prdata),
    .pready     (apb_pready),
    .pslverr    (apb_pslverr)
);

// ---------------------------------------------------------------------------
// HDMI 发送
// ---------------------------------------------------------------------------
hdmi_tx u_hdmi_tx (
    .clk_pixel  (hdmi_tx_slow_clk),
    .pll_locked (hdmi_pll_locked),
    .tmds_clk   (tmds_tx_clk_TX_DATA),
    .tmds_data0 (tmds_tx_data0_TX_DATA),
    .tmds_data1 (tmds_tx_data1_TX_DATA),
    .tmds_data2 (tmds_tx_data2_TX_DATA)
);

// LVDS TX 常开、不复位
assign tmds_tx_clk_TX_OE    = 1'b1;
assign tmds_tx_data0_TX_OE  = 1'b1;
assign tmds_tx_data1_TX_OE  = 1'b1;
assign tmds_tx_data2_TX_OE  = 1'b1;
assign tmds_tx_clk_TX_RST   = 1'b0;
assign tmds_tx_data0_TX_RST = 1'b0;
assign tmds_tx_data1_TX_RST = 1'b0;
assign tmds_tx_data2_TX_RST = 1'b0;

endmodule

`default_nettype wire
