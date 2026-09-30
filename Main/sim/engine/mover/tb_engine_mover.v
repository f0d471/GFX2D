// 引擎 P1 B2：搬运层 engine_mover 的测试台
//
// 被测是 rtl/engine/engine_mover.v 整块，经 rtl/vendor/axi_mux/Axi_Mux.v（S1）
// 接到补齐四样的 sim/axi_probe/axi_ram_model.v。测试台扮演三边：
//   几何层（I2 发读写描述符）、计算阵列（I3 收读流、I4 送写流）、控制层（I7 发排空请求与门限）
//
// 用例（用 +define+ 选，见 run.sh）：
//   CASE_NORMAL   读侧仲裁（命令取数优先）、读流分发与上下文、写掩码、状态里的命令索引与末行标记、
//                 在途契约、地址与突发约束、正常运行不得置超时
//   CASE_DRAIN    读、写突发中途各排空一次；「控制层复位」那一组消费者撤 ready、写源停供，
//                 「软复位」那一组消费者维持 ready（搬运层看不到区别，只有激励不同）；
//                 另有一组请求在完成之后继续举着：drain_done 只打一次，请求撤下之前不收新提交
//   CASE_ERR      读回 SLVERR 与写响应 SLVERR 各一例，状态里的错误与命令索引正确
//   CASE_TIMEOUT  从机按住超过门限：报超时、不放弃；放开后照常完成，数据属于原来那一笔
//
// 判据全文见 engine 第 02 章 3.11 节；注错位见 engine_mover.v 文件头。
`timescale 1ns/1ps
`default_nettype none

module tb_engine_mover;

`ifdef INJECT_VAL
    localparam integer INJECT = `INJECT_VAL;
`else
    localparam integer INJECT = 0;
`endif
`ifdef TMO_LIMIT_VAL
    localparam integer TMO_LIMIT = `TMO_LIMIT_VAL;
`else
    localparam integer TMO_LIMIT = 4096;   // D30 的复位值
`endif
`ifdef SEED_VAL
    localparam integer SEED = `SEED_VAL;
`else
    localparam integer SEED = 7;
`endif
`ifdef STALL_VAL
    localparam integer STALL = `STALL_VAL;
`else
    localparam integer STALL = 3;
`endif

    // 存储区域（以 128 位字计），每区 64 字
    localparam integer PB = 32'h0100;   // 像素读
    localparam integer CB = 32'h0200;   // 命令取数
    localparam integer WB = 32'h0300;   // 写目的

    localparam integer TMO_W = 13;

    // 错误区间：CASE_ERR 用 ERR_REGION 打开，落在 0x9000~0x9FFF 的突发回 SLVERR
`ifdef ERR_REGION
    localparam [31:0] ERR_LO_P = 32'h0000_9000;
    localparam [31:0] ERR_HI_P = 32'h0000_9FFF;
`else
    localparam [31:0] ERR_LO_P = 32'hFFFF_FFFF;
    localparam [31:0] ERR_HI_P = 32'h0000_0000;
