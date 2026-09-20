// gfx2d 顶层
//
// 组成：
//   u_soc        Sapphire RISC-V SoC（efx_soc 3.4.0 生成，fpga/gfx2d_ti60/ip/soc/soc.v）
//   u_ddr3       DDR3 软核控制器（厂商加密源码，rtl/vendor/ddr3_controller/）
//   u_apb_probe  APB 探针寄存器，占位并验证 APB 通路（rtl/bus/apb_probe.v）
//   u_axi_probe  AXI 探针主机，DDR3 上的第二个主机，实验 E4 用（rtl/bus/axi_probe_master.v）
//   u_scanout   扫描输出替身，实验 E5 的 2.3 节用（rtl/bus/scanout_stub.v）
//   u_axi_mux    AXI 多路器，扫描替身、探针主机与 SoC 共用 DDR3 控制器（rtl/vendor/axi_mux/Axi_Mux.v）
//   u_mux_guard 仲裁配置与 SoC 防饿死保护，永久模块（rtl/bus/axi_mux_guard.v）
//   u_hdmi_tx    HDMI 发送，640x480@60 八色竖条（rtl/hdmi/hdmi_tx.v）
//
// 时钟（全部由 Interface Designer 里的 PLL 产生，见 fpga/gfx2d_ti60/README.md）：
//   core_clk           100MHz   PLL_TL0 <- clk_25m    SoC、DDR3 控制器 AXI 侧、APB
//   sdram_clk 等四个   400MHz   PLL_TR0 <- clk_25m    DDR3 PHY
//   hdmi_tx_slow_clk   25.2MHz  PLL_BL0 <- clk_27m    HDMI 像素时钟
//
// 连接（实验 E5 的 2.1 节：Axi_Mux 用 X3，端口重分配为 S0 扫描、S1 引擎、S2 SoC）：
//   scanout_stub（引擎接入前是扫描替身）-> Axi_Mux S0 --+
//   axi_probe_master（引擎接入前是引擎替身）-> S1 -------+--> DDR3 控制器的 AXI 从口
//   SoC io_ddrA（AXI4，128 位）          -> S2 ----------+
//   仲裁方式与三通道掩码由 u_mux_guard 给出，掩码 = 用户掩码 & SoC 防饿死保护掩码
//   SoC APB Slave 0（CPU 地址 0xF810_0000，64KB 窗口）
//     0x1000~0x1FFF -> axi_probe_master 寄存器
//     0xE000~0xEFFF -> scanout_stub 寄存器（扫描替身的量测）
//     0xF000~0xFFFF -> axi_mux_guard 寄存器（仲裁配置与保护）
//     其余          -> apb_probe
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

    // KEY0~3：板上四个按键，低有效（按下为 0），接 GPIO1 的 [3:0]
    input  wire [3:0]                  key_n,

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
// 两侧位宽不同，下面两处是有意的截断，与官方参考设计一致，2026-09-16 实验 E2 已查证：
//   地址  SoC 32 位 -> 控制器 28 位。SoC 送出的地址与 CPU 地址按位相同（不减基址）。
//         上板实测低 28 位逐位参与寻址，高 4 位丢弃，回绕周期 256MB：
//         0x0000_1000~0x0FFF_FFFF 内 CPU 地址即控制器地址，以上全是别名。
//   ID    SoC 8 位 -> 控制器 4 位，读写响应回来时高 4 位补 0。
//         SoC 内部 CPU 一路的 ID 接常量 0，发出的 awid/arid 恒为 8'h00，截断安全。
//         io_ddrMasters_0 的 ID 最高位是 1，启用它会让响应回错主机；这个口已在 SoC IP 里关掉
//         （efx_soc 3.4.0 重新生成，AXIMaster=0）。
//   依据见 Engineering/TechReports/platform/02-DDR地址译码与AXI-ID实测.md。
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

