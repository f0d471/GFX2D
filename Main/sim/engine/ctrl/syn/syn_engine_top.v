// B5 engine_top 单独综合封装（B3/B4 同一套做法）：全部输入由 LFSR 驱动（不接常数），
// 全部输出折进观测寄存器 obs，RAM/逻辑不会被剪。约束 100 MHz（core_clk）。
// APB 与 AXI 从侧（arready/rdata/...）由 LFSR 直接驱动；AXI 主口输出与 irq_b、
// prdata/pslverr 等折进 obs。
`timescale 1ns/1ps
`default_nettype none
module syn_engine_top(input wire clk, input wire rst, output reg [127:0] obs);
    reg [191:0] lfsr;
    always @(posedge clk) begin
        if (rst) lfsr <= 192'h0123456789ABCDEF_FEDCBA9876543210_0F1E2D3C4B5A6978;
        else lfsr <= {lfsr[190:0], lfsr[191] ^ lfsr[189] ^ lfsr[187] ^ lfsr[181] ^ obs[0]};
    end

    // APB 从侧
    wire        psel    = lfsr[0];
    wire        penable = lfsr[1];
    wire        pwrite  = lfsr[2];
    wire [11:0] paddr   = lfsr[14:3];
    wire [31:0] pwdata  = lfsr[46:15];
    wire [31:0] prdata;
    wire        pready, pslverr;

    // AXI 从侧（引擎是主口，这些是它的输入）
    wire        m_axi_arready = lfsr[47];
    wire [127:0] m_axi_rdata  = lfsr[175:48];
    wire [1:0]   m_axi_rresp  = lfsr[177:176];
    wire         m_axi_rlast  = lfsr[178];
    wire         m_axi_rvalid = lfsr[179];
    wire        m_axi_awready = lfsr[180];
    wire        m_axi_wready  = lfsr[181];
    wire [1:0]  m_axi_bresp   = lfsr[183:182];
    wire        m_axi_bvalid  = lfsr[184];
    // m_axi_bready 是引擎的输出（mover 驱动），不由 LFSR 驱动

    wire [31:0] m_axi_araddr, m_axi_awaddr, m_axi_wdata_a;
    wire [7:0]  m_axi_arlen, m_axi_awlen;
    wire [2:0]  m_axi_arsize, m_axi_awsize;
    wire [1:0]  m_axi_arburst, m_axi_awburst;
    wire        m_axi_arvalid, m_axi_rready, m_axi_awvalid, m_axi_wvalid;
    wire [127:0] m_axi_wdata;
    wire [15:0] m_axi_wstrb;
    wire        m_axi_wlast;
    wire        m_axi_bready;              // 引擎的输出（mover 驱动）
    wire        irq_b, dbg_fe_cidx_err;

    engine_top u_dut (
        .clk(clk), .rst_ctrl(rst), .rst_ddr_n(~rst),
        .paddr(paddr), .psel(psel), .penable(penable), .pwrite(pwrite),
        .pwdata(pwdata), .prdata(prdata), .pready(pready), .pslverr(pslverr),
        .m_axi_araddr(m_axi_araddr), .m_axi_arlen(m_axi_arlen),
        .m_axi_arsize(m_axi_arsize), .m_axi_arburst(m_axi_arburst),
        .m_axi_arvalid(m_axi_arvalid), .m_axi_arready(m_axi_arready),
        .m_axi_rdata(m_axi_rdata), .m_axi_rresp(m_axi_rresp),
        .m_axi_rlast(m_axi_rlast), .m_axi_rvalid(m_axi_rvalid), .m_axi_rready(m_axi_rready),
        .m_axi_awaddr(m_axi_awaddr), .m_axi_awlen(m_axi_awlen),
        .m_axi_awsize(m_axi_awsize), .m_axi_awburst(m_axi_awburst),
        .m_axi_awvalid(m_axi_awvalid), .m_axi_awready(m_axi_awready),
        .m_axi_wdata(m_axi_wdata), .m_axi_wstrb(m_axi_wstrb), .m_axi_wlast(m_axi_wlast),
        .m_axi_wvalid(m_axi_wvalid), .m_axi_wready(m_axi_wready),
        .m_axi_bresp(m_axi_bresp), .m_axi_bvalid(m_axi_bvalid), .m_axi_bready(m_axi_bready),
        .irq_b(irq_b), .dbg_fe_cidx_err(dbg_fe_cidx_err)
    );

    always @(posedge clk) begin
        if (rst) obs <= 128'b0;
        else obs <= obs
                  ^ {m_axi_araddr[31:16],  m_axi_awaddr[31:16],
                     m_axi_arlen[3:0],     m_axi_awlen[3:0],
                     m_axi_arsize[1:0],    m_axi_awburst[1:0],
                     m_axi_arvalid, m_axi_rready, m_axi_awvalid, m_axi_wvalid,
                     m_axi_wlast, m_axi_wstrb[7:0],
                     irq_b, dbg_fe_cidx_err, pready, pslverr,
                     prdata[31:0] ^ m_axi_wdata[31:0]};
    end
endmodule
`default_nettype wire