`endif

    reg clk = 0, rst = 1;
    always #5 clk = ~clk;

    integer errors = 0;
    integer case_id = 0;

    // 内存已知图案：低 32 位放字号，高 96 位放固定标记
    function [127:0] pat;
        input integer idx;
        reg [31:0] t;
        begin
            t = idx;
            pat = {96'h5A5A5A5A5A5A5A5A5A5A5A5A, t};
        end
    endfunction

    // 写源图案：像素载荷 24 位，X 字节给出「不写」标记
    function [23:0] wpay;
        input integer tag, i, p;
        begin
            wpay = ((tag * 64 + i * 4 + p) & 24'hFFFFFF);
        end
    endfunction
    function wkill;
        input integer i, p;
        begin
            wkill = (((i + p) % 3) == 0);
        end
    endfunction
    function [31:0] wpix;
        input integer tag, i, p;
        begin
            wpix = {8'h00, wpay(tag,i,p)} | (wkill(i,p) ? 32'h8000_0000 : 32'h0000_0000);
        end
    endfunction

    // =====================================================================
    // 被测端口
    // =====================================================================
    reg  [31:0] rd_addr = 0; reg [11:0] rd_bytes = 0; reg [1:0] rd_buf = 0;
    reg  [3:0]  rd_cidx = 0; reg rd_valid = 0; wire rd_ready;
    reg  [31:0] fd_addr = 0; reg [11:0] fd_bytes = 0;
    reg  [3:0]  fd_cidx = 0; reg fd_valid = 0; wire fd_ready;

    wire [127:0] rs_tdata; wire [15:0] rs_tkeep; wire rs_tlast, rs_tvalid; wire [1:0] rs_buf;
    wire [127:0] fs_tdata; wire [15:0] fs_tkeep; wire fs_tlast, fs_tvalid; wire [3:0] fs_cidx;
    reg          rs_ready = 1, fs_ready = 1;

    reg  [31:0] wr_addr = 0; reg [11:0] wr_bytes = 0; reg [3:0] wr_cidx = 0;
    reg         wr_last_row = 0; reg wr_valid = 0; wire wr_ready;
    reg  [127:0] ws_tdata = 0; reg [15:0] ws_tkeep = 16'hFFFF; reg ws_tlast = 0;
    reg  ws_tvalid = 0; wire ws_tready;

    wire rd_stat_valid, rd_stat_err; wire [3:0] rd_stat_cidx; wire [1:0] rd_stat_buf;
    wire wr_stat_valid, wr_stat_err; wire [3:0] wr_stat_cidx; wire wr_stat_last_row;

    reg  drain_req = 0; wire drain_busy, drain_done;
    reg  [TMO_W-1:0] tmo_limit = TMO_LIMIT;
    reg  err_clr = 0;
    wire tmo_evt, tmo_dir;

    wire [31:0]  m_araddr; wire [7:0] m_arlen; wire [2:0] m_arsize; wire [1:0] m_arburst;
    wire m_arvalid, m_arready;
    wire [127:0] m_rdata; wire [1:0] m_rresp; wire m_rlast, m_rvalid, m_rready;
    wire [31:0]  m_awaddr; wire [7:0] m_awlen; wire [2:0] m_awsize; wire [1:0] m_awburst;
    wire m_awvalid, m_awready;
    wire [127:0] m_wdata; wire [15:0] m_wstrb; wire m_wlast, m_wvalid, m_wready;
    wire [1:0]   m_bresp; wire m_bvalid, m_bready;

    engine_mover #(
        .AXI_MAX_BURST_LEN(192), .TMO_WIDTH(TMO_W), .INJECT(INJECT)
    ) dut (
        .clk(clk), .rst(rst),
        .rd_addr(rd_addr), .rd_bytes(rd_bytes), .rd_buf(rd_buf), .rd_cidx(rd_cidx),
        .rd_valid(rd_valid), .rd_ready(rd_ready),
        .fd_addr(fd_addr), .fd_bytes(fd_bytes), .fd_cidx(fd_cidx),
        .fd_valid(fd_valid), .fd_ready(fd_ready),
        .rs_tdata(rs_tdata), .rs_tkeep(rs_tkeep), .rs_tlast(rs_tlast), .rs_tvalid(rs_tvalid),
        .rs_tready(rs_ready), .rs_buf(rs_buf),
        .fs_tdata(fs_tdata), .fs_tkeep(fs_tkeep), .fs_tlast(fs_tlast), .fs_tvalid(fs_tvalid),
        .fs_tready(fs_ready), .fs_cidx(fs_cidx),
        .wr_addr(wr_addr), .wr_bytes(wr_bytes), .wr_cidx(wr_cidx), .wr_last_row(wr_last_row),
        .wr_valid(wr_valid), .wr_ready(wr_ready),
        .ws_tdata(ws_tdata), .ws_tkeep(ws_tkeep), .ws_tlast(ws_tlast),
        .ws_tvalid(ws_tvalid), .ws_tready(ws_tready),
        .rd_stat_valid(rd_stat_valid), .rd_stat_err(rd_stat_err),
        .rd_stat_cidx(rd_stat_cidx), .rd_stat_buf(rd_stat_buf),
        .wr_stat_valid(wr_stat_valid), .wr_stat_err(wr_stat_err),
        .wr_stat_cidx(wr_stat_cidx), .wr_stat_last_row(wr_stat_last_row),
        .drain_req(drain_req), .drain_busy(drain_busy), .drain_done(drain_done),
        .tmo_limit(tmo_limit), .err_clr(err_clr), .tmo_evt(tmo_evt), .tmo_dir(tmo_dir),
        .m_axi_araddr(m_araddr), .m_axi_arlen(m_arlen), .m_axi_arsize(m_arsize),
        .m_axi_arburst(m_arburst), .m_axi_arvalid(m_arvalid), .m_axi_arready(m_arready),
        .m_axi_rdata(m_rdata), .m_axi_rresp(m_rresp), .m_axi_rlast(m_rlast),
        .m_axi_rvalid(m_rvalid), .m_axi_rready(m_rready),
        .m_axi_awaddr(m_awaddr), .m_axi_awlen(m_awlen), .m_axi_awsize(m_awsize),
        .m_axi_awburst(m_awburst), .m_axi_awvalid(m_awvalid), .m_axi_awready(m_awready),
        .m_axi_wdata(m_wdata), .m_axi_wstrb(m_wstrb), .m_axi_wlast(m_wlast),
        .m_axi_wvalid(m_wvalid), .m_axi_wready(m_wready),
        .m_axi_bresp(s_bresp), .m_axi_bvalid(m_bvalid), .m_axi_bready(m_bready)
    );

    // ---- 真实 Axi_Mux（仓内源码，X3，板上现行配置：优先级仲裁、三通道全开）----
    // 注意：Axi_Mux 没有 BRESP 端口（rtl/README.md 已登记），顶层把控制器的 bresp 并接给各主机，
    // 这里照做——写响应直接由从机给被测，不经过 mux。
    wire [31:0]  s_awaddr; wire [7:0] s_awlen; wire [2:0] s_awsize; wire [1:0] s_awburst;
    wire s_awvalid, s_awready;
    wire [127:0] s_wdata; wire [15:0] s_wstrb; wire s_wlast, s_wvalid, s_wready;
    wire s_bvalid, s_bready; wire [1:0] s_bresp;
    wire [31:0]  s_araddr; wire [7:0] s_arlen; wire [2:0] s_arsize; wire [1:0] s_arburst;
    wire s_arvalid, s_arready;
    wire [127:0] s_rdata; wire [1:0] s_rresp; wire s_rlast, s_rvalid, s_rready;
    wire [7:0]   s_awid, s_wid, s_bid, s_arid, s_rid;
    wire [1:0]   s_awlock, s_arlock;

    Axi_Mux #(.SLAVE_ID_WIDTH(8), .ID_MODE_SEL("Normal"), .AXI_DATA_WIDTH(128)) u_mux (
        .Sys_Clk(clk), .Sys_Rst_N(~rst),
        .O_M_AW_ID(s_awid), .O_M_AW_ADDR(s_awaddr), .O_M_AW_LEN(s_awlen), .O_M_AW_SIZE(s_awsize),
        .O_M_AW_BURST(s_awburst), .O_M_AW_LOCK(s_awlock), .O_M_AW_VALID(s_awvalid),
        .I_M_AW_READY(s_awready),
        .O_M_W_ID(s_wid), .O_M_W_DATA(s_wdata), .O_M_W_LAST(s_wlast), .O_M_W_STRB(s_wstrb),
        .O_M_W_VALID(s_wvalid), .I_M_W_READY(s_wready),
        .I_M_B_ID(8'h0), .I_M_B_VALID(s_bvalid), .O_M_B_READY(s_bready),
        .O_M_AR_ID(s_arid), .O_M_AR_ADDR(s_araddr), .O_M_AR_LEN(s_arlen), .O_M_AR_SIZE(s_arsize),
        .O_M_AR_BURST(s_arburst), .O_M_AR_LOCK(s_arlock), .O_M_AR_VALID(s_arvalid),
        .I_M_AR_READY(s_arready),
        .I_M_R_ID(8'h0), .I_M_R_DATA(s_rdata), .I_M_R_LAST(s_rlast), .I_M_R_RESP(s_rresp),
        .I_M_R_VALID(s_rvalid), .O_M_R_READY(s_rready),

        .I_S0_AW_ID(8'h0), .I_S0_AW_ADDR(32'h0), .I_S0_AW_LEN(8'h0), .I_S0_AW_SIZE(3'b000),
        .I_S0_AW_BURST(2'b00), .I_S0_AW_LOCK(2'b00), .I_S0_AW_VALID(1'b0), .O_S0_AW_READY(),
        .I_S0_W_ID(8'h0), .I_S0_W_DATA(128'h0), .I_S0_W_LAST(1'b0), .I_S0_W_STRB(16'h0),
        .I_S0_W_VALID(1'b0), .O_S0_W_READY(),
        .O_S0_B_ID(), .O_S0_B_VALID(), .I_S0_B_READY(1'b0),
        .I_S0_AR_ID(8'h0), .I_S0_AR_ADDR(32'h0), .I_S0_AR_LEN(8'h0), .I_S0_AR_SIZE(3'b000),
        .I_S0_AR_BURST(2'b00), .I_S0_AR_LOCK(2'b00), .I_S0_AR_VALID(1'b0), .O_S0_AR_READY(),
        .O_S0_R_ID(), .O_S0_R_DATA(), .O_S0_R_LAST(), .O_S0_R_RESP(), .O_S0_R_VALID(),
        .I_S0_R_READY(1'b0),

        .I_S1_AW_ID(8'h0), .I_S1_AW_ADDR(m_awaddr), .I_S1_AW_LEN(m_awlen), .I_S1_AW_SIZE(m_awsize),
        .I_S1_AW_BURST(m_awburst), .I_S1_AW_LOCK(2'b00), .I_S1_AW_VALID(m_awvalid),
        .O_S1_AW_READY(m_awready),
        .I_S1_W_ID(8'h0), .I_S1_W_DATA(m_wdata), .I_S1_W_LAST(m_wlast), .I_S1_W_STRB(m_wstrb),
        .I_S1_W_VALID(m_wvalid), .O_S1_W_READY(m_wready),
        .O_S1_B_ID(), .O_S1_B_VALID(m_bvalid), .I_S1_B_READY(m_bready),
        .I_S1_AR_ID(8'h0), .I_S1_AR_ADDR(m_araddr), .I_S1_AR_LEN(m_arlen), .I_S1_AR_SIZE(m_arsize),
        .I_S1_AR_BURST(m_arburst), .I_S1_AR_LOCK(2'b00), .I_S1_AR_VALID(m_arvalid),
        .O_S1_AR_READY(m_arready),
        .O_S1_R_ID(), .O_S1_R_DATA(m_rdata), .O_S1_R_LAST(m_rlast), .O_S1_R_RESP(m_rresp),
        .O_S1_R_VALID(m_rvalid), .I_S1_R_READY(m_rready),

        .I_S2_AW_ID(8'h0), .I_S2_AW_ADDR(32'h0), .I_S2_AW_LEN(8'h0), .I_S2_AW_SIZE(3'b000),
        .I_S2_AW_BURST(2'b00), .I_S2_AW_LOCK(2'b00), .I_S2_AW_VALID(1'b0), .O_S2_AW_READY(),
        .I_S2_W_ID(8'h0), .I_S2_W_DATA(128'h0), .I_S2_W_LAST(1'b0), .I_S2_W_STRB(16'h0),
        .I_S2_W_VALID(1'b0), .O_S2_W_READY(),
        .O_S2_B_ID(), .O_S2_B_VALID(), .I_S2_B_READY(1'b0),
        .I_S2_AR_ID(8'h0), .I_S2_AR_ADDR(32'h0), .I_S2_AR_LEN(8'h0), .I_S2_AR_SIZE(3'b000),
        .I_S2_AR_BURST(2'b00), .I_S2_AR_LOCK(2'b00), .I_S2_AR_VALID(1'b0), .O_S2_AR_READY(),
        .O_S2_R_ID(), .O_S2_R_DATA(), .O_S2_R_LAST(), .O_S2_R_RESP(), .O_S2_R_VALID(),
        .I_S2_R_READY(1'b0),

        .Cfg_Arb_Type(1'b1), .Cfg_Cnl_Mask(3'b111),
        .O_AW_Cnl_Num(), .O_W_Cnl_Num(), .O_B_Cnl_Num(), .O_AR_Cnl_Num(), .O_R_Cnl_Num()
    );

    // ---- 从机的持续反压：超时用例用它模拟「从机不再响应」。
    //      不自己在主机与 mux 之间造闸门：那个闸门只挡一边，mux 会以为已经握手、
    //      下游却没收，总线直接挂在一个不自洽的状态上。
    reg bus_stall = 0;

    axi_ram_model #(
        .MAX_LEN(255), .SEED(SEED), .STALL(STALL),
        .ADDR_STALL_MAX(0), .RD_QUEUE(1), .WR_QUEUE(1),
        .MEM_WORDS(4096), .STRB_EN(1),
        .ERR_LO(ERR_LO_P), .ERR_HI(ERR_HI_P),
        .TIMED(0)
    ) u_slave (
        .clk(clk), .rst_n(~rst),
        .awaddr(s_awaddr), .awlen(s_awlen), .awvalid(s_awvalid), .awready(s_awready),
        .wdata(s_wdata), .wstrb(s_wstrb), .wlast(s_wlast), .wvalid(s_wvalid), .wready(s_wready),
        .bresp(s_bresp), .bvalid(s_bvalid), .bready(s_bready),
        .araddr(s_araddr), .arlen(s_arlen), .arvalid(s_arvalid), .arready(s_arready),
        .rdata(s_rdata), .rresp(s_rresp), .rlast(s_rlast), .rvalid(s_rvalid), .rready(s_rready),
        .hold_aw(bus_stall), .hold_ar(bus_stall)
    );

    // =====================================================================
    // 总线监视：契约、地址、突发、rready、WLAST
    // =====================================================================
    integer rd_out = 0, wr_out = 0, rd_viol = 0, wr_viol = 0;
    integer ar_seen = 0, aw_seen = 0;
    integer ar_per_txn = 0, aw_per_txn = 0;
    integer multi_burst = 0, unaligned = 0, cross4k = 0;
    integer rready_bubble = 0, rd_data_phase = 0;
    integer tmo_pulses = 0, tmo_rd_pulses = 0, tmo_wr_pulses = 0;
    integer ar_order_fd = 0, ar_order_pix = 0, seq = 0;

    always @(posedge clk) if (!rst) begin
        // 在途契约：上一笔没结束时下一笔的 valid 拉高一个周期即违例（按 valid 判）
        if (m_arvalid && rd_out >= 1) rd_viol = rd_viol + 1;
        if (m_awvalid && wr_out >= 1) wr_viol = wr_viol + 1;

        rd_out = rd_out + ((m_arvalid && m_arready) ? 1 : 0)
                         - ((m_rvalid && m_rready && m_rlast) ? 1 : 0);
        wr_out = wr_out + ((m_awvalid && m_awready) ? 1 : 0)
                         - ((m_bvalid && m_bready) ? 1 : 0);

        // 地址与突发约束
        if (m_arvalid && m_arready) begin
            ar_seen = ar_seen + 1;
            ar_per_txn = ar_per_txn + 1;
            if (m_araddr[3:0] != 4'h0) unaligned = unaligned + 1;
            if ((m_araddr >> 12) != ((m_araddr + (m_arlen << 4)) >> 12)) cross4k = cross4k + 1;
            seq = seq + 1;
            if (m_araddr >= (CB << 4) && m_araddr < ((CB + 32'h40) << 4)) begin
                if (ar_order_fd == 0) ar_order_fd = seq;
            end else if (ar_order_pix == 0) ar_order_pix = seq;
        end
        if (m_awvalid && m_awready) begin
            aw_seen = aw_seen + 1;
            aw_per_txn = aw_per_txn + 1;
            if (m_awaddr[3:0] != 4'h0) unaligned = unaligned + 1;
            if ((m_awaddr >> 12) != ((m_awaddr + (m_awlen << 4)) >> 12)) cross4k = cross4k + 1;
        end

        // 一个描述符恰好一笔突发
        if (m_rvalid && m_rready && m_rlast) begin
            if (ar_per_txn != 1) multi_burst = multi_burst + 1;
            ar_per_txn = 0;
        end
        if (m_bvalid && m_bready) begin
            if (aw_per_txn != 1) multi_burst = multi_burst + 1;
            aw_per_txn = 0;
        end

        // 读数据阶段 rready 恒为 1
        if (m_rvalid) rd_data_phase = rd_data_phase + 1;
        if (m_rvalid && !m_rready) rready_bubble = rready_bubble + 1;

        // 超时事件
        if (tmo_evt) begin
            tmo_pulses = tmo_pulses + 1;
            if (tmo_dir) tmo_wr_pulses = tmo_wr_pulses + 1;
            else         tmo_rd_pulses = tmo_rd_pulses + 1;
        end
    end

    // =====================================================================
    // 读流监视：数据与上下文（目标缓冲号 / 命令索引）必须属于产生它的那个描述符
    // =====================================================================
    reg         exp_pix_en = 0;
    reg  [1:0]  exp_pix_buf = 0;
    integer     exp_pix_idx = 0, exp_pix_beats = 0, got_pix_beats = 0;

    always @(posedge clk) if (!rst && exp_pix_en && rs_tvalid && rs_ready) begin
        if (rs_buf !== exp_pix_buf) begin
            $display("ERR 像素读流的目标缓冲号 %0d != %0d（第 %0d 拍）", rs_buf, exp_pix_buf, got_pix_beats);
            errors = errors + 1;
        end
        if (rs_tdata !== pat(exp_pix_idx)) begin
            $display("ERR 像素读流数据不符：字 %0d got %h want %h", exp_pix_idx, rs_tdata, pat(exp_pix_idx));
            errors = errors + 1;
        end
        exp_pix_idx = exp_pix_idx + 1;
        got_pix_beats = got_pix_beats + 1;
        if (rs_tlast) begin
            if (got_pix_beats != exp_pix_beats) begin
                $display("ERR 像素读流拍数 %0d != %0d", got_pix_beats, exp_pix_beats);
                errors = errors + 1;
            end
            exp_pix_en = 0;
        end
    end

    reg         exp_cmd_en = 0;
    reg  [3:0]  exp_cmd_cidx = 0;
    integer     exp_cmd_idx = 0, exp_cmd_beats = 0, got_cmd_beats = 0;

    always @(posedge clk) if (!rst && exp_cmd_en && fs_tvalid && fs_ready) begin
        if (fs_cidx !== exp_cmd_cidx) begin
            $display("ERR 命令取数流的命令索引 %0d != %0d", fs_cidx, exp_cmd_cidx);
            errors = errors + 1;
        end
        if (fs_tdata !== pat(exp_cmd_idx)) begin
            $display("ERR 命令取数流数据不符：字 %0d", exp_cmd_idx);
            errors = errors + 1;
        end
        exp_cmd_idx = exp_cmd_idx + 1;
        got_cmd_beats = got_cmd_beats + 1;
        if (fs_tlast) begin
            if (got_cmd_beats != exp_cmd_beats) begin
                $display("ERR 命令取数流拍数 %0d != %0d", got_cmd_beats, exp_cmd_beats);
                errors = errors + 1;
            end
            exp_cmd_en = 0;
        end
    end

    // =====================================================================
    // 状态监视：读状态带缓冲号与命令索引，写状态带命令索引与末行标记
    // =====================================================================
    integer     exp_rd_st_cidx = -1, exp_wr_st_cidx = -1, exp_wr_st_last = -1;
    reg  [1:0]  exp_rd_st_buf = 0;
    integer     got_rd_st = 0, got_wr_st = 0;

    always @(posedge clk) if (!rst && rd_stat_valid) begin
        got_rd_st = got_rd_st + 1;
        if (exp_rd_st_cidx >= 0 && rd_stat_cidx !== exp_rd_st_cidx[3:0]) begin
            $display("ERR 读状态的命令索引 %0d != %0d", rd_stat_cidx, exp_rd_st_cidx);
            errors = errors + 1;
        end
        if (exp_rd_st_cidx >= 0 && rd_stat_buf !== exp_rd_st_buf) begin
            $display("ERR 读状态的缓冲号 %0d != %0d", rd_stat_buf, exp_rd_st_buf);
            errors = errors + 1;
        end
    end

    always @(posedge clk) if (!rst && wr_stat_valid) begin
        got_wr_st = got_wr_st + 1;
        if (exp_wr_st_cidx >= 0 && wr_stat_cidx !== exp_wr_st_cidx[3:0]) begin
            $display("ERR 写状态的命令索引 %0d != %0d", wr_stat_cidx, exp_wr_st_cidx);
            errors = errors + 1;
        end
        if (exp_wr_st_last >= 0 && wr_stat_last_row !== exp_wr_st_last[0]) begin
            $display("ERR 写状态的末行标记 %0d != %0d", wr_stat_last_row, exp_wr_st_last);
            errors = errors + 1;
        end
    end

    // =====================================================================
    // 写数据源
    // =====================================================================
    reg         wr_src_en = 0;
    integer     wsrc_i = 0, wsrc_total = 0, wsrc_stop = 0, wsrc_tag = 0;
    reg  [127:0] wsrc_data = 0;
    reg          wsrc_last = 0;

    always @(*) begin
        wsrc_data[31:0]   = wpix(wsrc_tag, wsrc_i, 0);
        wsrc_data[63:32]  = wpix(wsrc_tag, wsrc_i, 1);
        wsrc_data[95:64]  = wpix(wsrc_tag, wsrc_i, 2);
        wsrc_data[127:96] = wpix(wsrc_tag, wsrc_i, 3);
        wsrc_last = (wsrc_i == wsrc_total - 1);
    end

    always @(negedge clk) begin
        ws_tvalid = wr_src_en && (wsrc_i < wsrc_stop);
        ws_tdata  = wsrc_data;
        ws_tkeep  = 16'hFFFF;
        ws_tlast  = wsrc_last;
    end

    always @(posedge clk) if (!rst && ws_tvalid && ws_tready) wsrc_i = wsrc_i + 1;

    // 描述符握手计数：任务用它判「这一次举手已经握上」
    integer rd_fire_n = 0, fd_fire_n = 0, wr_fire_n = 0;
    always @(posedge clk) if (!rst) begin
        if (rd_valid && rd_ready) rd_fire_n <= rd_fire_n + 1;
        if (fd_valid && fd_ready) fd_fire_n <= fd_fire_n + 1;
        if (wr_valid && wr_ready) wr_fire_n <= wr_fire_n + 1;
    end

    // =====================================================================
    // 核对写目的区
    // =====================================================================
    task verify_write;
        input integer aw;
        input integer nb;
        input integer tag;
        input integer fed;
        integer i, p, idx;
        reg [127:0] wgot, wold;
        reg [31:0] exp_pix, got_pix;
        begin
            for (i = 0; i < nb; i = i + 1) begin
                idx = aw + i;
                wgot = u_slave.mem[idx];
                wold = pat(idx);
                for (p = 0; p < 4; p = p + 1) begin
                    got_pix = wgot[32*p +: 32];
                    if (i < fed && !wkill(i,p)) exp_pix = {8'h00, wpay(tag,i,p)};
                    else                        exp_pix = wold[32*p +: 32];
                    if (got_pix !== exp_pix) begin
                        $display("ERR 写目的字 %0d 像素 %0d got %h want %h (拍 %0d)", idx, p, got_pix, exp_pix, i);
                        errors = errors + 1;
                    end
                end
            end
        end
    endtask

    // =====================================================================
    // 驱动任务
    // =====================================================================
    task pix_read;
        input integer aw;
        input integer nb;
        input [1:0]   bnum;
        input [3:0]   cidx;
        integer base;
        begin
            exp_pix_buf = bnum; exp_pix_beats = nb; exp_pix_idx = aw; exp_pix_en = 1; got_pix_beats = 0;
            exp_rd_st_cidx = cidx; exp_rd_st_buf = bnum;
            @(negedge clk);
            base = rd_fire_n;
            rd_addr = aw << 4; rd_bytes = nb * 16; rd_buf = bnum; rd_cidx = cidx; rd_valid = 1;
            @(negedge clk);
            while (rd_fire_n == base) @(negedge clk);
            rd_valid = 0;
            while (exp_pix_en) @(negedge clk);
            @(negedge clk);
        end
    endtask

    task cmd_read;
        input integer aw;
        input integer nb;
        input [3:0]   cidx;
        integer base;
        begin
            exp_cmd_cidx = cidx; exp_cmd_beats = nb; exp_cmd_idx = aw; exp_cmd_en = 1; got_cmd_beats = 0;
            exp_rd_st_cidx = cidx; exp_rd_st_buf = 2'd0;
            @(negedge clk);
            base = fd_fire_n;
            fd_addr = aw << 4; fd_bytes = nb * 16; fd_cidx = cidx; fd_valid = 1;
            @(negedge clk);
            while (fd_fire_n == base) @(negedge clk);
            fd_valid = 0;
            while (exp_cmd_en) @(negedge clk);
            @(negedge clk);
        end
    endtask

    task do_write;
        input integer aw;
        input integer nb;
        input integer tag;
        input integer lastrow;
        input integer stop;
        integer base;
        begin
            wsrc_tag = tag; wsrc_total = nb; wsrc_stop = stop; wsrc_i = 0; wr_src_en = 1;
            exp_wr_st_cidx = tag; exp_wr_st_last = lastrow;
            @(negedge clk);
            base = wr_fire_n;
            wr_addr = aw << 4; wr_bytes = nb * 16; wr_cidx = tag[3:0];
            wr_last_row = lastrow[0]; wr_valid = 1;
            @(negedge clk);
            while (wr_fire_n == base) @(negedge clk);
            wr_valid = 0;
        end
    endtask

    task wait_wr_status;
        integer guard;
        begin
            guard = 0;
            while (!wr_stat_valid && guard < 20000) begin @(negedge clk); guard = guard + 1; end
            if (!wr_stat_valid) begin
                $display("ERR 写状态一直没回来");
                errors = errors + 1;
            end
            @(negedge clk);
        end
    endtask

    // 排空：拉高请求，等完成脉冲；返回等了多少拍
    task do_drain;
        output integer waited;
        begin
            @(negedge clk);
            drain_req = 1;
            waited = 0;
            while (!drain_done && waited < 20000) begin @(negedge clk); waited = waited + 1; end
            if (!drain_done) begin
                $display("ERR 排空没有完成（等了 %0d 拍）", waited);
                errors = errors + 1;
            end else if (rd_out != 0 || wr_out != 0) begin
                $display("ERR 排空完成时总线上还有断掉的突发 rd=%0d wr=%0d", rd_out, wr_out);
                errors = errors + 1;
            end
            @(negedge clk);
            drain_req = 0;
            @(negedge clk);
        end
    endtask

    // =====================================================================
    // 用例
    // =====================================================================
    integer i, guard, base, waited, done_n;

    task case_normal;
        integer stat0;
        begin
            // 1) 读侧仲裁：两个请求方同时举手，命令取数必须先上总线
            ar_order_fd = 0; ar_order_pix = 0; seq = 0;
            exp_pix_buf = 2'd1; exp_pix_beats = 4; exp_pix_idx = PB + 4; exp_pix_en = 1; got_pix_beats = 0;
            exp_cmd_cidx = 4'd3; exp_cmd_beats = 2; exp_cmd_idx = CB; exp_cmd_en = 1; got_cmd_beats = 0;
            exp_rd_st_cidx = -1;
            @(negedge clk);
            base = fd_fire_n;
            rd_addr = (PB+4) << 4; rd_bytes = 4*16; rd_buf = 2'd1; rd_cidx = 4'd1; rd_valid = 1;
            fd_addr = CB << 4;     fd_bytes = 2*16; fd_cidx = 4'd3; fd_valid = 1;
            @(negedge clk);
            guard = 0;
            while (fd_fire_n == base && guard < 500) begin @(negedge clk); guard = guard + 1; end
            if (fd_fire_n == base) begin
                $display("ERR 命令取数一直没有被接受"); errors = errors + 1;
            end
            fd_valid = 0;
            guard = 0;
            while (rd_fire_n == 0 && guard < 5000) begin @(negedge clk); guard = guard + 1; end
            if (rd_fire_n == 0) begin
                $display("ERR 像素读一直没有被接受"); errors = errors + 1;
            end
            rd_valid = 0;
            while (exp_pix_en || exp_cmd_en) @(negedge clk);
            @(negedge clk);
            if (ar_order_fd == 0 || (ar_order_pix != 0 && ar_order_fd > ar_order_pix)) begin
                $display("ERR 仲裁顺序：命令取数没有优先（fd=%0d pix=%0d）", ar_order_fd, ar_order_pix);
                errors = errors + 1;
            end

            // 2) 若干像素读：缓冲号与命令索引轮换，读流上下文要跟着对
            pix_read(PB + 0,  4, 2'd0, 4'd0);
            pix_read(PB + 8,  8, 2'd1, 4'd5);
            pix_read(PB + 16, 12, 2'd2, 4'd9);
            pix_read(PB + 0,  1, 2'd3, 4'd15);
            // 3) 命令取数
            cmd_read(CB + 0, 1, 4'd1);
            cmd_read(CB + 4, 6, 4'd2);

            // 4) 三笔写：带内掩码、命令索引与末行标记
            do_write(WB + 0,  4, 1, 1, 4); wait_wr_status; @(negedge clk); wr_src_en = 0;
            verify_write(WB + 0, 4, 1, 4);
            do_write(WB + 8,  7, 2, 0, 7); wait_wr_status; @(negedge clk); wr_src_en = 0;
            verify_write(WB + 8, 7, 2, 7);
            do_write(WB + 24, 16, 3, 1, 16); wait_wr_status; @(negedge clk); wr_src_en = 0;
            verify_write(WB + 24, 16, 3, 16);

            // 5) 写在途门控：上一笔的状态没回来之前，第二个写描述符不得被接受。
            //    这一条不能靠「发一笔等一笔」的写法验，那样门控根本不会被碰到。
            @(negedge clk);
            wsrc_tag = 6; wsrc_total = 4; wsrc_stop = 4; wsrc_i = 0; wr_src_en = 1;
            exp_wr_st_cidx = 6; exp_wr_st_last = 0;
            base = wr_fire_n;
            wr_addr = (WB+40) << 4; wr_bytes = 4*16; wr_cidx = 4'd6; wr_last_row = 1'b0; wr_valid = 1;
            @(negedge clk);
            while (wr_fire_n == base) @(negedge clk);
            wr_valid = 0;
            // 第二笔立刻举手，只举 5 拍：第一笔从描述符到状态要一百多拍，
            // 这 5 拍里门控必须把它挡住。举久了会和门控放开那一拍抢同一个边沿。
            @(negedge clk);
            stat0 = got_wr_st;
            base = wr_fire_n;
            wr_addr = (WB+48) << 4; wr_bytes = 4*16; wr_cidx = 4'd7; wr_last_row = 1'b0; wr_valid = 1;
            repeat (5) @(negedge clk);
            if (wr_fire_n != base) begin
                $display("ERR 上一笔写还没结束就接受了下一个写描述符");
                errors = errors + 1;
            end
            wr_valid = 0;
            guard = 0;
            while (got_wr_st == stat0 && guard < 20000) begin @(negedge clk); guard = guard + 1; end
            if (got_wr_st == stat0) begin
                $display("ERR 写状态一直没回来"); errors = errors + 1;
            end
            @(negedge clk);
            wr_src_en = 0;

            // 6) 判据汇总
            if (rd_viol != 0 || wr_viol != 0) begin
                $display("ERR 在途契约违例 rd=%0d wr=%0d", rd_viol, wr_viol); errors = errors + 1;
            end
            if (rd_out != 0 || wr_out != 0) begin
                $display("ERR 收尾时仍有在途事务 rd=%0d wr=%0d", rd_out, wr_out); errors = errors + 1;
            end
            if (unaligned != 0) begin
                $display("ERR 有 %0d 笔地址不按 16 字节对齐", unaligned); errors = errors + 1;
            end
            if (cross4k != 0) begin
                $display("ERR 有 %0d 笔跨 4KB", cross4k); errors = errors + 1;
            end
            if (multi_burst != 0) begin
                $display("ERR 有 %0d 个描述符不是恰好一笔突发", multi_burst); errors = errors + 1;
            end
            if (rready_bubble != 0) begin
                $display("ERR 读数据阶段 rready 拉低了 %0d 拍", rready_bubble); errors = errors + 1;
            end
            if (tmo_pulses != 0) begin
                $display("ERR 正常用例里置了超时位 %0d 次", tmo_pulses); errors = errors + 1;
            end
            if (rd_stat_err || wr_stat_err) begin
                $display("ERR 不该报总线错误 rd=%0d wr=%0d", rd_stat_err, wr_stat_err); errors = errors + 1;
            end
        end
    endtask

    task case_drain;
        begin
            // (a) 读突发中途排空，消费者撤 ready —— 模拟控制层复位
            exp_pix_en = 0; exp_rd_st_cidx = -1;
            @(negedge clk);
            base = rd_fire_n;
            rd_addr = (PB+0) << 4; rd_bytes = 32*16; rd_buf = 2'd1; rd_cidx = 4'd6; rd_valid = 1;
            @(negedge clk);
            while (rd_fire_n == base) @(negedge clk);
            rd_valid = 0;
            repeat (3) @(negedge clk);
            rs_ready = 0; fs_ready = 0;          // 消费者挂在控制层复位下
            do_drain(waited);
            rs_ready = 1; fs_ready = 1;
            if (tmo_pulses != 0) begin
                $display("ERR 排空用例里置了超时位 %0d 次", tmo_pulses); errors = errors + 1;
            end
            // 排空后必须能再提交
            pix_read(PB + 4, 4, 2'd3, 4'd7);

            // (b) 写突发中途排空，写源停供 —— 同样是控制层复位
            wr_src_en = 0;
            do_write(WB + 32, 32, 1, 1, 8);
            while (wsrc_i < 8) @(negedge clk);
            @(negedge clk);
            do_drain(waited);
            wr_src_en = 0;
            verify_write(WB + 32, 32, 1, 8);
            do_write(WB + 96, 4, 3, 1, 4); wait_wr_status; @(negedge clk); wr_src_en = 0;
            verify_write(WB + 96, 4, 3, 4);

            // (c) 软复位那一组：消费者维持 ready，读方向靠搬运层自己把数据吃掉
            if (INJECT == 0) begin
                exp_pix_en = 0; exp_rd_st_cidx = -1;
                @(negedge clk);
                base = rd_fire_n;
                rd_addr = (PB+0) << 4; rd_bytes = 32*16; rd_buf = 2'd2; rd_cidx = 4'd8; rd_valid = 1;
                @(negedge clk);
                while (rd_fire_n == base) @(negedge clk);
                rd_valid = 0;
                repeat (3) @(negedge clk);
                do_drain(waited);
                pix_read(PB + 8, 4, 2'd0, 4'd9);
            end

            // (d) 请求一直举着：控制层复位持续很久时，drain_req 可能就是复位电平本身。
            //     一次请求只完成一次：drain_done 只打一拍；请求撤下之前不得接受新提交
            //     （像素读描述符全程举着），撤下之后照常收。
            //     do_drain 看到 drain_done 半拍后就撤请求，碰不到这一条。
            exp_pix_en = 0; exp_rd_st_cidx = -1;
            @(negedge clk);
            drain_req = 1;
            base = rd_fire_n;
            rd_addr = (PB+12) << 4; rd_bytes = 4*16; rd_buf = 2'd0; rd_cidx = 4'd10; rd_valid = 1;
            done_n = 0;
            repeat (40) begin
                @(negedge clk);
                if (drain_done) done_n = done_n + 1;
            end
            if (done_n != 1) begin
                $display("ERR 请求一直举着时 drain_done 打了 %0d 次，应为 1 次", done_n); errors = errors + 1;
            end
            if (rd_fire_n != base) begin
                $display("ERR 排空请求还举着就接受了新的读描述符"); errors = errors + 1;
            end
            drain_req = 0;
            guard = 0;
            while (rd_fire_n == base && guard < 200) begin @(negedge clk); guard = guard + 1; end
            rd_valid = 0;
            if (rd_fire_n == base) begin
                $display("ERR 排空请求撤下之后读描述符一直没被接受"); errors = errors + 1;
            end
            // 等这一笔走完（数据不比对），免得它拖进用例结尾的统计
            guard = 0;
            while (dut.rd_busy && guard < 20000) begin @(negedge clk); guard = guard + 1; end
            repeat (4) @(negedge clk);

            if (tmo_pulses != 0) begin
                $display("ERR 排空用例里置了超时位 %0d 次", tmo_pulses); errors = errors + 1;
            end
        end
    endtask

    task case_err;
        begin
            // 读回 SLVERR：命令取数区与像素读区各放一段越过区间的地址
            exp_pix_en = 0; exp_cmd_en = 0;
            exp_rd_st_cidx = 4'd4; exp_rd_st_buf = 2'd2;
            @(negedge clk);
            base = rd_fire_n;
            rd_addr = 32'h0000_9000; rd_bytes = 4*16; rd_buf = 2'd2; rd_cidx = 4'd4; rd_valid = 1;
            @(negedge clk);
            while (rd_fire_n == base) @(negedge clk);
            rd_valid = 0;
            guard = 0;
            while (!rd_stat_valid && guard < 20000) begin @(negedge clk); guard = guard + 1; end
            // 状态是一拍脉冲，先判再走下一拍，不然读到的已经是低电平
            if (!rd_stat_valid) begin
                $display("ERR 读状态没有回来"); errors = errors + 1;
            end else if (!rd_stat_err) begin
                $display("ERR 读回 SLVERR 但状态里没有报错"); errors = errors + 1;
            end
            @(negedge clk);

            // 写响应 SLVERR（一拍的一笔，写源按正常通路供一拍）
            do_write(32'h0000_0900, 1, 11, 1, 1);
            guard = 0;
            while (!wr_stat_valid && guard < 20000) begin @(negedge clk); guard = guard + 1; end
            if (!wr_stat_valid) begin
                $display("ERR 写状态没有回来"); errors = errors + 1;
            end else if (!wr_stat_err) begin
                $display("ERR 写响应 SLVERR 但状态里没有报错"); errors = errors + 1;
            end
            @(negedge clk);
            wr_src_en = 0;
        end
    endtask

    task case_timeout;
        begin
            // 门限取小值（run.sh 传 TMO_LIMIT_VAL=64），从机按住
            bus_stall = 1;
            exp_pix_en = 0;
            @(negedge clk);
            base = rd_fire_n;
            rd_addr = (PB+0) << 4; rd_bytes = 4*16; rd_buf = 2'd0; rd_cidx = 4'd2; rd_valid = 1;
            @(negedge clk);
            while (rd_fire_n == base) @(negedge clk);
            rd_valid = 0;
            guard = 0;
            while (tmo_pulses == 0 && guard < 20000) begin @(negedge clk); guard = guard + 1; end
            if (tmo_pulses == 0) begin
                $display("ERR 超过门限没有报超时（等了 %0d 拍）", guard); errors = errors + 1;
            end else if (tmo_dir != 1'b0) begin
                $display("ERR 超时方向报成了写"); errors = errors + 1;
            end
            // 只报告不放弃：放开之后必须照常完成，数据属于原来那一笔
            bus_stall = 0;
            exp_pix_buf = 2'd0; exp_pix_beats = 4; exp_pix_idx = PB; exp_pix_en = 1; got_pix_beats = 0;
            guard = 0;
            while (exp_pix_en && guard < 20000) begin @(negedge clk); guard = guard + 1; end
            if (exp_pix_en) begin
                $display("ERR 放开之后这一笔没有完成"); errors = errors + 1;
            end
            if (rd_out != 0) begin
                $display("ERR 超时用例收尾仍有在途读 %0d", rd_out); errors = errors + 1;
            end
            if (tmo_pulses != 1) begin
                $display("ERR 超时脉冲发了 %0d 次，应当只报一次", tmo_pulses); errors = errors + 1;
            end
        end
    endtask

    // =====================================================================
    // 调试跟踪
    // =====================================================================
`ifdef TRACE
    always @(posedge clk) if (!rst && (m_arvalid || m_arready || s_arvalid || m_rvalid || m_rready)) begin
        $display("T=%0t arv=%b arr=%b s_arv=%b s_arr=%b rv=%b rr=%b rl=%b stall=%b",
                 $time, m_arvalid, m_arready, s_arvalid, s_arready,
                 m_rvalid, m_rready, m_rlast, bus_stall);
    end
