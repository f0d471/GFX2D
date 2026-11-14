// 真实游戏整帧：生产组帧、TEX 热集、随机反压、可见区与 padding、PERF。
`timescale 1ns/1ps
`default_nettype none
module tb_g4_frame #(parameter integer SH=16);
    reg clk = 1'b0;
    reg rst = 1'b1;
    always #5 clk = ~clk;

    // APB
    reg         psel = 1'b0, penable = 1'b0, pwrite = 1'b0;
    reg [15:0]  paddr = 16'd0;
    reg [31:0]  pwdata = 32'd0;
    wire [31:0] prdata;
    wire        pready, pslverr;

    // AXI
    wire [31:0] araddr;  wire [7:0] arlen;
    wire        arvalid, arready;
    wire [127:0] rdata;  wire [1:0] rresp;
    wire        rlast, rvalid;
    wire        rready;                  // 引擎主口的输出
    wire [31:0] awaddr;  wire [7:0] awlen;
    wire        awvalid, awready;
    wire [127:0] wdata;   wire [15:0] wstrb;
    wire        wlast, wvalid, wready;
    wire [1:0] bresp, model_bresp; wire bvalid;
    assign bresp = inject_write ? 2'b10 : model_bresp;
    wire        bready;
    reg         ar_hold = 1'b0, aw_hold = 1'b0;

    wire irq_b;
    wire fe_cidx_err;

    engine_top #(.STRIP_H(SH)) dut (
        .clk(clk), .rst_ctrl(rst), .rst_ddr_n(~rst),
        .paddr(paddr), .psel(psel), .penable(penable), .pwrite(pwrite),
        .pwdata(pwdata), .prdata(prdata), .pready(pready), .pslverr(pslverr),
        .m_axi_araddr(araddr), .m_axi_arlen(arlen), .m_axi_arsize(), .m_axi_arburst(),
        .m_axi_arvalid(arvalid), .m_axi_arready(arready),
        .m_axi_rdata(rdata), .m_axi_rresp(rresp), .m_axi_rlast(rlast),
        .m_axi_rvalid(rvalid), .m_axi_rready(rready),
        .m_axi_awaddr(awaddr), .m_axi_awlen(awlen), .m_axi_arsize(), .m_axi_awburst(),
        .m_axi_awvalid(awvalid), .m_axi_awready(awready),
        .m_axi_wdata(wdata), .m_axi_wstrb(wstrb), .m_axi_wlast(wlast),
        .m_axi_wvalid(wvalid), .m_axi_wready(wready),
        .m_axi_bresp(bresp), .m_axi_bvalid(bvalid), .m_axi_bready(bready),
        .irq_b(irq_b), .dbg_fe_cidx_err(fe_cidx_err)
    );

    axi_ram_model #(
        .MEM_WORDS(524288),
        .STRB_EN(1)
    ) u_ram (
        .clk(clk), .rst_n(~rst),
        .awaddr(awaddr), .awlen(awlen), .awvalid(awvalid), .awready(awready),
        .wdata(wdata), .wstrb(wstrb), .wlast(wlast), .wvalid(wvalid), .wready(wready),
        .bresp(model_bresp), .bvalid(bvalid), .bready(bready),
        .araddr(araddr), .arlen(arlen), .arvalid(arvalid), .arready(arready),
        .rdata(rdata), .rresp(rresp), .rlast(rlast), .rvalid(rvalid), .rready(rready),
        .hold_aw(aw_hold), .hold_ar(ar_hold)
    );

    localparam [11:0] R_CTRL  = 12'h008, R_STATUS = 12'h00C, R_IRQ_EN = 12'h010,
                      R_IRST  = 12'h014, R_EIDX   = 12'h018, R_ETAG  = 12'h01C,
                      R_LBASE = 12'h020, R_LCNT   = 12'h024, R_DOOR  = 12'h028,
                      R_SURF  = 12'h040, R_PERF   = 12'h100;
    localparam [31:0] ST_BUSY = 32'h1, ST_DRAIN = 32'h2, ST_DONE = 32'h4,
                      ST_ERR  = 32'h8;

    localparam [31:0] VBASE = 32'h0020_0000;   // 向量命令表区（每表 1 KiB 跨距）
    localparam [31:0] LBASE = 32'h0020_4000;   // 定向用例命令表区
    localparam [31:0] WORK  = 32'h0001_0000;   // 定向用例像素区

    integer errors   = 0;
    integer checks   = 0;
    integer vec_fail = 0;
    integer vec_total = 0;
    integer guard_g;

    task step; begin @(posedge clk); end endtask

    // ---------------------------------------------------------------- APB BFM
    task apb_wr(input [15:0] a, input [31:0] d);
        begin
            @(posedge clk);
            psel <= 1'b1; penable <= 1'b0; pwrite <= 1'b1; paddr <= a; pwdata <= d;
            @(posedge clk);
            penable <= 1'b1;
            @(posedge clk);
            checks = checks + 1;
            if (pslverr !== 1'b0) begin errors = errors + 1;
                $display("APB-FAIL 写 %03x: PSLVERR=%b", a, pslverr); end
            psel <= 1'b0; penable <= 1'b0; pwrite <= 1'b0;
        end
    endtask

    task apb_rd(input [15:0] a, output [31:0] d);
        begin
            @(posedge clk);
            psel <= 1'b1; penable <= 1'b0; pwrite <= 1'b0; paddr <= a;
            @(posedge clk);
            penable <= 1'b1;
            @(posedge clk);
            d = prdata;
            checks = checks + 1;
            if (pslverr !== 1'b0) begin errors = errors + 1;
                $display("APB-FAIL 读 %03x: PSLVERR=%b", a, pslverr); end
            psel <= 1'b0; penable <= 1'b0;
        end
    endtask

    task apb_wr_reject(input [15:0] a, input [31:0] d);
        begin
            @(posedge clk);
            psel <= 1'b1; penable <= 1'b0; pwrite <= 1'b1; paddr <= a; pwdata <= d;
            @(posedge clk);
            penable <= 1'b1;
            @(posedge clk);
            checks = checks + 1;
            if (pslverr !== 1'b1) begin errors = errors + 1;
                $display("APB-FAIL 写 %03x 应拒绝: PSLVERR=%b", a, pslverr); end
            psel <= 1'b0; penable <= 1'b0; pwrite <= 1'b0;
        end
    endtask

    task apb_rd_reject(input [15:0] a);
        begin
            @(posedge clk);
            psel <= 1'b1; penable <= 1'b0; pwrite <= 1'b0; paddr <= a;
            @(posedge clk);
            penable <= 1'b1;
            @(posedge clk);
            checks = checks + 1;
            if (pslverr !== 1'b1) begin errors = errors + 1;
                $display("APB-FAIL 读 %03x 应拒绝: PSLVERR=%b", a, pslverr); end
            psel <= 1'b0; penable <= 1'b0;
        end
    endtask

    task put_surf3(input integer slot, input [31:0] w0, input [31:0] w1, input [31:0] w2);
        begin
            apb_wr(12'h040 + slot*12 + 0, w0);
            apb_wr(12'h040 + slot*12 + 4, w1);
            apb_wr(12'h040 + slot*12 + 8, w2);
        end
    endtask


    localparam [31:0] TABLE=32'h00600000, FB=32'h00001000;
    reg [127:0] expected [0:524287];
    reg [31:0] surfaces [0:47];
    reg [31:0] texture [0:4095];
    reg [31:0] stats [0:2];
    reg [2047:0] prefix;
    reg inject_write=0;
    integer ar_cmd=0, ar_pixel=0, aw_pixel=0;
    always @(posedge clk) begin
        if(arvalid && arready) begin
            if(araddr>=TABLE) ar_cmd<=ar_cmd+1; else ar_pixel<=ar_pixel+1;
        end
        if(awvalid && awready)aw_pixel<=aw_pixel+1;

    end
    task fresh(input integer strips);
        integer i;
        begin
            inject_write=0;u_ram.err_lo_r=32'hffffffff;u_ram.err_hi_r=0;
            rst=1;repeat(5)step;rst=0;repeat(10)step;
            $readmemh({prefix,".init"},u_ram.mem);
            for(i=0;i<16;i=i+1)put_surf3(i,surfaces[i*3],surfaces[i*3+1],surfaces[i*3+2]);
            apb_wr(R_SURF+4,((strips*SH)<<16)|32'd640);
            apb_wr(12'h02c,strips);apb_wr(12'h030,$clog2(SH));apb_wr(12'h034,0);
            for(i=0;i<4096;i=i+1)apb_wr(16'h4000+i*4,texture[i]);
            apb_wr(R_IRQ_EN,3);apb_wr(R_IRST,3);apb_wr(R_CTRL,13);
            apb_wr(R_LBASE,TABLE);apb_wr(R_LCNT,0);
            ar_cmd=0;ar_pixel=0;aw_pixel=0;
        end
    endtask
    task word32(input integer address,input [31:0] value);
        begin u_ram.mem[address>>4][(address%16)*8 +:32]=value;end
    endtask
    task zero_header;
        integer i;begin for(i=0;i<32;i=i+1)word32(TABLE+4*i,0);end
    endtask
    task raw_cmd(input integer s,input integer k,input [31:0] op,input [31:0] xy,
                 input [31:0] size,input [31:0] tag);
        integer a;
        begin
            a=TABLE+(s+1)*32768+k*32;
            u_ram.mem[a>>4]={size,xy,32'd0,op};u_ram.mem[(a>>4)+1]={tag,96'd0};
        end
    endtask
    task finish_case(input [31:0] code,input [31:0] idx,input [31:0] tag);
        reg [31:0] stv,eidx,etag;integer n;
        begin
            n=0;stv=1;
            while((stv&1)!=0 || (stv&12)==0) begin
                repeat(20)step;apb_rd(R_STATUS,stv);n=n+1;
                if(n>300000)begin $display("WATCHDOG st=%d band=%d fe=%d",dut.u_cmd.st,dut.u_mover.g_band.u_band.phase,dut.u_cmd.fe_st);$finish;end
            end
            apb_rd(R_EIDX,eidx);apb_rd(R_ETAG,etag);checks=checks+1;
            if(((stv>>4)&15)!=code || eidx!=idx || etag!=tag ||
               (code==0 ? ((stv&12)!=4) : ((stv&12)!=8)) || !irq_b) begin
                errors=errors+1;$display("CASE-FAIL code=%d idx=%d tag=%h expected=%d/%d/%h st=%h",(stv>>4)&15,eidx,etag,code,idx,tag,stv);
            end
            if(dut.u_mover.band_busy || dut.cmd_busy || fe_cidx_err || u_ram.wlast_err!=0)begin
                errors=errors+1;$display("DRAIN-FAIL");end
        end
    endtask
    task negative(input integer s,input [31:0] op,input [31:0] xy,input [31:0] size,input integer code);
        begin
            fresh(2);zero_header;word32(TABLE+s*4,1);raw_cmd(s,0,op,xy,size,32'hcafe005a);
            apb_wr(R_DOOR,1);finish_case(code,s*1024,32'hcafe005a);
        end
    endtask
    integer y,w,i,diff;
    reg [31:0] rd,wr,beat;
    initial begin
        if(!$value$plusargs("case=%s",prefix))prefix="build/scene";
        $readmemh({prefix,".tex"},texture);$readmemh({prefix,".stats"},stats);
        $readmemh({prefix,".surf"},surfaces);$readmemh({prefix,".gold"},expected);
        fresh(480/SH);apb_wr(R_DOOR,1);finish_case(0,0,0);
        diff=0;
        for(y=0;y<480;y=y+1)for(w=0;w<256;w=w+1)
            if(u_ram.mem[(FB>>4)+y*256+w]!==expected[(FB>>4)+y*256+w])begin
                diff=diff+1;if(diff<5)$display("PIXEL-FAIL y=%d word=%d got=%h exp=%h",y,w,u_ram.mem[(FB>>4)+y*256+w],expected[(FB>>4)+y*256+w]);
            end
        checks=checks+1;if(diff)errors=errors+1;
        apb_rd(R_PERF+12,rd);apb_rd(R_PERF+16,wr);apb_rd(R_PERF+20,beat);
        if(rd!=ar_cmd+ar_pixel || wr!=aw_pixel || ar_pixel<480 || aw_pixel!=480)begin
            errors=errors+1;$display("PERF-FAIL rd=%d ar=%d wr=%d aw=%d",rd,ar_cmd+ar_pixel,wr,aw_pixel);end
        $display("G3 FRAME SH=%d diff=%d cmd_reads=%d pixel_reads=%d writes=%d DDR_beats=%d",SH,diff,ar_cmd,ar_pixel,aw_pixel,beat);
        apb_rd(R_PERF+4,rd);apb_rd(R_PERF+8,wr);
        if(rd!==stats[0] || wr!==stats[1])begin
            errors=errors+1;$display("PIXEL-COUNT-FAIL got=%d/%d want=%d/%d",rd,wr,stats[0],stats[1]);end
        $display("G4 game frame checks=%d errors=%d",checks,errors);
        if(errors==0)$display("RESULT: PASS");else $display("RESULT: FAIL");
        $finish;
    end
    initial begin #100000000;$display("WATCHDOG global");$finish;end
endmodule
