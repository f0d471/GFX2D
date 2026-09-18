// axi_probe_master + Axi_Mux 测试台
//
// 两个 axi_probe_master 分别接 Axi_Mux 的 S0、S1，出口接随机反压的从机模型。
// S0 在板上是 SoC，这里用第二个探针主机代替，目的是让两个主机真正并发。
//
// 流程，对 LEN = 0,1,3,...,255 各做一轮：
//   1. m0、m1 同时写各自的区域（m0 在 0x0000 起，m1 在 0x8000 起），各 256 拍
//   2. m0、m1 同时读比对各自的区域
//   3. m1 用 m0 的种子读 m0 的区域：必须无失配（第二个观测者看到第一个主机写的数据）
//   4. m1 用错误的种子读 m0 的区域：必须有失配（比对判据能分辨）
// 每一步检查 DONE、无超时、无错误响应、rlast 位置正确、事务数正确。
// 最后读一拍检查 RDATA0。
`timescale 1ns/1ps
`default_nettype none

module tb_axi_probe;

reg clk = 1'b0;
always #5 clk = ~clk;

reg rst = 1'b1;

// ---------------------------------------------------------------------------
// APB，两个主机共用信号，psel 分开
// ---------------------------------------------------------------------------
reg  [11:0] paddr;
reg         psel0, psel1, penable, pwrite;
reg  [31:0] pwdata;
wire [31:0] prdata0, prdata1;
wire        pslverr0, pslverr1;
wire        pready0, pready1;

// ---------------------------------------------------------------------------
// AXI：两个主机侧与从机侧
// ---------------------------------------------------------------------------
wire [31:0]  m_awaddr  [0:1];
wire [7:0]   m_awlen   [0:1];
wire [2:0]   m_awsize  [0:1];
wire [1:0]   m_awburst [0:1];
wire         m_awvalid [0:1];
wire         m_awready [0:1];
wire [127:0] m_wdata   [0:1];
wire [15:0]  m_wstrb   [0:1];
wire         m_wlast   [0:1];
wire         m_wvalid  [0:1];
wire         m_wready  [0:1];
wire         m_bvalid  [0:1];
wire         m_bready  [0:1];
wire [31:0]  m_araddr  [0:1];
wire [7:0]   m_arlen   [0:1];
wire [2:0]   m_arsize  [0:1];
wire [1:0]   m_arburst [0:1];
wire         m_arvalid [0:1];
wire         m_arready [0:1];
wire [127:0] m_rdata   [0:1];
wire [1:0]   m_rresp   [0:1];
wire         m_rlast   [0:1];
wire         m_rvalid  [0:1];
wire         m_rready  [0:1];

wire         arb_type;
wire [2:0]   arb_mask;
wire         unused_type;
wire [1:0]   unused_mask;

wire [31:0]  s_awaddr, s_araddr;
wire [7:0]   s_awlen, s_arlen;
wire [2:0]   s_awsize, s_arsize;
wire [1:0]   s_awburst, s_arburst;
wire [1:0]   s_awlock, s_arlock;
wire [7:0]   s_awid, s_arid, s_wid, s_bid, s_rid;
wire         s_awvalid, s_awready, s_wvalid, s_wready, s_wlast;
wire [127:0] s_wdata, s_rdata;
wire [15:0]  s_wstrb;
wire [1:0]   s_bresp, s_rresp;
wire         s_bvalid, s_bready, s_arvalid, s_arready, s_rvalid, s_rready, s_rlast;

// 顶层的做法：Axi_Mux 没有 BRESP 端口，控制器的 bresp 直接并接给两个主机
wire [1:0]   m_bresp = s_bresp;

