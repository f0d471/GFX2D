// 控制层的单元仿真测试台：engine_top（五层全部）经 APB 提交，从机是补齐后的
// axi_ram_model（4 MiB、wstrb、运行时 SLVERR 窗口）。判据：
//
//   1 十步检查：159 条一致性向量逐条喂进引擎，错误三元组逐条相同；
//     命令表放在专用地址，可读记录之后由从机回 SLVERR（vec_to_sim 转换）。
//   2 取数的边界：FETCH 落在批中间（回退后定位到那一条）、首批/下一批不可取、
//     LIST_BASE 非 32 字节对齐、批跨 4KB 拆两笔、32 位地址末端的 END 优先。
//   3 总线错误与超时（端到端）：BUS_READ/BUS_WRITE 各一例（三元组、中断 B、
//     失败命令之后的命令无像素副作用）；从机迟到超过门限报 TIMEOUT，
//     迟到响应到达后引擎回空闲、能再次提交。
//   4 软复位：排空完成后 valid 全清、裁剪框恢复全游戏区、之后能再次提交。
//   5 跨命令写后读与首错顺序：别名表面的先写后读逐字相同；空命令/NOP/SET_CLIP
//     不死锁；前一条 BUS_WRITE 先于后一条 BAD_FIELD。
//   6 PERF_CMD/PIXEL/KEY_SKIP 的精确对账与读拍、CAPS、APB 拒绝语义。
//
// 注错构建（run.sh 另编两次）：-DINJ_CHK_SWAP 对调 space/format、-DINJ_NO_FB 关掉逐条回退，
// 各自必须让至少一条向量见红（+vectors_only 只跑向量段并反转判据）。
// 开关是预处理器级宏，直接作用于被测 RTL，本测试台不持有也不转译它们。
// +vec_from=N 跳过前 N 条向量（调试用）。
//
// 地址约定：模型存储 4 MiB、按 (addr>>4) 取模映射。命令表区 0x0020_0000 起每表
// 1 KiB；向量与定向用例的像素区在低 64 KiB；0xFFFFFFE0 的读落在字 0x3FFFE
//（(0xFFFFFFE0>>4) mod 0x40000，地址 0x0003_FFE0），测试台按别名地址写。
// XRGB 目的的 X 字节恒为 0，所有内存期望只对比低 24 位。
`timescale 1ns/1ps
`default_nettype none
module tb_stripctl #(parameter integer SH=16);
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
        .MEM_WORDS(262144),
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
    task apb_wr(input [11:0] a, input [31:0] d);
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

    task apb_rd(input [11:0] a, output [31:0] d);
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

    task apb_wr_reject(input [11:0] a, input [31:0] d);
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

    task apb_rd_reject(input [11:0] a);
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


    localparam [31:0] TABLE=32'h00300000, FB=32'h00001000;
    reg [127:0] expected [0:262143];
    reg [31:0] surfaces [0:47];
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
                if(n>30000)begin $display("WATCHDOG st=%d band=%d fe=%d",dut.u_cmd.st,dut.u_mover.g_band.u_band.phase,dut.u_cmd.fe_st);$finish;end
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
        fresh(2);zero_header;apb_wr(R_DOOR,1);finish_case(0,0,0);
        if(ar_cmd!=1 || ar_pixel!=2*SH || aw_pixel!=2*SH)begin errors=errors+1;$display("EMPTY-FAIL");end
        // 背景/目的槽独立：空桶也必须将另一个背景面复制到目的。
        fresh(2);zero_header;put_surf3(3,32'h00260000,((2*SH)<<16)|32'd640,32'h8000000c);
        apb_wr(12'h034,32'h30);apb_wr(R_DOOR,1);finish_case(0,0,0);
        for(y=0;y<2*SH;y=y+1)for(w=0;w<160;w=w+1)
            if(u_ram.mem[(FB>>4)+y*256+w]!==u_ram.mem[(32'h260000>>4)+y*256+w])begin
                errors=errors+1;$display("BACKGROUND-FAIL y=%d word=%d",y,w);
            end
        fresh(2);apb_wr(R_LBASE,TABLE+32);apb_wr(R_DOOR,1);finish_case(2,0,0);
        fresh(2);apb_wr(12'h02c,31);apb_wr(R_DOOR,1);finish_case(2,0,0);
        negative(1,3,0,0,2);negative(1,255,0,0,2);
        negative(1,1,32'hffff0000,32'h00010001,2);
        negative(1,1,(SH-1)<<16,32'h00020001,2);
        negative(1,32'h00100001,0,32'h00010001,2);
        negative(1,32'h40000001,0,0,2);
        fresh(2);zero_header;put_surf3(2,FB,((2*SH)<<16)|32'd640,32'h8000000c);
        word32(TABLE+4,1);raw_cmd(1,0,32'h00020002,0,32'h00010001,32'hcafe005a);
        apb_wr(R_DOOR,1);finish_case(7,1024,32'hcafe005a);
        fresh(2);zero_header;word32(TABLE+4,1025);apb_wr(R_DOOR,1);finish_case(2,1024,0);
        fresh(2);apb_wr(12'h030,2);apb_wr(R_DOOR,1);finish_case(2,0,0);
        fresh(2);u_ram.err_lo_r=TABLE;u_ram.err_hi_r=TABLE+127;apb_wr(R_DOOR,1);finish_case(9,0,0);
        fresh(2);zero_header;word32(TABLE+4,19);
        for(i=0;i<19;i=i+1)raw_cmd(1,i,0,0,0,i+32'habc000);
        u_ram.err_lo_r=TABLE+65536+17*32;u_ram.err_hi_r=TABLE+65536+19*32-1;
        apb_wr(R_DOOR,1);finish_case(9,1041,0);
        fresh(2);zero_header;u_ram.err_lo_r=FB;u_ram.err_hi_r=FB+2559;
        apb_wr(R_DOOR,1);finish_case(10,0,0);
        fresh(2);zero_header;inject_write=1;apb_wr(R_DOOR,1);finish_case(11,0,0);
        fresh(2);zero_header;ar_hold=1;apb_wr(R_DOOR,1);repeat(5000)step;ar_hold=0;finish_case(12,0,0);
        fresh(2);zero_header;apb_wr(R_DOOR,1);repeat(50)step;apb_wr(R_CTRL,11);
        repeat(2000)step;apb_rd(R_STATUS,rd);
        if((rd&3)!=0)begin errors=errors+1;$display("RESET-FAIL");end
        fresh(2);zero_header;apb_wr(R_DOOR,1);finish_case(0,0,0);
        $display("G3 checks=%d errors=%d",checks,errors);
        if(errors==0)$display("RESULT: PASS");else $display("RESULT: FAIL");
        $finish;
    end
    initial begin #50000000;$display("WATCHDOG global");$finish;end
endmodule
