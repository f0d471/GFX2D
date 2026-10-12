// sim/axi_mux_guard 测试台（Axi_Mux 防饿死保护的仿真）
//
// 被测：rtl/vendor/axi_mux/Axi_Mux.v（X3，真厂商源码）、rtl/bus/axi_mux_guard.v，
//       从机复用 sim/axi_probe/axi_ram_model.v。
// 用例由 run.sh 用编译期宏选，见文件末尾的 initial 块：
//   CASE_GRANT_SEQUENCE  直接例化 Axi_Mux_Addr_Ctrl，复现固定授权序列
//   CASE_DUAL_DIRECTION  guard 单元用例：AR、AW 同时超限且分时完成
//   CASE_CHAIN           全链路：三个主机 + guard + Axi_Mux + 从机
//                       可叠加 GUARD_OFF / INFLIGHT1 / S1_WRITE / INJECT_MASK_S0 / HOLD_MIDGRANT / INJECT_WLAST
//
// 判据（阈值都是按板上实测定的验收口径，不是形式上界）：
//   grant_sequence   三路持续请求复现 S0=10 S1=10 S2=0；仅 S1/S2 复现 S1=19 S2=0
//   dual_direction   两个 protect 都清除之前 S1 不恢复；同拍触发 HITS 加 2；
//                    未握手时 S2_MAX_WAIT 已经在涨（VERSION 2 的运行中最大值）
//   全链路用例一律要求：S1 不撤销 valid、扫描不超时、从机的 wlast_err 为 0、
//   S2 完成 CPU_REQS 笔、S2 最大地址等待（含未握手的那一笔）不超过阈值。
//   阈值：遵守在途契约（每方向一笔在途）时 CONTRACT_BOUND_VAL（414），激进 S1 时 NORMAL_BOUND_VAL（700）。
//   读方向 normal / inflight1 PASS，inject_noguard 同一阈值下 FAIL（保护在读方向起作用的对照）；
//   写方向 wr_* 全部 PASS，保护开关的结果逐项相同，由 run.sh 比对；
//   inject_mask_s0   掩码把 S0 位一起清掉，扫描必须拿不到授权（判据报 FAIL 才算对）
//   inject_midgrant  S1 已获授权后下游持续不 ready，S1 不得撤销 valid，S2 释放后能完成
//   inject_wlast     复原引擎替身打一拍的 WLAST，从机的协议检查必须报 FAIL
`timescale 1ns/1ps
`default_nettype none

`ifndef ADDR_STALL_MAX_VAL
  `define ADDR_STALL_MAX_VAL 4
`endif
`ifndef RD_QUEUE_VAL
  `define RD_QUEUE_VAL 4
`endif
`ifndef WR_QUEUE_VAL
  `define WR_QUEUE_VAL 1
`endif
`ifndef AGE_LIMIT_VAL
  `define AGE_LIMIT_VAL 64
`endif
`ifndef RUN_CYCLES
  `define RUN_CYCLES 20000
`endif
`ifndef CPU_REQS
  `define CPU_REQS 10
`endif
// 两条验收阈值，都不是形式上界，来自板上实测：
//   NORMAL_BOUND_VAL    激进 S1 时：按实测取（保护开启 590、关闭 889），留余量取 700
//   CONTRACT_BOUND_VAL  遵守措施一时：AGE_LIMIT(64) + 一笔 256 拍读(约 310) + 搜索余量(40)
`ifndef NORMAL_BOUND_VAL
  `define NORMAL_BOUND_VAL 700
`endif
`ifndef CONTRACT_BOUND_VAL
  `define CONTRACT_BOUND_VAL 414
`endif

`ifdef GUARD_OFF
  `define CTRL_VAL 32'd1
`else
  `define CTRL_VAL 32'd3
`endif

module tb_axi_mux_guard;

// ---------------------------------------------------------------------------
// 时钟、复位、用例名
// ---------------------------------------------------------------------------
reg clk = 0;
always #5 clk = ~clk;
reg rstn = 0;

integer errors = 0;

// ---------------------------------------------------------------------------
// 三个主机侧的 AXI 信号（0 = S0 扫描替身，1 = S1 引擎替身，2 = S2 SoC）
// ---------------------------------------------------------------------------
wire [31:0]  m_awaddr  [0:2];
wire [7:0]   m_awlen   [0:2];
wire [2:0]   m_awsize  [0:2];
wire [1:0]   m_awburst [0:2];
wire         m_awvalid [0:2];
wire         m_awready [0:2];
wire [127:0] m_wdata   [0:2];
wire [15:0]  m_wstrb   [0:2];
wire         m_wlast   [0:2];
wire         m_wvalid  [0:2];
wire         m_wready  [0:2];
wire         m_bvalid  [0:2];
wire         m_bready  [0:2];
wire [31:0]  m_araddr  [0:2];
wire [7:0]   m_arlen   [0:2];
wire [2:0]   m_arsize  [0:2];
wire [1:0]   m_arburst [0:2];
wire         m_arvalid [0:2];
wire         m_arready [0:2];
wire [127:0] m_rdata   [0:2];
wire [1:0]   m_rresp   [0:2];
wire         m_rlast   [0:2];
wire         m_rvalid  [0:2];
wire         m_rready  [0:2];

// ---------------------------------------------------------------------------
// 从机侧
// ---------------------------------------------------------------------------
wire [31:0]  s_awaddr, s_araddr;
wire [7:0]   s_awlen, s_arlen;
wire [2:0]   s_awsize, s_arsize;
wire [1:0]   s_awburst, s_arburst;
wire         s_awvalid, s_awready, s_arvalid, s_arready;
wire [127:0] s_wdata, s_rdata;
wire [15:0]  s_wstrb;
wire         s_wlast, s_wvalid, s_wready;
wire         s_bvalid, s_bready;
wire [1:0]   s_bresp, s_rresp;
wire         s_rlast, s_rvalid, s_rready;

reg          hold_aw = 0, hold_ar = 0;

// 地址通道反压监视：ARREADY 连续为低的拍数上限
integer      ar_low_cnt = 0;
integer      ar_low_max = 0;
always @(posedge clk) begin
    if (!s_arready) begin
        ar_low_cnt = ar_low_cnt + 1;
        if (ar_low_cnt > ar_low_max) ar_low_max = ar_low_cnt;
    end else
        ar_low_cnt = 0;
end

// ---------------------------------------------------------------------------
// guard：仲裁配置、掩码与保护
// ---------------------------------------------------------------------------
reg  [11:0] g_paddr = 0;
reg         g_psel = 0, g_penable = 0, g_pwrite = 0;
reg  [31:0] g_pwdata = 0;
wire [31:0] g_prdata;
wire        g_pready, g_pslverr;

wire        guard_arb_type;
wire [2:0]  guard_cnl_mask;

`ifdef INJECT_MASK_S0
  // 注错：让保护掩码的 S0 位跟着 S1 一起被清（inject_mask_s0）
  wire [2:0] mux_mask = guard_cnl_mask & 3'b110;