`endif

`ifdef DBG
    always @(posedge clk) if (!rst && (dut.wr_status_valid || wr_stat_valid || dut.wr_fire || (m_bvalid && m_bready)))
        $display("DBG T=%0t wr_fire=%b core_st_v=%b core_err=%b | st_v=%b err=%b cidx=%0d bresp=%b bvalid=%b",
                 $time, dut.wr_fire, dut.wr_status_valid, dut.wr_status_err,
                 wr_stat_valid, wr_stat_err, wr_stat_cidx, m_bresp, m_bvalid);
`endif

    // =====================================================================
    initial begin
        #1;
        for (i = 0; i < 4096; i = i + 1) u_slave.mem[i] = pat(i);
        rst = 1;
        repeat (4) @(posedge clk);
        rst = 0;
        repeat (2) @(negedge clk);

`ifdef CASE_NORMAL
        case_normal;
`endif
`ifdef CASE_DRAIN
        case_drain;
`endif
`ifdef CASE_ERR
        case_err;
`endif
`ifdef CASE_TIMEOUT
        case_timeout;
`endif

        repeat (6) @(negedge clk);
        $display("STAT rd_viol=%0d wr_viol=%0d rd_out=%0d wr_out=%0d ar=%0d aw=%0d multi=%0d unal=%0d x4k=%0d rrb=%0d tmo=%0d",
                 rd_viol, wr_viol, rd_out, wr_out, ar_seen, aw_seen, multi_burst, unaligned,
                 cross4k, rready_bubble, tmo_pulses);
        if (u_slave.wlast_err != 0) begin
            $display("ERR 从机报 WLAST 与 AWLEN 对不上 %0d 次", u_slave.wlast_err);
            errors = errors + 1;
        end
        if (errors == 0) $display("RESULT: PASS");
        else             $display("RESULT: FAIL (%0d errors)", errors);
        $finish;
    end

    // 看门狗：判据必须自己收尾，挂住要看得出来
    initial begin
        #4000000;
        $display("STAT rd_viol=%0d wr_viol=%0d rd_out=%0d wr_out=%0d ar=%0d aw=%0d multi=%0d unal=%0d x4k=%0d rrb=%0d tmo=%0d",
                 rd_viol, wr_viol, rd_out, wr_out, ar_seen, aw_seen, multi_burst, unaligned,
                 cross4k, rready_bubble, tmo_pulses);
        $display("RESULT: FAIL (看门狗：仿真没有自己结束)");
        $finish;
    end

endmodule

`default_nettype wire