genvar g;
generate
for (g = 0; g < 2; g = g + 1) begin : gm
    axi_probe_master u_m (
        .clk(clk), .rst(rst),
        .paddr(paddr), .psel(g == 0 ? psel0 : psel1), .penable(penable), .pwrite(pwrite),
        .pwdata(pwdata), .prdata(), .pready(), .pslverr(),
        .arb_type(), .arb_mask(),
        .awaddr(m_awaddr[g]), .awlen(m_awlen[g]), .awsize(m_awsize[g]), .awburst(m_awburst[g]),
        .awvalid(m_awvalid[g]), .awready(m_awready[g]),
        .wdata(m_wdata[g]), .wstrb(m_wstrb[g]), .wlast(m_wlast[g]), .wvalid(m_wvalid[g]), .wready(m_wready[g]),
        .bresp(m_bresp), .bvalid(m_bvalid[g]), .bready(m_bready[g]),
        .araddr(m_araddr[g]), .arlen(m_arlen[g]), .arsize(m_arsize[g]), .arburst(m_arburst[g]),
        .arvalid(m_arvalid[g]), .arready(m_arready[g]),
        .rdata(m_rdata[g]), .rresp(m_rresp[g]), .rlast(m_rlast[g]), .rvalid(m_rvalid[g]), .rready(m_rready[g])
    );
end
endgenerate