`else
  wire [2:0] mux_mask = guard_cnl_mask;
`endif

// dual_direction 用例直接把 guard 的观测口接到测试台寄存器
`ifdef CASE_DUAL_DIRECTION
  reg  dd_arvalid = 0, dd_arready = 0, dd_awvalid = 0, dd_awready = 0;
  wire dd_s2_arvalid = dd_arvalid, dd_s2_arready = dd_arready;
  wire dd_s2_awvalid = dd_awvalid, dd_s2_awready = dd_awready;
`else
  wire dd_s2_arvalid = m_arvalid[2], dd_s2_arready = m_arready[2];
  wire dd_s2_awvalid = m_awvalid[2], dd_s2_awready = m_awready[2];
`endif

axi_mux_guard u_guard (
    .clk            (clk),
    .rst            (~rstn),
    .paddr          (g_paddr),
    .psel           (g_psel),
    .penable        (g_penable),
    .pwrite         (g_pwrite),
    .pwdata         (g_pwdata),
    .prdata         (g_prdata),
    .pready         (g_pready),
    .pslverr        (g_pslverr),
    .arb_type       (guard_arb_type),
    .cnl_mask       (guard_cnl_mask),
    .s2_arvalid     (dd_s2_arvalid),
    .s2_arready     (dd_s2_arready),
    .s2_awvalid     (dd_s2_awvalid),
    .s2_awready     (dd_s2_awready)
);

// ---------------------------------------------------------------------------
// Axi_Mux（真厂商源码，X3）
// ---------------------------------------------------------------------------
Axi_Mux #(.SLAVE_ID_WIDTH(8), .ID_MODE_SEL("Normal"), .AXI_DATA_WIDTH(128)) u_mux (
    .Sys_Clk(clk), .Sys_Rst_N(rstn),

    .O_M_AW_ID(), .O_M_AW_ADDR(s_awaddr), .O_M_AW_LEN(s_awlen), .O_M_AW_SIZE(s_awsize),
    .O_M_AW_BURST(s_awburst), .O_M_AW_LOCK(), .O_M_AW_VALID(s_awvalid), .I_M_AW_READY(s_awready),
    .O_M_W_ID(), .O_M_W_DATA(s_wdata), .O_M_W_LAST(s_wlast), .O_M_W_STRB(s_wstrb),
    .O_M_W_VALID(s_wvalid), .I_M_W_READY(s_wready),
    .I_M_B_ID(8'h0), .I_M_B_VALID(s_bvalid), .O_M_B_READY(s_bready),
    .O_M_AR_ID(), .O_M_AR_ADDR(s_araddr), .O_M_AR_LEN(s_arlen), .O_M_AR_SIZE(s_arsize),
    .O_M_AR_BURST(s_arburst), .O_M_AR_LOCK(), .O_M_AR_VALID(s_arvalid), .I_M_AR_READY(s_arready),
    .I_M_R_ID(8'h0), .I_M_R_DATA(s_rdata), .I_M_R_LAST(s_rlast), .I_M_R_RESP(s_rresp),
    .I_M_R_VALID(s_rvalid), .O_M_R_READY(s_rready),

    // S0：扫描替身
    .I_S0_AW_ID(8'h0), .I_S0_AW_ADDR(m_awaddr[0]), .I_S0_AW_LEN(m_awlen[0]), .I_S0_AW_SIZE(m_awsize[0]),
    .I_S0_AW_BURST(m_awburst[0]), .I_S0_AW_LOCK(2'b00), .I_S0_AW_VALID(m_awvalid[0]), .O_S0_AW_READY(m_awready[0]),
    .I_S0_W_ID(8'h0), .I_S0_W_DATA(m_wdata[0]), .I_S0_W_LAST(m_wlast[0]), .I_S0_W_STRB(m_wstrb[0]),
    .I_S0_W_VALID(m_wvalid[0]), .O_S0_W_READY(m_wready[0]),
    .O_S0_B_ID(), .O_S0_B_VALID(m_bvalid[0]), .I_S0_B_READY(m_bready[0]),
    .I_S0_AR_ID(8'h0), .I_S0_AR_ADDR(m_araddr[0]), .I_S0_AR_LEN(m_arlen[0]), .I_S0_AR_SIZE(m_arsize[0]),
    .I_S0_AR_BURST(m_arburst[0]), .I_S0_AR_LOCK(2'b00), .I_S0_AR_VALID(m_arvalid[0]), .O_S0_AR_READY(m_arready[0]),
    .O_S0_R_ID(), .O_S0_R_DATA(m_rdata[0]), .O_S0_R_LAST(m_rlast[0]), .O_S0_R_RESP(m_rresp[0]),
    .O_S0_R_VALID(m_rvalid[0]), .I_S0_R_READY(m_rready[0]),

    // S1：引擎替身
    .I_S1_AW_ID(8'h0), .I_S1_AW_ADDR(m_awaddr[1]), .I_S1_AW_LEN(m_awlen[1]), .I_S1_AW_SIZE(m_awsize[1]),
    .I_S1_AW_BURST(m_awburst[1]), .I_S1_AW_LOCK(2'b00), .I_S1_AW_VALID(m_awvalid[1]), .O_S1_AW_READY(m_awready[1]),
    .I_S1_W_ID(8'h0), .I_S1_W_DATA(m_wdata[1]), .I_S1_W_LAST(m_wlast[1]), .I_S1_W_STRB(m_wstrb[1]),
    .I_S1_W_VALID(m_wvalid[1]), .O_S1_W_READY(m_wready[1]),
    .O_S1_B_ID(), .O_S1_B_VALID(m_bvalid[1]), .I_S1_B_READY(m_bready[1]),
    .I_S1_AR_ID(8'h0), .I_S1_AR_ADDR(m_araddr[1]), .I_S1_AR_LEN(m_arlen[1]), .I_S1_AR_SIZE(m_arsize[1]),
    .I_S1_AR_BURST(m_arburst[1]), .I_S1_AR_LOCK(2'b00), .I_S1_AR_VALID(m_arvalid[1]), .O_S1_AR_READY(m_arready[1]),
    .O_S1_R_ID(), .O_S1_R_DATA(m_rdata[1]), .O_S1_R_LAST(m_rlast[1]), .O_S1_R_RESP(m_rresp[1]),
    .O_S1_R_VALID(m_rvalid[1]), .I_S1_R_READY(m_rready[1]),

    // S2：SoC
    .I_S2_AW_ID(8'h0), .I_S2_AW_ADDR(m_awaddr[2]), .I_S2_AW_LEN(m_awlen[2]), .I_S2_AW_SIZE(m_awsize[2]),
    .I_S2_AW_BURST(m_awburst[2]), .I_S2_AW_LOCK(2'b00), .I_S2_AW_VALID(m_awvalid[2]), .O_S2_AW_READY(m_awready[2]),
    .I_S2_W_ID(8'h0), .I_S2_W_DATA(m_wdata[2]), .I_S2_W_LAST(m_wlast[2]), .I_S2_W_STRB(m_wstrb[2]),
    .I_S2_W_VALID(m_wvalid[2]), .O_S2_W_READY(m_wready[2]),
    .O_S2_B_ID(), .O_S2_B_VALID(m_bvalid[2]), .I_S2_B_READY(m_bready[2]),
    .I_S2_AR_ID(8'h0), .I_S2_AR_ADDR(m_araddr[2]), .I_S2_AR_LEN(m_arlen[2]), .I_S2_AR_SIZE(m_arsize[2]),
    .I_S2_AR_BURST(m_arburst[2]), .I_S2_AR_LOCK(2'b00), .I_S2_AR_VALID(m_arvalid[2]), .O_S2_AR_READY(m_arready[2]),
    .O_S2_R_ID(), .O_S2_R_DATA(m_rdata[2]), .O_S2_R_LAST(m_rlast[2]), .O_S2_R_RESP(m_rresp[2]),
    .O_S2_R_VALID(m_rvalid[2]), .I_S2_R_READY(m_rready[2]),

    .Cfg_Arb_Type(guard_arb_type), .Cfg_Cnl_Mask(mux_mask),
    .O_AW_Cnl_Num(), .O_W_Cnl_Num(), .O_B_Cnl_Num(), .O_AR_Cnl_Num(), .O_R_Cnl_Num()
);

// ---------------------------------------------------------------------------
// 从机（复用 axi_probe 的模型）
// ---------------------------------------------------------------------------
axi_ram_model #(
    .MAX_LEN(255), .SEED(7), .STALL(1),
    .ADDR_STALL_MAX(`ADDR_STALL_MAX_VAL), .RD_QUEUE(`RD_QUEUE_VAL), .WR_QUEUE(`WR_QUEUE_VAL)
) u_ram (
    .clk(clk), .rst_n(rstn),
    .awaddr(s_awaddr), .awlen(s_awlen), .awvalid(s_awvalid), .awready(s_awready),
    .wdata(s_wdata), .wlast(s_wlast), .wvalid(s_wvalid), .wready(s_wready),
    .bresp(s_bresp), .bvalid(s_bvalid), .bready(s_bready),
    .araddr(s_araddr), .arlen(s_arlen), .arvalid(s_arvalid), .arready(s_arready),
    .rdata(s_rdata), .rresp(s_rresp), .rlast(s_rlast), .rvalid(s_rvalid), .rready(s_rready),
    .hold_aw(hold_aw), .hold_ar(hold_ar)
);

