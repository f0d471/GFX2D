// 构型 elaboration 探针：只验证该构型的 engine_top 能否在当前源树上通过 elaboration。
// 由 check_features.py 生成到构型自己的 build 目录，不参与生产。
`timescale 1ns/1ps
module tb_feature_elab;
    reg clk = 1'b0;
    always #5 clk = ~clk;
    reg rst_ctrl = 1'b1;
    reg rst_ddr_n = 1'b0;
    reg [15:0] paddr = 16'h0;
    reg psel = 1'b0, penable = 1'b0, pwrite = 1'b0;
    reg [31:0] pwdata = 32'h0;
    wire [31:0] prdata;
    wire pready, pslverr;
    wire [31:0] m_axi_araddr; wire [7:0] m_axi_arlen; wire [2:0] m_axi_arsize;
    wire [1:0] m_axi_arburst; wire m_axi_arvalid; reg m_axi_arready = 1'b1;
    reg [127:0] m_axi_rdata = 128'h0; reg [1:0] m_axi_rresp = 2'b00;
    reg m_axi_rlast = 1'b0, m_axi_rvalid = 1'b0; wire m_axi_rready;
    wire [31:0] m_axi_awaddr; wire [7:0] m_axi_awlen; wire [2:0] m_axi_awsize;
    wire [1:0] m_axi_awburst; wire m_axi_awvalid; reg m_axi_awready = 1'b1;
    wire [127:0] m_axi_wdata; wire [15:0] m_axi_wstrb; wire m_axi_wlast, m_axi_wvalid;
    reg m_axi_wready = 1'b1; reg [1:0] m_axi_bresp = 2'b00; reg m_axi_bvalid = 1'b0;
    wire m_axi_bready; wire irq_b, dbg_fe_cidx_err;

    engine_top u_engine (
        .clk(clk), .rst_ctrl(rst_ctrl), .rst_ddr_n(rst_ddr_n),
        .paddr(paddr), .psel(psel), .penable(penable), .pwrite(pwrite),
        .pwdata(pwdata), .prdata(prdata), .pready(pready), .pslverr(pslverr),
        .m_axi_araddr(m_axi_araddr), .m_axi_arlen(m_axi_arlen), .m_axi_arsize(m_axi_arsize),
        .m_axi_arburst(m_axi_arburst), .m_axi_arvalid(m_axi_arvalid), .m_axi_arready(m_axi_arready),
        .m_axi_rdata(m_axi_rdata), .m_axi_rresp(m_axi_rresp), .m_axi_rlast(m_axi_rlast),
        .m_axi_rvalid(m_axi_rvalid), .m_axi_rready(m_axi_rready),
        .m_axi_awaddr(m_axi_awaddr), .m_axi_awlen(m_axi_awlen), .m_axi_awsize(m_axi_awsize),
        .m_axi_awburst(m_axi_awburst), .m_axi_awvalid(m_axi_awvalid), .m_axi_awready(m_axi_awready),
        .m_axi_wdata(m_axi_wdata), .m_axi_wstrb(m_axi_wstrb), .m_axi_wlast(m_axi_wlast),
        .m_axi_wvalid(m_axi_wvalid), .m_axi_wready(m_axi_wready),
        .m_axi_bresp(m_axi_bresp), .m_axi_bvalid(m_axi_bvalid), .m_axi_bready(m_axi_bready),
        .irq_b(irq_b), .dbg_fe_cidx_err(dbg_fe_cidx_err));

    initial begin
        $display("FEATURE-ELAB-PASS");
        $finish;
    end
endmodule