// generate 块里的输出不能直接接数组元素以外的名字，APB 读数据与仲裁配置单独引出
assign prdata0  = gm[0].u_m.prdata;
assign prdata1  = gm[1].u_m.prdata;
assign pslverr0 = gm[0].u_m.pslverr;
assign pslverr1 = gm[1].u_m.pslverr;
assign arb_type = gm[1].u_m.arb_type;
// Axi_Mux 改成 X3 之后 Cfg_Cnl_Mask 是 3 位（E5 2.1 节）；探针的 ARB 只有 2 位，
// 最高位补 1，保持「三个通道都开」的语义
assign arb_mask = {1'b1, gm[1].u_m.arb_mask};

Axi_Mux #(.SLAVE_ID_WIDTH(8), .ID_MODE_SEL("Normal"), .AXI_DATA_WIDTH(128)) u_mux (
    .Sys_Clk(clk), .Sys_Rst_N(~rst),

    .O_M_AW_ID(s_awid), .O_M_AW_ADDR(s_awaddr), .O_M_AW_LEN(s_awlen), .O_M_AW_SIZE(s_awsize),
    .O_M_AW_BURST(s_awburst), .O_M_AW_LOCK(s_awlock), .O_M_AW_VALID(s_awvalid), .I_M_AW_READY(s_awready),
    .O_M_W_ID(s_wid), .O_M_W_DATA(s_wdata), .O_M_W_LAST(s_wlast), .O_M_W_STRB(s_wstrb),
    .O_M_W_VALID(s_wvalid), .I_M_W_READY(s_wready),
    .I_M_B_ID(8'h0), .I_M_B_VALID(s_bvalid), .O_M_B_READY(s_bready),
    .O_M_AR_ID(s_arid), .O_M_AR_ADDR(s_araddr), .O_M_AR_LEN(s_arlen), .O_M_AR_SIZE(s_arsize),
    .O_M_AR_BURST(s_arburst), .O_M_AR_LOCK(s_arlock), .O_M_AR_VALID(s_arvalid), .I_M_AR_READY(s_arready),
    .I_M_R_ID(8'h0), .I_M_R_DATA(s_rdata), .I_M_R_LAST(s_rlast), .I_M_R_RESP(s_rresp),
    .I_M_R_VALID(s_rvalid), .O_M_R_READY(s_rready),

    .I_S0_AW_ID(8'h0), .I_S0_AW_ADDR(m_awaddr[0]), .I_S0_AW_LEN(m_awlen[0]), .I_S0_AW_SIZE(m_awsize[0]),
    .I_S0_AW_BURST(m_awburst[0]), .I_S0_AW_LOCK(2'b00), .I_S0_AW_VALID(m_awvalid[0]), .O_S0_AW_READY(m_awready[0]),
    .I_S0_W_ID(8'h0), .I_S0_W_DATA(m_wdata[0]), .I_S0_W_LAST(m_wlast[0]), .I_S0_W_STRB(m_wstrb[0]),
    .I_S0_W_VALID(m_wvalid[0]), .O_S0_W_READY(m_wready[0]),
    .O_S0_B_ID(), .O_S0_B_VALID(m_bvalid[0]), .I_S0_B_READY(m_bready[0]),
    .I_S0_AR_ID(8'h0), .I_S0_AR_ADDR(m_araddr[0]), .I_S0_AR_LEN(m_arlen[0]), .I_S0_AR_SIZE(m_arsize[0]),
    .I_S0_AR_BURST(m_arburst[0]), .I_S0_AR_LOCK(2'b00), .I_S0_AR_VALID(m_arvalid[0]), .O_S0_AR_READY(m_arready[0]),
    .O_S0_R_ID(), .O_S0_R_DATA(m_rdata[0]), .O_S0_R_LAST(m_rlast[0]), .O_S0_R_RESP(m_rresp[0]),
    .O_S0_R_VALID(m_rvalid[0]), .I_S0_R_READY(m_rready[0]),

    .I_S1_AW_ID(8'h0), .I_S1_AW_ADDR(m_awaddr[1]), .I_S1_AW_LEN(m_awlen[1]), .I_S1_AW_SIZE(m_awsize[1]),
    .I_S1_AW_BURST(m_awburst[1]), .I_S1_AW_LOCK(2'b00), .I_S1_AW_VALID(m_awvalid[1]), .O_S1_AW_READY(m_awready[1]),
    .I_S1_W_ID(8'h0), .I_S1_W_DATA(m_wdata[1]), .I_S1_W_LAST(m_wlast[1]), .I_S1_W_STRB(m_wstrb[1]),
    .I_S1_W_VALID(m_wvalid[1]), .O_S1_W_READY(m_wready[1]),
    .O_S1_B_ID(), .O_S1_B_VALID(m_bvalid[1]), .I_S1_B_READY(m_bready[1]),
    .I_S1_AR_ID(8'h0), .I_S1_AR_ADDR(m_araddr[1]), .I_S1_AR_LEN(m_arlen[1]), .I_S1_AR_SIZE(m_arsize[1]),
    .I_S1_AR_BURST(m_arburst[1]), .I_S1_AR_LOCK(2'b00), .I_S1_AR_VALID(m_arvalid[1]), .O_S1_AR_READY(m_arready[1]),
    .O_S1_R_ID(), .O_S1_R_DATA(m_rdata[1]), .O_S1_R_LAST(m_rlast[1]), .O_S1_R_RESP(m_rresp[1]),
    .O_S1_R_VALID(m_rvalid[1]), .I_S1_R_READY(m_rready[1]),

    // Axi_Mux 改成 X3（E5 2.1 节）之后多出 S2 一组端口；本测试台只用两个主机，
    // 把 S2 明确接成不发起，避免输入悬空
    .I_S2_AW_ID(8'h0), .I_S2_AW_ADDR(32'h0), .I_S2_AW_LEN(8'h0), .I_S2_AW_SIZE(3'b000),
    .I_S2_AW_BURST(2'b00), .I_S2_AW_LOCK(2'b00), .I_S2_AW_VALID(1'b0), .O_S2_AW_READY(),
    .I_S2_W_ID(8'h0), .I_S2_W_DATA(128'h0), .I_S2_W_LAST(1'b0), .I_S2_W_STRB(16'h0),
    .I_S2_W_VALID(1'b0), .O_S2_W_READY(),
    .O_S2_B_ID(), .O_S2_B_VALID(), .I_S2_B_READY(1'b0),
    .I_S2_AR_ID(8'h0), .I_S2_AR_ADDR(32'h0), .I_S2_AR_LEN(8'h0), .I_S2_AR_SIZE(3'b000),
    .I_S2_AR_BURST(2'b00), .I_S2_AR_LOCK(2'b00), .I_S2_AR_VALID(1'b0), .O_S2_AR_READY(),
    .O_S2_R_ID(), .O_S2_R_DATA(), .O_S2_R_LAST(), .O_S2_R_RESP(), .O_S2_R_VALID(),
    .I_S2_R_READY(1'b0),

    .Cfg_Arb_Type(arb_type), .Cfg_Cnl_Mask(arb_mask),
    .O_AW_Cnl_Num(), .O_W_Cnl_Num(), .O_B_Cnl_Num(), .O_AR_Cnl_Num(), .O_R_Cnl_Num()
);

axi_ram_model #(
`ifdef INJECT_MAXLEN
    .MAX_LEN(15),
`else
    .MAX_LEN(255),
`endif
    .SEED(7), .STALL(3)
) u_ram (
    .clk(clk), .rst_n(~rst),
    .awaddr(s_awaddr), .awlen(s_awlen), .awvalid(s_awvalid), .awready(s_awready),
    .wdata(s_wdata), .wlast(s_wlast), .wvalid(s_wvalid), .wready(s_wready),
    .bresp(s_bresp), .bvalid(s_bvalid), .bready(s_bready),
    .araddr(s_araddr), .arlen(s_arlen), .arvalid(s_arvalid), .arready(s_arready),
    .rdata(s_rdata), .rresp(s_rresp), .rlast(s_rlast), .rvalid(s_rvalid), .rready(s_rready),
    // E5 加的地址通道持续反压控制，本测试台不用
    .hold_aw(1'b0), .hold_ar(1'b0)
);

// ---------------------------------------------------------------------------
// APB 任务
// ---------------------------------------------------------------------------
task apb_wr(input integer who, input [11:0] a, input [31:0] d);
    begin
        @(posedge clk);
        paddr <= a; pwdata <= d; pwrite <= 1'b1;
        psel0 <= (who == 0); psel1 <= (who == 1); penable <= 1'b0;
        @(posedge clk);
        penable <= 1'b1;
        @(posedge clk);
        psel0 <= 1'b0; psel1 <= 1'b0; penable <= 1'b0; pwrite <= 1'b0;
    end
endtask

task apb_rd(input integer who, input [11:0] a, output [31:0] d);
    begin
        @(posedge clk);
        paddr <= a; pwrite <= 1'b0;
        psel0 <= (who == 0); psel1 <= (who == 1); penable <= 1'b0;
        @(posedge clk);
        penable <= 1'b1;
        #1 d = (who == 0) ? prdata0 : prdata1;
        @(posedge clk);
        psel0 <= 1'b0; psel1 <= 1'b0; penable <= 1'b0;
    end
endtask

integer errors = 0;

task config_run(input integer who, input [31:0] addr, input [7:0] len, input [31:0] count,
                input [2:0] mode, input [31:0] seed);
    begin
        apb_wr(who, 12'h008, addr);
        apb_wr(who, 12'h00C, {24'h0, len});
        apb_wr(who, 12'h010, count);
        apb_wr(who, 12'h014, {29'h0, mode});
        apb_wr(who, 12'h018, seed);
`ifdef INJECT_TIMEOUT
        apb_wr(who, 12'h01C, 32'd1);
`else
        apb_wr(who, 12'h01C, 32'd2000);
`endif
    end
endtask

task wait_done(input integer who);
    reg [31:0] st;
    integer n;
    begin
        st = 0;
        n  = 0;
        while (!st[1] && n < 200000) begin
            apb_rd(who, 12'h004, st);
            n = n + 1;
        end
        if (!st[1]) begin
            $display("ERROR: master %0d never finished", who);
            errors = errors + 1;
        end
    end
endtask

// 检查一轮结果；want_mismatch = 1 时要求失配数大于 0
task check_run(input integer who, input [31:0] count, input want_mismatch, input [8*24-1:0] tag);
    reg [31:0] st, txd, cyc, lat, errs;
    begin
        apb_rd(who, 12'h004, st);
        apb_rd(who, 12'h020, txd);
        apb_rd(who, 12'h024, cyc);
        apb_rd(who, 12'h028, lat);
        apb_rd(who, 12'h02C, errs);
        if (st[2]) begin $display("ERROR: %0s m%0d timeout", tag, who); errors = errors + 1; end
        if (st[3]) begin $display("ERROR: %0s m%0d resp_err count %0d", tag, who, errs[15:0]); errors = errors + 1; end
        if (st[4]) begin $display("ERROR: %0s m%0d rlast position", tag, who); errors = errors + 1; end
        if (txd != count) begin $display("ERROR: %0s m%0d tx_done %0d want %0d", tag, who, txd, count); errors = errors + 1; end
        if (want_mismatch && errs[31:16] == 0) begin
            $display("ERROR: %0s m%0d expected mismatches, got none", tag, who); errors = errors + 1;
        end
        if (!want_mismatch && errs[31:16] != 0) begin
            $display("ERROR: %0s m%0d mismatches %0d", tag, who, errs[31:16]); errors = errors + 1;
        end
        $display("%0s m%0d tx=%0d cycles=%0d lat_sum=%0d mismatch=%0d", tag, who, txd, cyc, lat, errs[31:16]);
    end
endtask

localparam [31:0] SEED0 = 32'h1234_0000;
localparam [31:0] SEED1 = 32'h9876_0000;

integer k;
reg [7:0]  len;
reg [31:0] cnt, d;

initial begin
    paddr = 0; psel0 = 0; psel1 = 0; penable = 0; pwrite = 0; pwdata = 0;
    repeat (5) @(posedge clk);
    rst = 1'b0;

    for (k = 0; k < 9; k = k + 1) begin
        len = (8'd1 << k) - 8'd1;
        if (k == 8) len = 8'd255;
        cnt = 32'd256 >> k;

        config_run(0, 32'h0000_0000, len, cnt, 3'b011, SEED0 + k);
        config_run(1, 32'h0000_8000, len, cnt, 3'b011, SEED1 + k);
        apb_wr(0, 12'h000, 32'h1);
        apb_wr(1, 12'h000, 32'h1);
        wait_done(0); wait_done(1);
        check_run(0, cnt, 1'b0, "write");
        check_run(1, cnt, 1'b0, "write");

        config_run(0, 32'h0000_0000, len, cnt, 3'b110, SEED0 + k);
        config_run(1, 32'h0000_8000, len, cnt, 3'b110, SEED1 + k);
        apb_wr(0, 12'h000, 32'h1);
        apb_wr(1, 12'h000, 32'h1);
        wait_done(0); wait_done(1);
        check_run(0, cnt, 1'b0, "read");
        check_run(1, cnt, 1'b0, "read");

        config_run(1, 32'h0000_0000, len, cnt, 3'b110, SEED0 + k);
        apb_wr(1, 12'h000, 32'h1);
        wait_done(1);
        check_run(1, cnt, 1'b0, "cross");

        config_run(1, 32'h0000_0000, len, cnt, 3'b110, SEED1 + k);
        apb_wr(1, 12'h000, 32'h1);
        wait_done(1);
        check_run(1, cnt, 1'b1, "wrongseed");

        $display("len=%0d done, errors so far %0d", len, errors);
    end

    // RDATA0：最后一轮 m0 写的是 SEED0+8，地址 0x10 那一拍的低 32 位应为 SEED0+8+0x10
    config_run(1, 32'h0000_0010, 8'd0, 32'd1, 3'b000, 32'h0);
    apb_wr(1, 12'h000, 32'h1);
    wait_done(1);
    apb_rd(1, 12'h030, d);
    if (d != SEED0 + 32'd8 + 32'h10) begin
        $display("ERROR: rdata0 %h want %h", d, SEED0 + 32'd8 + 32'h10);
        errors = errors + 1;
    end
    apb_rd(1, 12'h03C, d);
    if (d != 32'h4158_4950) begin
        $display("ERROR: id %h", d);
        errors = errors + 1;
    end

    if (errors == 0) $display("RESULT: PASS");
    else             $display("RESULT: FAIL (%0d errors)", errors);
    $finish;
end

// 总超时，防止注错用例挂死
initial begin
    #200_000_000;
    $display("RESULT: FAIL (global timeout)");
    $finish;
end

endmodule

`default_nettype wire