// ---------------------------------------------------------------------------
// 三个主机
// ---------------------------------------------------------------------------
reg s0_en = 0, s1_en = 0, s2_en = 0;
`ifdef S1_WRITE
  reg s1_mode = 1;                      // 1 = 写方向满负载（读、写由两个独立仲裁器处理，必须分开验）
`else
  reg s1_mode = 0;                      // 0 = 读方向满负载
`endif
`ifdef INFLIGHT1
  wire s1_inflight1 = 1'b1;
`else
  wire s1_inflight1 = 1'b0;
`endif

wire [31:0] scan_txn, scan_max_first, scan_max_full;
wire        scan_timeout;
wire [31:0] aggr_txn;
wire [31:0] cpu_done_rd, cpu_done_wr, cpu_max_ar, cpu_max_aw;

master_scan u_scan (
    .clk(clk), .rstn(rstn), .enable(s0_en), .period(32'd400),
    .araddr(m_araddr[0]), .arlen(m_arlen[0]), .arsize(m_arsize[0]), .arburst(m_arburst[0]),
    .arvalid(m_arvalid[0]), .arready(m_arready[0]),
    .rdata(m_rdata[0]), .rlast(m_rlast[0]), .rvalid(m_rvalid[0]), .rready(m_rready[0]),
    .txn_done(scan_txn), .max_first(scan_max_first), .max_full(scan_max_full), .timeout(scan_timeout)
);
assign m_awvalid[0] = 1'b0;
assign m_awaddr[0]  = 32'h0;
assign m_awlen[0]   = 8'h0;
assign m_awsize[0]  = 3'b000;
assign m_awburst[0] = 2'b00;
assign m_wvalid[0]  = 1'b0;
assign m_wdata[0]   = 128'h0;
assign m_wstrb[0]   = 16'h0;
assign m_wlast[0]   = 1'b0;
assign m_bready[0]  = 1'b1;

master_aggr u_aggr (
    .clk(clk), .rstn(rstn), .enable(s1_en), .mode(s1_mode), .inflight1(s1_inflight1),
    .araddr(m_araddr[1]), .arlen(m_arlen[1]), .arsize(m_arsize[1]), .arburst(m_arburst[1]),
    .arvalid(m_arvalid[1]), .arready(m_arready[1]),
    .rdata(m_rdata[1]), .rlast(m_rlast[1]), .rvalid(m_rvalid[1]), .rready(m_rready[1]),
    .awaddr(m_awaddr[1]), .awlen(m_awlen[1]), .awsize(m_awsize[1]), .awburst(m_awburst[1]),
    .awvalid(m_awvalid[1]), .awready(m_awready[1]),
    .wdata(m_wdata[1]), .wstrb(m_wstrb[1]), .wlast(m_wlast[1]), .wvalid(m_wvalid[1]), .wready(m_wready[1]),
    .bvalid(m_bvalid[1]), .bready(m_bready[1]),
    .txn_done(aggr_txn)
);

master_cpu u_cpu (
    .clk(clk), .rstn(rstn), .enable(s2_en),
    .araddr(m_araddr[2]), .arlen(m_arlen[2]), .arsize(m_arsize[2]), .arburst(m_arburst[2]),
    .arvalid(m_arvalid[2]), .arready(m_arready[2]),
    .rdata(m_rdata[2]), .rlast(m_rlast[2]), .rvalid(m_rvalid[2]), .rready(m_rready[2]),
    .awaddr(m_awaddr[2]), .awlen(m_awlen[2]), .awsize(m_awsize[2]), .awburst(m_awburst[2]),
    .awvalid(m_awvalid[2]), .awready(m_awready[2]),
    .wdata(m_wdata[2]), .wstrb(m_wstrb[2]), .wlast(m_wlast[2]), .wvalid(m_wvalid[2]), .wready(m_wready[2]),
    .bvalid(m_bvalid[2]), .bready(m_bready[2]),
    .done_rd(cpu_done_rd), .done_wr(cpu_done_wr), .max_ar_wait(cpu_max_ar), .max_aw_wait(cpu_max_aw)
);

// S1 的协议监视：使能期间 valid 拉高之后、在握手（上一拍 ready 为高）之前不得撤销
`ifndef CASE_DUAL_DIRECTION
reg proto_err = 0;
reg s1_ar_prev = 0, s1_aw_prev = 0;
reg s1_ar_rdy_prev = 0, s1_aw_rdy_prev = 0;
always @(posedge clk) begin
    if (rstn && s1_en) begin
        if (s1_ar_prev && !s1_ar_rdy_prev && !m_arvalid[1]) proto_err <= 1;
        if (s1_aw_prev && !s1_aw_rdy_prev && !m_awvalid[1]) proto_err <= 1;
    end
    s1_ar_prev     <= m_arvalid[1];
    s1_aw_prev     <= m_awvalid[1];
    s1_ar_rdy_prev <= m_arready[1];
    s1_aw_rdy_prev <= m_awready[1];
end
`endif

// ---------------------------------------------------------------------------
// grant_sequence 用：直接例化地址仲裁器（固定授权序列的数字出自这里）
// ---------------------------------------------------------------------------
reg  [2:0]  ac_valid = 3'b000;
reg         ac_ready = 1'b1;
reg  [2:0]  ac_mask  = 3'b111;
wire        ac_mvalid;
wire [2:0]  ac_sready;
wire        ac_end;
wire [1:0]  ac_num;
wire [2:0]  ac_list;
integer     g0 = 0, g1 = 0, g2 = 0;
reg         counting = 0;

Axi_Mux_Addr_Ctrl #(.AXI_CHANNEL_NUM(3)) u_ac (
    .Sys_Clk(clk), .Sys_Rst_N(rstn),
    .O_M_A_VALID(ac_mvalid), .I_M_A_READY(ac_ready),
    .I_S_A_VALID(ac_valid), .O_S_A_READY(ac_sready),
    .Cfg_Arb_Type(1'b1), .Cfg_Cnl_Mask(ac_mask), .I_Cnl_Sw_En(1'b1),
    .O_Act_Cnl_End(ac_end), .O_Act_Cnl_Num(ac_num), .O_Act_Cnl_List(ac_list)
);