// SoC 侧经 Axi_Mux 回来的握手与数据
wire                           soc_ddr_bvalid;
wire [7:0]                     soc_ddr_bid;
wire                           soc_ddr_rvalid;
wire                           soc_ddr_rlast;
wire [7:0]                     soc_ddr_rid;
wire [`AXI_DATA_WIDTH-1:0]     soc_ddr_rdata;
wire [1:0]                     soc_ddr_rresp;

// ---------------------------------------------------------------------------
// axi_probe_master 的 AXI 主机口（Axi_Mux S1）
// ---------------------------------------------------------------------------
wire [31:0]                    prb_awaddr;
wire [7:0]                     prb_awlen;
wire [2:0]                     prb_awsize;
wire [1:0]                     prb_awburst;
wire                           prb_awvalid;
wire                           prb_awready;
wire [`AXI_DATA_WIDTH-1:0]     prb_wdata;
wire [`AXI_DATA_WIDTH/8-1:0]   prb_wstrb;
wire                           prb_wlast;
wire                           prb_wvalid;
wire                           prb_wready;
wire                           prb_bvalid;
wire                           prb_bready;
wire [31:0]                    prb_araddr;
wire [7:0]                     prb_arlen;
wire [2:0]                     prb_arsize;
wire [1:0]                     prb_arburst;
wire                           prb_arvalid;
wire                           prb_arready;
wire [`AXI_DATA_WIDTH-1:0]     prb_rdata;
wire [1:0]                     prb_rresp;
wire                           prb_rlast;
wire                           prb_rvalid;
wire                           prb_rready;
// ---------------------------------------------------------------------------
// 扫描输出（display）的 AXI 读主机口（Axi_Mux S0）
// ---------------------------------------------------------------------------
// 只发起读，写通道在例化处接成不发起
wire [31:0]                    sco_araddr;
wire [7:0]                     sco_arlen;
wire [2:0]                     sco_arsize;
wire [1:0]                     sco_arburst;
wire [1:0]                     sco_arlock;
wire                           sco_arvalid;
wire                           sco_arready;
wire [7:0]                     sco_arid;
wire [7:0]                     sco_rid;
wire [`AXI_DATA_WIDTH-1:0]     sco_rdata;
wire [1:0]                     sco_rresp;
wire                           sco_rlast;
wire                           sco_rvalid;
wire                           sco_rready;

// 扫描输出与 HDMI 之间（D10 取 B′）：像素与同步由 display 出，复位由 hdmi_tx 出
wire [23:0]                    disp_vid_rgb;
wire                           disp_vid_hs;
wire                           disp_vid_vs;
wire                           disp_vid_de;
wire                           hdmi_vid_rst_n;
wire                           disp_irq;

// Axi_Mux 的仲裁配置来自永久模块 u_mux_guard（E5 2.1 节第 3 步）
wire                           guard_arb_type;
wire [2:0]                     guard_cnl_mask;

// ---------------------------------------------------------------------------
// Axi_Mux 出口，接 DDR3 控制器
// ---------------------------------------------------------------------------
wire [7:0]                     mux_awid;
wire [31:0]                    mux_awaddr;
wire [7:0]                     mux_awlen;
wire [2:0]                     mux_awsize;
wire [1:0]                     mux_awburst;
wire [1:0]                     mux_awlock;
wire                           mux_awvalid;
wire                           ddr_awready;
wire [`AXI_DATA_WIDTH-1:0]     mux_wdata;
wire [`AXI_DATA_WIDTH/8-1:0]   mux_wstrb;
wire                           mux_wlast;
wire                           mux_wvalid;
wire                           ddr_wready;
wire                           mux_bready;
wire [7:0]                     mux_arid;
wire [31:0]                    mux_araddr;
wire [7:0]                     mux_arlen;
wire [2:0]                     mux_arsize;
wire [1:0]                     mux_arburst;
wire [1:0]                     mux_arlock;
wire                           mux_arvalid;
wire                           ddr_arready;
wire                           mux_rready;

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

    // 从 Axi_Mux 出口进来。Axi_Mux 不传 cache/prot/qos，接常量；lock 取它 2 位里的低位
    .s_axi_awid             (mux_awid[`AXI_ID_WIDTH-1:0]),
    .s_axi_awaddr           (mux_awaddr[`AXI_ADDR_WIDTH-1:0]),
    .s_axi_awlen            (mux_awlen),
    .s_axi_awsize           (mux_awsize),
    .s_axi_awburst          (mux_awburst),
    .s_axi_awlock           (mux_awlock[0]),
    .s_axi_awcache          (4'h0),
    .s_axi_awprot           (3'b000),
    .s_axi_awqos            (4'h0),
    .s_axi_awvalid          (mux_awvalid),
    .s_axi_awready          (ddr_awready),

    .s_axi_wdata            (mux_wdata),
    .s_axi_wstrb            (mux_wstrb),
    .s_axi_wlast            (mux_wlast),
    .s_axi_wvalid           (mux_wvalid),
    .s_axi_wready           (ddr_wready),

    .s_axi_bid              (ddr_bid),
    .s_axi_bresp            (ddr_bresp),
    .s_axi_bvalid           (ddr_bvalid),
    .s_axi_bready           (mux_bready),

    .s_axi_arid             (mux_arid[`AXI_ID_WIDTH-1:0]),
    .s_axi_araddr           (mux_araddr[`AXI_ADDR_WIDTH-1:0]),
    .s_axi_arlen            (mux_arlen),
    .s_axi_arsize           (mux_arsize),
    .s_axi_arburst          (mux_arburst),
    .s_axi_arlock           (mux_arlock[0]),
    .s_axi_arcache          (4'h0),
    .s_axi_arprot           (3'b000),
    .s_axi_arqos            (4'h0),
    .s_axi_arvalid          (mux_arvalid),
    .s_axi_arready          (ddr_arready),

    .s_axi_rid              (ddr_rid),
    .s_axi_rdata            (ddr_rdata),
    .s_axi_rresp            (ddr_rresp),
    .s_axi_rlast            (ddr_rlast),
    .s_axi_rvalid           (ddr_rvalid),
    .s_axi_rready           (mux_rready),

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

    // GPIO1：[3:0] 按键 KEY0~3（低有效，软件取反），[4] 预留给摄像头模组复位，[7:5] 备用。
    // 摄像头工作点接 [4] 的管脚之前，写口与使能口悬空。
    .system_gpio_1_io_read              ({4'b0000, key_n}),
    .system_gpio_1_io_write             (),
    .system_gpio_1_io_writeEnable       (),

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

    .io_ddrA_b_payload_id               (soc_ddr_bid),
    .io_ddrA_b_payload_resp             (ddr_bresp),
    .io_ddrA_b_valid                    (soc_ddr_bvalid),
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

    .io_ddrA_r_payload_id               (soc_ddr_rid),
    .io_ddrA_r_payload_data             (soc_ddr_rdata),
    .io_ddrA_r_payload_resp             (soc_ddr_rresp),
    .io_ddrA_r_payload_last             (soc_ddr_rlast),
    .io_ddrA_r_valid                    (soc_ddr_rvalid),
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

    // 用户中断：A 扫描输出场同步，B 引擎，C 摄像头帧完成（中断号 16/17/22，见 sdk 的 soc.h）。
    // A 已接到扫描输出的场起始中断（电平，复位后 IRQ_EN=0，见扫描输出 spec 的 D5）；
    // B、C 的模块还没有接入，先接 0。分配见 Engineering/TechReports/platform/07-SoC-IP定型与工具链升级.md 3.1 节。
    .userInterruptA                     (disp_irq),
    .userInterruptB                     (1'b0),
    .userInterruptC                     (1'b0),

    // I2C0：保留给摄像头配置传感器（0xF801_7000）。管脚等摄像头工作点再接，
    // 在那之前读口接空闲电平 1，写口悬空。
    .system_i2c_0_io_scl_read           (1'b1),
    .system_i2c_0_io_sda_read           (1'b1),
    .system_i2c_0_io_scl_write          (),
    .system_i2c_0_io_sda_write          ()
);

// ---------------------------------------------------------------------------
// APB Slave 0 的地址划分（按 paddr[15:12] 分成四个窗口）
// ---------------------------------------------------------------------------
//   0x1000~0x1FFF  axi_probe_master 的寄存器（CPU 地址 0xF810_1000 起）
//   0xE000~0xEFFF  scanout_stub 的寄存器（实验 E5 的扫描替身量测）
//   0xF000~0xFFFF  axi_mux_guard 的寄存器（仲裁配置与 SoC 防饿死保护）
//   其余           apb_probe，它按完整 16 位译码，未映射地址仍然置 PSLVERROR
wire        apb_sel_prb = (apb_paddr[15:12] == 4'h1);
wire        apb_sel_sco = (apb_paddr[15:12] == 4'hE);
wire        apb_sel_grd = (apb_paddr[15:12] == 4'hF);

wire [31:0] probe_prdata;
wire        probe_pready;
wire        probe_pslverr;
wire [31:0] prb_prdata;
wire        prb_pready;
wire        prb_pslverr;
wire [31:0] sco_prdata;
wire        sco_pready;
wire        sco_pslverr;
wire [31:0] grd_prdata;
wire        grd_pready;
wire        grd_pslverr;

apb_probe u_apb_probe (
    .clk        (core_clk),
    .rst        (soc_system_reset),
    .paddr      (apb_paddr),
    .psel       (apb_psel & ~apb_sel_prb & ~apb_sel_sco & ~apb_sel_grd),
    .penable    (apb_penable),
    .pwrite     (apb_pwrite),
    .pwdata     (apb_pwdata),
    .prdata     (probe_prdata),
    .pready     (probe_pready),
    .pslverr    (probe_pslverr)
);

assign apb_prdata  = apb_sel_prb ? prb_prdata  :
                     apb_sel_sco ? sco_prdata  :
                     apb_sel_grd ? grd_prdata  : probe_prdata;
assign apb_pready  = apb_sel_prb ? prb_pready  :
                     apb_sel_sco ? sco_pready  :
                     apb_sel_grd ? grd_pready  : probe_pready;
assign apb_pslverr = apb_sel_prb ? prb_pslverr :
                     apb_sel_sco ? sco_pslverr :
                     apb_sel_grd ? grd_pslverr : probe_pslverr;

// ---------------------------------------------------------------------------
// AXI 探针主机（实验 E4），Axi_Mux 的 S1
// ---------------------------------------------------------------------------
axi_probe_master u_axi_probe (
    .clk        (core_clk),
    .rst        (soc_system_reset),
    .paddr      (apb_paddr[11:0]),
    .psel       (apb_psel & apb_sel_prb),
    .penable    (apb_penable),
    .pwrite     (apb_pwrite),
    .pwdata     (apb_pwdata),
    .prdata     (prb_prdata),
    .pready     (prb_pready),
    .pslverr    (prb_pslverr),

    // 仲裁配置已移到 u_mux_guard（E5 2.1 节第 3 步）：探针的 ARB 寄存器不再驱动多路器，
    // 寄存器本身保留，供自检与旧程序读回
    .arb_type   (),
    .arb_mask   (),

    .awaddr     (prb_awaddr),
    .awlen      (prb_awlen),
    .awsize     (prb_awsize),
    .awburst    (prb_awburst),
    .awvalid    (prb_awvalid),
    .awready    (prb_awready),
    .wdata      (prb_wdata),
    .wstrb      (prb_wstrb),
    .wlast      (prb_wlast),
    .wvalid     (prb_wvalid),
    .wready     (prb_wready),
    .bresp      (ddr_bresp),
    .bvalid     (prb_bvalid),
    .bready     (prb_bready),
    .araddr     (prb_araddr),
    .arlen      (prb_arlen),
    .arsize     (prb_arsize),
    .arburst    (prb_arburst),
    .arvalid    (prb_arvalid),
    .arready    (prb_arready),
    .rdata      (prb_rdata),
    .rresp      (prb_rresp),
    .rlast      (prb_rlast),
    .rvalid     (prb_rvalid),
    .rready     (prb_rready)
);

// ---------------------------------------------------------------------------
// Axi_Mux 的仲裁配置与 SoC 防饿死保护（E5 2.1 节第 3、4 步，永久模块）
// ---------------------------------------------------------------------------
// APB 窗口 0xF810_F000~0xF810_FFFF；送进多路器的掩码 = 用户掩码 & 保护掩码。
axi_mux_guard u_mux_guard (
    .clk            (core_clk),
    .rst            (soc_system_reset),
    .paddr          (apb_paddr[11:0]),
    .psel           (apb_psel & apb_sel_grd),
    .penable        (apb_penable),
    .pwrite         (apb_pwrite),
    .pwdata         (apb_pwdata),
    .prdata         (grd_prdata),
    .pready         (grd_pready),
    .pslverr        (grd_pslverr),

    .arb_type       (guard_arb_type),
    .cnl_mask       (guard_cnl_mask),

    .s2_arvalid     (soc_ddr_arvalid),
    .s2_arready     (soc_ddr_arready),
    .s2_awvalid     (soc_ddr_awvalid),
    .s2_awready     (soc_ddr_awready)
);

// ---------------------------------------------------------------------------
// 扫描输出替身（E5 2.3 节：按行节拍发 160 拍读并把时间记下来）
// ---------------------------------------------------------------------------
// 扫描输出（display）
// ---------------------------------------------------------------------------
// APB 窗口 0xF810_E000~0xF810_EFFF（接管替身那一页）；只发起读，写通道在 Axi_Mux 例化处接成不发起。
// 像素侧与 hdmi_tx 对接（D10 取 B′）：display 出像素与 hs/vs/de，hdmi_tx 出 vid_rst_n 与 TMDS。
// 顶层管脚与 peri.xml 都不变。
display_top #(
    .H_ACTIVE   (640),
    .H_FP       (16),
    .H_SYNC     (96),
    .H_BP       (48),
    .V_ACTIVE   (480),
    .V_FP       (10),
    .V_SYNC     (2),
    .V_BP       (33),
    .SYNC_POL   (1'b0),
    .FRONT_BASE_DEF (32'h0200_0000),
    .BACK_BASE_DEF  (32'h0240_0000)
) u_display (
    .clk_core       (core_clk),
    .rst_core       (soc_system_reset),
    .clk_pixel      (hdmi_tx_slow_clk),
    .rst_pixel_n    (hdmi_vid_rst_n),

    .paddr          (apb_paddr[11:0]),
    .psel           (apb_psel & apb_sel_sco),
    .penable        (apb_penable),
    .pwrite         (apb_pwrite),
    .pwdata         (apb_pwdata),
    .prdata         (sco_prdata),
    .pready         (sco_pready),
    .pslverr        (sco_pslverr),

    .ar_addr        (sco_araddr),
    .ar_len         (sco_arlen),
    .ar_size        (sco_arsize),
    .ar_burst       (sco_arburst),
    .ar_lock        (sco_arlock),
    .ar_id          (sco_arid),
    .ar_valid       (sco_arvalid),
    .ar_ready       (sco_arready),

    .r_id           (sco_rid),
    .r_data         (sco_rdata),
    .r_last         (sco_rlast),
    .r_resp         (sco_rresp),
    .r_valid        (sco_rvalid),
    .r_ready        (sco_rready),

    .irq            (disp_irq),
    .vid_rgb        (disp_vid_rgb),
    .vid_hs         (disp_vid_hs),
    .vid_vs         (disp_vid_vs),
    .vid_de         (disp_vid_de)
);

// ---------------------------------------------------------------------------
// Axi_Mux：S0 = 扫描输出，S1 = axi_probe_master，S2 = SoC，出口 = DDR3 控制器
// ---------------------------------------------------------------------------
// Normal 模式下响应按事务顺序送回，与 ID 无关，所以控制器 4 位 ID 截断仍然安全。
// Axi_Mux 没有 BRESP 端口；同一时刻只有被选中的主机看到 bvalid，控制器的 bresp 直接并接给各主机。
// 仲裁方式与三通道掩码由 u_mux_guard 给出（不再是探针主机的 ARB 寄存器）：
// 上电为优先级模式、三通道全开、SoC 防饿死保护开启，见 E5 2.1、2.2 节。
Axi_Mux #(
    .SLAVE_ID_WIDTH (8),
    .ID_MODE_SEL    ("Normal"),
    .AXI_DATA_WIDTH (`AXI_DATA_WIDTH)
) u_axi_mux (
    .Sys_Clk        (core_clk),
    .Sys_Rst_N      (ddr_rstn),

    .O_M_AW_ID      (mux_awid),
    .O_M_AW_ADDR    (mux_awaddr),
    .O_M_AW_LEN     (mux_awlen),
    .O_M_AW_SIZE    (mux_awsize),
    .O_M_AW_BURST   (mux_awburst),
    .O_M_AW_LOCK    (mux_awlock),
    .O_M_AW_VALID   (mux_awvalid),
    .I_M_AW_READY   (ddr_awready),
    .O_M_W_ID       (),
    .O_M_W_DATA     (mux_wdata),
    .O_M_W_LAST     (mux_wlast),
    .O_M_W_STRB     (mux_wstrb),
    .O_M_W_VALID    (mux_wvalid),
    .I_M_W_READY    (ddr_wready),
    .I_M_B_ID       ({4'b0, ddr_bid}),
    .I_M_B_VALID    (ddr_bvalid),
    .O_M_B_READY    (mux_bready),
    .O_M_AR_ID      (mux_arid),
    .O_M_AR_ADDR    (mux_araddr),
    .O_M_AR_LEN     (mux_arlen),
    .O_M_AR_SIZE    (mux_arsize),
    .O_M_AR_BURST   (mux_arburst),
    .O_M_AR_LOCK    (mux_arlock),
    .O_M_AR_VALID   (mux_arvalid),
    .I_M_AR_READY   (ddr_arready),
    .I_M_R_ID       ({4'b0, ddr_rid}),
    .I_M_R_DATA     (ddr_rdata),
    .I_M_R_LAST     (ddr_rlast),
    .I_M_R_RESP     (ddr_rresp),
    .I_M_R_VALID    (ddr_rvalid),
    .O_M_R_READY    (mux_rready),

    // S0：扫描输出，只发起读，写通道接成不发起（E5 2.1 节第 2 步）
    .I_S0_AW_ID     (8'h0),
    .I_S0_AW_ADDR   (32'h0),
    .I_S0_AW_LEN    (8'h0),
    .I_S0_AW_SIZE   (3'b000),
    .I_S0_AW_BURST  (2'b00),
    .I_S0_AW_LOCK   (2'b00),
    .I_S0_AW_VALID  (1'b0),
    .O_S0_AW_READY  (),
    .I_S0_W_ID      (8'h0),
    .I_S0_W_DATA    ({`AXI_DATA_WIDTH{1'b0}}),
    .I_S0_W_LAST    (1'b0),
    .I_S0_W_STRB    ({`AXI_DATA_WIDTH/8{1'b0}}),
    .I_S0_W_VALID   (1'b0),
    .O_S0_W_READY   (),
    .O_S0_B_ID      (),
    .O_S0_B_VALID   (),
    .I_S0_B_READY   (1'b1),
    .I_S0_AR_ID     (sco_arid),
    .I_S0_AR_ADDR   (sco_araddr),
    .I_S0_AR_LEN    (sco_arlen),
    .I_S0_AR_SIZE   (sco_arsize),
    .I_S0_AR_BURST  (sco_arburst),
    .I_S0_AR_LOCK   (sco_arlock),
    .I_S0_AR_VALID  (sco_arvalid),
    .O_S0_AR_READY  (sco_arready),
    .O_S0_R_ID      (sco_rid),
    .O_S0_R_DATA    (sco_rdata),
    .O_S0_R_LAST    (sco_rlast),
    .O_S0_R_RESP    (sco_rresp),
    .O_S0_R_VALID   (sco_rvalid),
    .I_S0_R_READY   (sco_rready),

    // S2：SoC io_ddrA（原来在 S0，E5 2.1 节第 2 步挪到这里，由 guard 保护）
    .I_S2_AW_ID     (soc_ddr_awid),
    .I_S2_AW_ADDR   (soc_ddr_awaddr),
    .I_S2_AW_LEN    (soc_ddr_awlen),
    .I_S2_AW_SIZE   (soc_ddr_awsize),
    .I_S2_AW_BURST  (soc_ddr_awburst),
    .I_S2_AW_LOCK   ({1'b0, soc_ddr_awlock}),
    .I_S2_AW_VALID  (soc_ddr_awvalid),
    .O_S2_AW_READY  (soc_ddr_awready),
    .I_S2_W_ID      (8'h0),
    .I_S2_W_DATA    (soc_ddr_wdata),
    .I_S2_W_LAST    (soc_ddr_wlast),
    .I_S2_W_STRB    (soc_ddr_wstrb),
    .I_S2_W_VALID   (soc_ddr_wvalid),
    .O_S2_W_READY   (soc_ddr_wready),
    .O_S2_B_ID      (soc_ddr_bid),
    .O_S2_B_VALID   (soc_ddr_bvalid),
    .I_S2_B_READY   (soc_ddr_bready),
    .I_S2_AR_ID     (soc_ddr_arid),
    .I_S2_AR_ADDR   (soc_ddr_araddr),
    .I_S2_AR_LEN    (soc_ddr_arlen),
    .I_S2_AR_SIZE   (soc_ddr_arsize),
    .I_S2_AR_BURST  (soc_ddr_arburst),
    .I_S2_AR_LOCK   ({1'b0, soc_ddr_arlock}),
    .I_S2_AR_VALID  (soc_ddr_arvalid),
    .O_S2_AR_READY  (soc_ddr_arready),
    .O_S2_R_ID      (soc_ddr_rid),
    .O_S2_R_DATA    (soc_ddr_rdata),
    .O_S2_R_LAST    (soc_ddr_rlast),
    .O_S2_R_RESP    (soc_ddr_rresp),
    .O_S2_R_VALID   (soc_ddr_rvalid),
    .I_S2_R_READY   (soc_ddr_rready),

    .I_S1_AW_ID     (8'h0),
    .I_S1_AW_ADDR   (prb_awaddr),
    .I_S1_AW_LEN    (prb_awlen),
    .I_S1_AW_SIZE   (prb_awsize),
    .I_S1_AW_BURST  (prb_awburst),
    .I_S1_AW_LOCK   (2'b00),
    .I_S1_AW_VALID  (prb_awvalid),
    .O_S1_AW_READY  (prb_awready),
    .I_S1_W_ID      (8'h0),
    .I_S1_W_DATA    (prb_wdata),
    .I_S1_W_LAST    (prb_wlast),
    .I_S1_W_STRB    (prb_wstrb),
    .I_S1_W_VALID   (prb_wvalid),
    .O_S1_W_READY   (prb_wready),
    .O_S1_B_ID      (),
    .O_S1_B_VALID   (prb_bvalid),
    .I_S1_B_READY   (prb_bready),
    .I_S1_AR_ID     (8'h0),
    .I_S1_AR_ADDR   (prb_araddr),
    .I_S1_AR_LEN    (prb_arlen),
    .I_S1_AR_SIZE   (prb_arsize),
    .I_S1_AR_BURST  (prb_arburst),
    .I_S1_AR_LOCK   (2'b00),
    .I_S1_AR_VALID  (prb_arvalid),
    .O_S1_AR_READY  (prb_arready),
    .O_S1_R_ID      (),
    .O_S1_R_DATA    (prb_rdata),
    .O_S1_R_LAST    (prb_rlast),
    .O_S1_R_RESP    (prb_rresp),
    .O_S1_R_VALID   (prb_rvalid),
    .I_S1_R_READY   (prb_rready),

    .Cfg_Arb_Type   (guard_arb_type),
    .Cfg_Cnl_Mask   (guard_cnl_mask),
    .O_AW_Cnl_Num   (),
    .O_W_Cnl_Num    (),
    .O_B_Cnl_Num    (),
    .O_AR_Cnl_Num   (),
    .O_R_Cnl_Num    ()
);

// ---------------------------------------------------------------------------
// HDMI 发送（D10 取 B′：本模块是纯编码封装，彩条在 display_out 里）
// ---------------------------------------------------------------------------
// 像素与 hs/vs/de 由 u_display 给；本模块把复位同步的结果 vid_rst_n 回给像素侧共用。
hdmi_tx u_hdmi_tx (
    .clk_pixel  (hdmi_tx_slow_clk),
    .pll_locked (hdmi_pll_locked),
    .vid_rgb    (disp_vid_rgb),
    .vid_hs     (disp_vid_hs),
    .vid_vs     (disp_vid_vs),
    .vid_de     (disp_vid_de),
    .vid_rst_n  (hdmi_vid_rst_n),
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
