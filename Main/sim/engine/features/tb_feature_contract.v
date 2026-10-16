// 按构型的禁用契约：被裁掉的机制对应寄存器读 0、写拒绝，能力位与开关一致。
// 由 feature_contract.sh 对每个构型源树编译本文件；期望值由 `ifdef 决定。
`timescale 1ns/1ps
`include "engine_feature.vh"
`ifdef GFX_HAS_STRIP
`define FEATURE_BAND_REGS
`elsif GFX_HAS_PB
`define FEATURE_BAND_REGS
`endif

// 断言用宏：本机 iverilog 12 对带 integer 端口的老式 task 解析不稳，避免用任务。
`define CHECK(name, got, want) \
    begin \
        checks = checks + 1; \
        if ((got) !== (want)) begin \
            fails = fails + 1; \
            $display("  FAIL %0s got=%0d want=%0d", name, got, want); \
        end \
    end
module tb_feature_contract;
    reg clk = 1'b0;
    always #5 clk = ~clk;
    reg rst_ctrl = 1'b1, rst_ddr_n = 1'b0;
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

    // 声明一律前置：iverilog 12 对「任务之后再放 reg/localparam 声明」会误判语法。
    integer fails = 0, checks = 0;
    reg [31:0] rd; reg rerr, werr;
    localparam [11:0] REG_CAPS = 12'h004, REG_CTRL = 12'h008;
    localparam [11:0] REG_LSTRIPS = 12'h02C, REG_STRIP_H = 12'h030, REG_BAND_SURF = 12'h034;
    localparam [11:0] REG_ERROR_INSTANCE = 12'h038;
    localparam [11:0] REG_PB_BASE = 12'h140, REG_REF_CAP = 12'h14C;

    task apb_read;
        input [11:0] addr;
        output [31:0] data;
        output err;
        begin
            @(negedge clk); paddr = addr; psel = 1; penable = 0; pwrite = 0;
            @(negedge clk); penable = 1;
            @(negedge clk); data = prdata; err = pslverr; psel = 0; penable = 0;
        end
    endtask

    task apb_write;
        input [11:0] addr;
        input [31:0] data;
        output err;
        begin
            @(negedge clk); paddr = addr; psel = 1; penable = 0; pwrite = 1; pwdata = data;
            @(negedge clk); penable = 1;
            @(negedge clk); err = pslverr; psel = 0; penable = 0; pwrite = 0;
        end
    endtask


    initial begin
        repeat (4) @(negedge clk);
        rst_ctrl = 0; rst_ddr_n = 1;
        repeat (4) @(negedge clk);

        // CAPS 与构型一致：只能断言「本构型不该有的能力位为 0」，
        // 该有的位由逐位比较确认。
        apb_read(REG_CAPS, rd, rerr);
        `CHECK("CAPS 读无错", rerr, 0);
`ifdef GFX_HAS_TEX
        `CHECK("CAPS.TEX=1", rd[6], 1);
`else
        `CHECK("CAPS.TEX=0", rd[6], 0);
`endif
`ifdef GFX_HAS_BAND_HW
        `CHECK("CAPS.BAND=1", rd[5], 1);
`else
        `CHECK("CAPS.BAND=0", rd[5], 0);
`endif
`ifdef GFX_HAS_INST
        `CHECK("CAPS.INST=1", rd[12], 1);
`else
        `CHECK("CAPS.INST=0", rd[12], 0);
`endif
`ifdef GFX_HAS_PB
        `CHECK("CAPS.BIN=1", rd[13], 1);
`else
        `CHECK("CAPS.BIN=0", rd[13], 0);
`endif
`ifdef GFX_ALPHA
        `CHECK("CAPS.ALPHA_PM=1", rd[1], 1);
        `CHECK("CAPS.ALPHA_ST=1", rd[2], 1);
`else
        `CHECK("CAPS.ALPHA_PM=0", rd[1], 0);
        `CHECK("CAPS.ALPHA_ST=0", rd[2], 0);
`endif
`ifdef GFX_EQ_EXT
        `CHECK("CAPS.MUL=1", rd[3], 1);
`else
        `CHECK("CAPS.MUL=0", rd[3], 0);