always @(posedge clk) begin
    if (rstn && counting && ac_mvalid && ac_ready) begin
        if      (ac_num == 2'd0) g0 = g0 + 1;
        else if (ac_num == 2'd1) g1 = g1 + 1;
        else                     g2 = g2 + 1;
    end
end

// ---------------------------------------------------------------------------
// APB 任务
// ---------------------------------------------------------------------------
task g_wr(input [11:0] a, input [31:0] d);
    begin
        @(posedge clk);
        g_paddr <= a; g_pwdata <= d; g_pwrite <= 1'b1; g_psel <= 1'b1; g_penable <= 1'b0;
        @(posedge clk);
        g_penable <= 1'b1;
        @(posedge clk);
        g_psel <= 1'b0; g_penable <= 1'b0; g_pwrite <= 1'b0;
    end
endtask

task g_rd(input [11:0] a, output [31:0] d);
    begin
        @(posedge clk);
        g_paddr <= a; g_pwrite <= 1'b0; g_psel <= 1'b1; g_penable <= 1'b0;
        @(posedge clk);
        g_penable <= 1'b1;
        @(posedge clk);
        d = g_prdata;
        g_psel <= 1'b0; g_penable <= 1'b0;
    end
endtask

// ---------------------------------------------------------------------------
// 用例
// ---------------------------------------------------------------------------
integer c0, c1, c2;
integer k;
reg [31:0] rd_hits, rd_status, rd_maxar, rd_maxaw, rd_ver;

`ifdef CASE_GRANT_SEQUENCE
// 窗口：设 valid、数 n 个时钟的握手，按通道归类
task ac_window(input [2:0] v, input integer n, output integer o0, output integer o1, output integer o2);
    begin
        counting = 0; ac_valid = 3'b000;
        repeat (4) @(posedge clk);
        g0 = 0; g1 = 0; g2 = 0;
        @(negedge clk);
        ac_valid = v;
        counting = 1;
        repeat (n) @(posedge clk);
        counting = 0;
        ac_valid = 3'b000;
        o0 = g0; o1 = g1; o2 = g2;
    end
endtask
`endif

initial begin
`ifdef CASE_GRANT_SEQUENCE
    // ---------------------------------------------------------------
    // 固定授权序列：三路持续请求 = 10/10/0；仅 S1/S2 = 0/19/0
    // ---------------------------------------------------------------
    rstn = 0;
    repeat (8) @(posedge clk);
    rstn = 1;
    ac_window(3'b111, 40, c0, c1, c2);
    $display("SEQ1 三路持续请求 40 周期: S0=%0d S1=%0d S2=%0d (期望 10 10 0)", c0, c1, c2);
    if (c0 != 10 || c1 != 10 || c2 != 0) errors = errors + 1;
    ac_window(3'b110, 40, c0, c1, c2);
    $display("SEQ2 仅 S1/S2 请求 40 周期: S0=%0d S1=%0d S2=%0d (期望 0 19 0)", c0, c1, c2);
    if (c0 != 0 || c1 != 19 || c2 != 0) errors = errors + 1;

    if (errors == 0)
        $display("RESULT: PASS (固定授权序列与规格 1 节记录一致)");
    else
        $display("RESULT: FAIL (%0d 组序列不符)", errors);

`elsif CASE_DUAL_DIRECTION
    // ---------------------------------------------------------------
    // guard 单元：AR、AW 同时超限，分时完成，两个 protect 都清除之前 S1 不恢复
    // ---------------------------------------------------------------
    rstn = 0;
    repeat (8) @(posedge clk);
    rstn = 1;
    g_wr(12'h008, 32'd3);        // 优先级 + 保护开
    g_wr(12'h010, 32'd4);        // AGE_LIMIT = 4，仿真里缩短
    dd_arvalid = 1; dd_awvalid = 1;
    repeat (12) @(posedge clk);
    g_rd(12'h014, rd_status);
    $display("DUAL 双方向都超限: STATUS=%b MASK=%b (期望 11 与 S1=0)", rd_status[1:0], guard_cnl_mask);
    if (rd_status[1:0] !== 2'b11) errors = errors + 1;
    if (guard_cnl_mask[1] !== 1'b0) errors = errors + 1;

    // 同一拍两个方向都触发：GUARD_HITS 必须加 2（AR、AW 各计一次）
    g_rd(12'h018, rd_hits);
    $display("DUAL 同拍触发: HITS=%0d (期望 2：AR、AW 各一次)", rd_hits);
    if (rd_hits !== 32'd2) errors = errors + 1;

    // 两个方向都还没握手：S2_MAX_WAIT 必须已经在涨（VERSION 2 运行中取最大）。
    // 版本 1 只在握手那一拍更新，这里读到的会是 0，真饿死时寄存器看不到
    g_rd(12'h004, rd_ver);
    g_rd(12'h01C, rd_maxar);
    g_rd(12'h020, rd_maxaw);
    $display("DUAL 未握手时: VERSION=%0d MAX_WAIT_AR=%0d MAX_WAIT_AW=%0d (期望 2 与两者都 >= 12)",
             rd_ver, rd_maxar, rd_maxaw);
    if (rd_ver !== 32'd2) errors = errors + 1;
    if (rd_maxar < 32'd12 || rd_maxaw < 32'd12) errors = errors + 1;

    // 只让 AR 完成：protect_ar 清除，protect_aw 仍在，S1 不得恢复
    dd_arready = 1;
    repeat (4) @(posedge clk);
    dd_arready = 0; dd_arvalid = 0;
    repeat (4) @(posedge clk);
    g_rd(12'h014, rd_status);
    $display("DUAL 只完成 AR 之后: STATUS=%b MASK=%b (期望 10 与 S1=0)", rd_status[1:0], guard_cnl_mask);
    if (rd_status[1] !== 1'b1) errors = errors + 1;      // protect_aw 仍在（STATUS bit1）
    if (guard_cnl_mask[1] !== 1'b0) errors = errors + 1; // S1 不得恢复

    // 再让 AW 完成：两个 protect 都清除，S1 恢复
    dd_awready = 1;
    repeat (4) @(posedge clk);
    dd_awready = 0; dd_awvalid = 0;
    repeat (4) @(posedge clk);
    g_rd(12'h014, rd_status);
    $display("DUAL 两个方向都完成: STATUS=%b MASK=%b (期望 00 与 S1=1)", rd_status[1:0], guard_cnl_mask);
    if (rd_status[1:0] !== 2'b00) errors = errors + 1;
    if (guard_cnl_mask[1] !== 1'b1) errors = errors + 1;

    if (errors == 0)
        $display("RESULT: PASS (两个 protect 都清除之前 S1 不恢复)");
    else
        $display("RESULT: FAIL (%0d 项不符)", errors);

`else
    // ---------------------------------------------------------------
    // 全链路：三个主机 + guard + Axi_Mux + 从机
    // ---------------------------------------------------------------
    rstn = 0;
    repeat (8) @(posedge clk);
    rstn = 1;
    g_wr(12'h008, `CTRL_VAL);              // 仲裁方式与保护开关
    g_wr(12'h00C, 3'b111);                 // 三通道全开
    g_wr(12'h010, `AGE_LIMIT_VAL);         // 保护阈值
    g_wr(12'h024, 32'd1);                  // 清统计

`ifdef HOLD_MIDGRANT
    // 已授权后下游持续不 ready：只验「不伪造抢占」与释放后能恢复，不套用上界
    s1_en = 1; s2_en = 1; s0_en = 1;
    repeat (400) @(posedge clk);
    hold_ar = 1;
    repeat (600) @(posedge clk);
    hold_ar = 0;
    repeat (8000) @(posedge clk);
`else
    s0_en = 1; s1_en = 1; s2_en = 1;
    repeat (`RUN_CYCLES) @(posedge clk);
`endif

    s0_en = 0; s1_en = 0; s2_en = 0;
    repeat (200) @(posedge clk);

    g_rd(12'h018, rd_hits);
    g_rd(12'h014, rd_status);
    g_rd(12'h01C, rd_maxar);
    g_rd(12'h020, rd_maxaw);

    $display("CHAIN 扫描: txn=%0d timeout=%b max_first=%0d max_full=%0d",
             scan_txn, scan_timeout, scan_max_first, scan_max_full);
    $display("CHAIN 引擎: ar_handshake=%0d  CPU: rd=%0d wr=%0d max_ar_wait=%0d max_aw_wait=%0d (读需 %0d 笔)",
             aggr_txn, cpu_done_rd, cpu_done_wr, cpu_max_ar, cpu_max_aw, `CPU_REQS);
    $display("CHAIN guard: hits=%0d status=%b max_wait_ar=%0d max_wait_aw=%0d mask=%b arb=%b",
             rd_hits, rd_status[1:0], rd_maxar, rd_maxaw, guard_cnl_mask, guard_arb_type);
    $display("CHAIN 反压: ARREADY 连续为低最大 %0d 拍，上限 %0d", ar_low_max, `ADDR_STALL_MAX_VAL);
`ifndef CASE_DUAL_DIRECTION
    if (proto_err) $display("CHAIN 协议违例: S1 在握手前撤销了 valid");
    $display("CHAIN 协议: 从机 wlast_err=%0d（WLAST 与 AWLEN 对不上的拍数，必须为 0）", u_ram.wlast_err);
`endif

    // 判据
`ifdef INJECT_MASK_S0
    // 注错用例：S0 被掩码清掉，扫描必须拿不到授权 -> 报 FAIL
    if (scan_timeout && scan_txn == 0)
        $display("RESULT: FAIL (S0 掩码注错生效：扫描一笔都没完成，判据盖住了「不碰扫描输出」)");
    else
        $display("RESULT: PASS (注错没有生效：扫描仍然拿到授权)");
`elsif HOLD_MIDGRANT
    // 已授权后持续反压：S1 不得撤销 valid，释放后 S2 必须恢复
    if (!proto_err && u_ram.wlast_err == 0 && cpu_done_rd >= `CPU_REQS)
        $display("RESULT: PASS (S1 未撤销 valid，释放后 S2 完成 %0d 笔读)", cpu_done_rd);
    else
        $display("RESULT: FAIL (proto_err=%b wlast_err=%0d cpu_rd=%0d：协议违例或 S2 没能恢复)", proto_err, u_ram.wlast_err, cpu_done_rd);
`else
    // 保护开启/关闭跑同一判据，方向由 S1_WRITE 选；阈值来自板上实测。
    // 这里**不做**「关保护就报 FAIL」的硬编码：保护关闭必须靠实测超过阈值才 FAIL，
    // 否则把阈值改宽也看不出来（2026-09-19 复核发现的假绿漏洞）。
`ifdef INFLIGHT1
`define BOUND_VAL `CONTRACT_BOUND_VAL
`else
`define BOUND_VAL `NORMAL_BOUND_VAL
`endif
`ifdef S1_WRITE
    // S1 写满负载：竞争在写仲裁器上，看 S2 的写地址等待
    if (!proto_err && u_ram.wlast_err == 0 && scan_timeout == 1'b0 && cpu_done_wr >= `CPU_REQS && cpu_max_aw <= `BOUND_VAL)
        $display("RESULT: PASS (写方向：S2 写 %0d 笔，最大等待 %0d <= %0d)", cpu_done_wr, cpu_max_aw, `BOUND_VAL);
    else begin
        $display("RESULT: FAIL (写方向：proto_err=%b wlast_err=%0d scan_timeout=%b S2 写=%0d 最大等待=%0d 阈值=%0d)",
                 proto_err, u_ram.wlast_err, scan_timeout, cpu_done_wr, cpu_max_aw, `BOUND_VAL);
        errors = errors + 1;
    end
`else
    // S1 读满负载：竞争在读仲裁器上，看 S2 的读地址等待
    if (!proto_err && u_ram.wlast_err == 0 && scan_timeout == 1'b0 && cpu_done_rd >= `CPU_REQS && cpu_max_ar <= `BOUND_VAL)
        $display("RESULT: PASS (读方向：S2 读 %0d 笔，最大等待 %0d <= %0d)", cpu_done_rd, cpu_max_ar, `BOUND_VAL);
    else begin
        $display("RESULT: FAIL (读方向：proto_err=%b wlast_err=%0d scan_timeout=%b S2 读=%0d 最大等待=%0d 阈值=%0d)",
                 proto_err, u_ram.wlast_err, scan_timeout, cpu_done_rd, cpu_max_ar, `BOUND_VAL);
        errors = errors + 1;
    end
`endif
`endif
`endif

    $display("errors=%0d", errors);
    $finish;
end

// 全局看门狗
initial begin
    #40_000_000;
    $display("RESULT: FAIL (global timeout)");
    $finish;
end

endmodule

// ===========================================================================
// 扫描替身主机：按周期发行一笔 160 拍读，量首拍等待与整笔时间
// ===========================================================================
module master_scan (
    input  wire         clk,
    input  wire         rstn,
    input  wire         enable,
    input  wire [31:0]  period,
    output reg  [31:0]  araddr,
    output wire [7:0]   arlen,
    output wire [2:0]   arsize,
    output wire [1:0]   arburst,
    output reg          arvalid,
    input  wire         arready,
    input  wire [127:0] rdata,
    input  wire         rlast,
    input  wire         rvalid,
    output wire         rready,
    output reg  [31:0]  txn_done,
    output reg  [31:0]  max_first,
    output reg  [31:0]  max_full,
    output reg          timeout
);
    localparam [31:0] WAIT_TIMEOUT = 32'd4000;
    reg [31:0] gap = 0;
    reg [31:0] wait_ar = 0, wait_first = 0, wait_full = 0;
    reg        busy = 0, got = 0;

    assign arlen   = 8'd159;
    assign arsize  = 3'b100;
    assign arburst = 2'b01;
    assign rready  = 1'b1;

    always @(posedge clk) begin
        if (!rstn) begin
            arvalid <= 1'b0; araddr <= 32'h0040_0000; txn_done <= 32'h0;
            max_first <= 32'h0; max_full <= 32'h0; timeout <= 1'b0;
            gap <= 32'h0; busy <= 1'b0; got <= 1'b0;
            wait_ar <= 32'h0; wait_first <= 32'h0; wait_full <= 32'h0;
        end else begin
            // 发行
            if (!arvalid && !busy && enable && (gap == 32'h0)) begin
                arvalid <= 1'b1;
                araddr  <= araddr + 32'd4096;
                wait_ar <= 32'h0;
            end
            if (gap != 32'h0) gap <= gap - 32'h1;

            // 地址等待超时：扫描拿不到授权（掩码注错时就该发生）
            if (arvalid && !arready) begin
                wait_ar <= wait_ar + 32'h1;
                if (wait_ar > WAIT_TIMEOUT) timeout <= 1'b1;
            end

            if (arvalid && arready) begin
                arvalid <= 1'b0;
                busy    <= 1'b1;
                got     <= 1'b0;
                wait_first <= 32'h0;
                wait_full  <= 32'h0;
                gap <= period;
            end

            if (busy) begin
                wait_full <= wait_full + 32'h1;
                if (!got) wait_first <= wait_first + 32'h1;
            end

            if (rvalid) begin
                if (!got) begin
                    got <= 1'b1;
                    if (wait_first > max_first) max_first <= wait_first;
                end
                if (rlast) begin
                    busy <= 1'b0;
                    txn_done <= txn_done + 32'h1;
                    if (wait_full > max_full) max_full <= wait_full;
                end
            end
        end
    end
endmodule

// ===========================================================================
// 引擎替身主机：激进（地址握手后下一拍就再拉高）或每方向一笔在途
// ===========================================================================
module master_aggr (
    input  wire         clk,
    input  wire         rstn,
    input  wire         enable,
    input  wire         mode,        // 0 = 读，1 = 写
    input  wire         inflight1,
    output reg  [31:0]  araddr,
    output wire [7:0]   arlen,
    output wire [2:0]   arsize,
    output wire [1:0]   arburst,
    output reg          arvalid,
    input  wire         arready,
    input  wire [127:0] rdata,
    input  wire         rlast,
    input  wire         rvalid,
    output wire         rready,
    output reg  [31:0]  awaddr,
    output wire [7:0]   awlen,
    output wire [2:0]   awsize,
    output wire [1:0]   awburst,
    output reg          awvalid,
    input  wire         awready,
    output wire [127:0] wdata,
    output wire [15:0]  wstrb,
`ifdef INJECT_WLAST
    output reg          wlast,
`else
    output wire         wlast,
`endif
    output wire         wvalid,
    input  wire         wready,
    input  wire         bvalid,
    output wire         bready,
    output reg  [31:0]  txn_done
);
    reg        rd_busy = 0;
    reg        aw_hold = 0;
    reg [7:0]  wcnt = 0;
    reg [31:0] aw_issued = 0, w_served = 0, b_done = 0;

    // W 通道全部用组合逻辑：有已发出地址、还没发完数据的写就拉高 wvalid，
    // WLAST 由当前拍号直接给出。2026-09-19 之前这两个信号是打一拍的寄存器，
    // 连续握手时末拍漏掉 WLAST、下一笔第 0 拍反而带上，W 错位后整条写通路死锁
    // （INJECT_WLAST 复原这个错误，从机的 wlast_err 必须把它抓出来）。
    assign wvalid = enable && mode && (w_served < aw_issued);
`ifndef INJECT_WLAST
    assign wlast  = (wcnt == 8'd255);
`endif

    assign arlen   = 8'd255;
    assign arsize  = 3'b100;
    assign arburst = 2'b01;
    assign rready  = 1'b1;

    assign awlen   = 8'd255;
    assign awsize  = 3'b100;
    assign awburst = 2'b01;
    assign wdata   = 128'hdead_beef_0000_0000;
    assign wstrb   = 16'hFFFF;
    assign bready  = 1'b1;

    always @(posedge clk) begin
        if (!rstn) begin
            arvalid <= 1'b0; awvalid <= 1'b0;
`ifdef INJECT_WLAST
            wlast <= 1'b0;
`endif
            rd_busy <= 1'b0; wcnt <= 8'd0; txn_done <= 32'h0;
            aw_issued <= 32'h0; w_served <= 32'h0; b_done <= 32'h0; aw_hold <= 1'b0;
            araddr <= 32'h0010_0000; awaddr <= 32'h0020_0000;
        end else begin
            // 读方向
            if ((mode == 1'b0) && enable) begin
                if (inflight1) begin
                    if (!rd_busy) arvalid <= 1'b1;
                end else
                    arvalid <= 1'b1;           // 激进：一直拉高

                if (arvalid && arready) begin
                    araddr   <= araddr + 32'd4096;
                    txn_done <= txn_done + 32'h1;
                    if (inflight1) begin
                        arvalid <= 1'b0;
                        rd_busy <= 1'b1;
                    end
                end
                if (inflight1 && rd_busy && rvalid && rlast) rd_busy <= 1'b0;
            end else
                arvalid <= 1'b0;

            // 写方向：激进时地址流水，W 拍按序发；valid 一旦拉高就保持到握手（AXI 要求）。
            // inflight1 按在途契约（每方向一笔在途）：上一笔的写响应（bvalid 握手，bready 恒 1）回来之前不拉下一笔 awvalid。
            if ((mode == 1'b1) && enable) begin
                if (awvalid) begin
                    if (awready) begin
                        aw_issued <= aw_issued + 32'h1;
                        awaddr    <= awaddr + 32'd4096;
                        awvalid   <= inflight1 ? 1'b0 : 1'b1;
                        aw_hold   <= inflight1;
                    end
                end else if (!aw_hold || (aw_issued == b_done)) begin
                    awvalid <= 1'b1;
                    aw_hold <= 1'b0;
                end
                if (bvalid) b_done <= b_done + 32'h1;
                if (wvalid && wready) begin
                    if (wcnt == 8'd255) begin
                        wcnt     <= 8'd0;
                        w_served <= w_served + 32'h1;
                    end else
                        wcnt <= wcnt + 8'd1;
                end
`ifdef INJECT_WLAST
                wlast <= (wcnt == 8'd255);     // 注错：打一拍的旧写法
`endif
            end else begin
                awvalid <= 1'b0;
            end
        end
    end
endmodule

// ===========================================================================
// CPU 替身主机：随机间隔的短读写，量每笔的地址等待
// ===========================================================================
module master_cpu (
    input  wire         clk,
    input  wire         rstn,
    input  wire         enable,
    output reg  [31:0]  araddr,
    output wire [7:0]   arlen,
    output wire [2:0]   arsize,
    output wire [1:0]   arburst,
    output reg          arvalid,
    input  wire         arready,
    input  wire [127:0] rdata,
    input  wire         rlast,
    input  wire         rvalid,
    output wire         rready,
    output reg  [31:0]  awaddr,
    output wire [7:0]   awlen,
    output wire [2:0]   awsize,
    output wire [1:0]   awburst,
    output reg          awvalid,
    input  wire         awready,
    output wire [127:0] wdata,
    output wire [15:0]  wstrb,
    output wire         wlast,
    output wire         wvalid,
    input  wire         wready,
    input  wire         bvalid,
    output wire         bready,
    output reg  [31:0]  done_rd,
    output reg  [31:0]  done_wr,
    output reg  [31:0]  max_ar_wait,
    output reg  [31:0]  max_aw_wait
);
    integer    seed = 3;
    reg [31:0] gap = 0;
    reg [31:0] wait_ar = 0, wait_aw = 0;
    reg        rd_phase = 0;

    assign arlen   = 8'd3;          // 4 拍读
    assign arsize  = 3'b100;
    assign arburst = 2'b01;
    assign rready  = 1'b1;

    assign awlen   = 8'd0;          // 单拍写
    assign awsize  = 3'b100;
    assign awburst = 2'b01;
    assign wdata   = 128'h0000_0000_0000_1234;
    assign wstrb   = 16'hFFFF;
    assign wvalid  = enable;        // 单拍写：W 可以早于 AW 出现
    assign wlast   = 1'b1;
    assign bready  = 1'b1;

    // 最大地址等待按运行中取最大：每拍都比较，卡住不握手的那一笔也能看到。
    // 2026-09-19 之前只在握手那一拍更新，写通路死锁时报出来的是「已完成那几笔」的最大值 610，
    // 真正卡住的那笔已经等了一万多拍却不出现在任何统计里。
    always @(posedge clk) begin
        if (!rstn) begin
            max_ar_wait <= 32'h0; max_aw_wait <= 32'h0;
        end else if (enable) begin
            if (arvalid && !arready && (wait_ar + 32'h1 > max_ar_wait)) max_ar_wait <= wait_ar + 32'h1;
            if (awvalid && !awready && (wait_aw + 32'h1 > max_aw_wait)) max_aw_wait <= wait_aw + 32'h1;
        end
    end

    always @(posedge clk) begin
        if (!rstn) begin
            arvalid <= 1'b0; awvalid <= 1'b0; done_rd <= 32'h0; done_wr <= 32'h0;
            gap <= 32'h0; wait_ar <= 32'h0; wait_aw <= 32'h0; rd_phase <= 1'b0;
            araddr <= 32'h0000_1000; awaddr <= 32'h0000_2000;
        end else if (!enable) begin
            arvalid <= 1'b0; awvalid <= 1'b0;
        end else if (gap != 32'h0) begin
            gap <= gap - 32'h1;
            arvalid <= 1'b0;
            awvalid <= 1'b0;
        end else if (rd_phase) begin
            awvalid <= 1'b0;
            arvalid <= 1'b1;
            if (arvalid && arready) begin
                wait_ar <= 32'h0;
                arvalid <= 1'b0;
                araddr  <= araddr + 32'd64;
                done_rd <= done_rd + 32'h1;
                rd_phase <= 1'b0;
                gap <= {28'h0, $random(seed)} & 32'h0000_0007;
            end else
                wait_ar <= wait_ar + 32'h1;
        end else begin
            arvalid <= 1'b0;
            awvalid <= 1'b1;
            if (awvalid && awready) begin
                wait_aw <= 32'h0;
                awvalid <= 1'b0;
                awaddr  <= awaddr + 32'd64;
                done_wr <= done_wr + 32'h1;
                rd_phase <= 1'b1;
                gap <= {28'h0, $random(seed)} & 32'h0000_0007;
            end else
                wait_aw <= wait_aw + 32'h1;
        end
    end
endmodule

`default_nettype wire