`endif
`ifdef GFX_KEY
        `CHECK("CAPS.KEY=1", rd[0], 1);
`else
        `CHECK("CAPS.KEY=0", rd[0], 0);
`endif
        `CHECK("CAPS.LANES=4", rd[11:8], 4);

        // CTRL.BAND 选择命令层条带模式，由 STRIP 控制；PB 自行驱动条带数据面。
`ifdef GFX_HAS_STRIP
        apb_write(REG_CTRL, 32'h0000_0009, werr);   // EN|BAND
        `CHECK("CTRL.BAND 写被接受", werr, 0);
`else
        apb_write(REG_CTRL, 32'h0000_0001, werr);   // EN
        `CHECK("CTRL 只写 EN 被接受", werr, 0);
        apb_write(REG_CTRL, 32'h0000_000A, werr);   // EN|BAND：BAND 不支持
        `CHECK("CTRL.BAND 写被拒绝", werr, 1);
`endif
        apb_read(REG_CTRL, rd, rerr);
        `CHECK("CTRL 读回无错", rerr, 0);
        `CHECK("CTRL.EN 保留", rd[0], 1);
`ifdef GFX_HAS_STRIP
        `CHECK("CTRL.BAND 读回 1", rd[3], 1);
`else
        `CHECK("CTRL.BAND 读回 0", rd[3], 0);
`endif
        // 被拒绝的访问不能改变 EN，也不能触发软复位清空表面。
        apb_write(REG_CTRL, 32'h1, werr);
        apb_write(12'h048, 32'h80000008, werr);
`ifndef GFX_HAS_STRIP
        apb_write(REG_CTRL, 32'h0000000E, werr);
        `CHECK("无 STRIP 拒绝 BAND|SOFT_RST|CNT_CLR", werr, 1);
        repeat (12) @(negedge clk);
        apb_read(REG_CTRL, rd, rerr);
        `CHECK("拒绝 BAND 后 EN 不变", rd[0], 1);
        apb_read(12'h048, rd, rerr);
        `CHECK("拒绝 BAND 不清表面", rd, 32'h80000008);
`endif
`ifndef GFX_HAS_PB
        apb_write(REG_CTRL, 32'h00000016, werr);
        `CHECK("无 PB 拒绝 BIN|SOFT_RST|CNT_CLR", werr, 1);
        repeat (12) @(negedge clk);
        apb_read(REG_CTRL, rd, rerr);
        `CHECK("拒绝 BIN 后 EN 不变", rd[0], 1);
        apb_read(12'h048, rd, rerr);
        `CHECK("拒绝 BIN 不清表面", rd, 32'h80000008);
`endif
`ifdef GFX_HAS_PB
        `CHECK("CTRL.BIN 读回 0（未写）", rd[4], 0);
`else
        `CHECK("CTRL.BIN 读回 0", rd[4], 0);
`endif

        // 条带寄存器：无命令层条带模式时读 0、写拒绝。
        apb_read(REG_LSTRIPS, rd, rerr);
`ifdef FEATURE_BAND_REGS
        `CHECK("LSTRIPS 读无错", rerr, 0);
`else
        `CHECK("LSTRIPS 读有错", rerr, 1);
`endif
        apb_write(REG_LSTRIPS, 32'h0000_0004, werr);
`ifdef FEATURE_BAND_REGS
        `CHECK("LSTRIPS 写被接受", werr, 0);
`else
        `CHECK("LSTRIPS 写被拒绝", werr, 1);
`endif
        apb_read(REG_BAND_SURF, rd, rerr);
`ifdef FEATURE_BAND_REGS
        `CHECK("BAND_SURF 读无错", rerr, 0);
`else
        `CHECK("BAND_SURF 读有错", rerr, 1);
        `CHECK("BAND_SURF 读回 0", rd, 0);
`endif

        // PB 参数寄存器。
        apb_read(REG_PB_BASE, rd, rerr);
        `CHECK("PB_BASE 读无错（保留地址可读）", rerr, 0);
`ifdef GFX_HAS_PB
        apb_write(REG_REF_CAP, 32'h0000_0100, werr);
        `CHECK("REF_CAP 写被接受", werr, 0);
`else
        apb_write(REG_REF_CAP, 32'h0000_0100, werr);
        `CHECK("REF_CAP 写被拒绝", werr, 1);
        apb_read(REG_REF_CAP, rd, rerr);
        `CHECK("无 PB 的 REF_CAP 保持零", rd, 0);
`endif

        // ERROR_INSTANCE：无实例时恒为无实例哨兵。
        apb_read(REG_ERROR_INSTANCE, rd, rerr);
`ifdef GFX_HAS_INST
        `CHECK("ERROR_INSTANCE 读无错", rerr, 0);
`else
        `CHECK("ERROR_INSTANCE 读无错（哨兵地址仍映射）", rerr, 0);
        `CHECK("ERROR_INSTANCE=0xFFFFFFFF", rd, 32'hFFFFFFFF);
`endif

        if (fails == 0)
            $display("CONTRACT-PASS checks=%0d", checks);
        else
            $display("CONTRACT-FAIL checks=%0d fails=%0d", checks, fails);
        $finish;
    end
endmodule
