// F5 完整引擎：PB 字节、像素和 AXI 计数消费者。
`timescale 1ns/1ps
`default_nettype none
module tb_engine_pb #(parameter integer TEST=0,STRIP_H=16);
localparam integer MEM_WORDS=65536;
    reg clk=0, rst=1; always #5 clk=~clk;
    reg psel=0, penable=0, pwrite=0;
    reg [15:0] paddr=0;
    reg [31:0] pwdata=0;
    wire [31:0] prdata;
    wire pready, pslverr;

    wire [31:0] araddr, awaddr;
    wire [7:0] arlen, awlen;
    wire [2:0] arsize, awsize;
    wire [1:0] arburst, awburst;
    wire arvalid, arready, awvalid, awready;
    wire [127:0] rdata, wdata;
    wire [1:0] rresp,model_rresp,bresp;
    reg read_error_owner=0;
    assign rresp=read_error_owner?2'b10:model_rresp;
    always @(posedge clk)if(arvalid&&arready)begin
        read_error_owner<=
            (TEST==21&&araddr>=32'h80600&&araddr<32'h80620)||
            (TEST==22&&araddr==32'h80200)||
            (TEST==23&&araddr==32'h80200&&!u_eng.u_pb.bin_busy)||
            (TEST==24&&araddr==32'h61000&&!u_eng.u_pb.bin_busy);
    end
    wire rlast, rvalid, rready, wlast, wvalid, wready, bvalid, bready;
    wire [15:0] wstrb;
    wire irq_b;

    engine_top #(.STRIP_H(STRIP_H)) u_eng(
        .clk(clk), .rst_ctrl(rst), .rst_ddr_n(!rst),
        .paddr(paddr), .psel(psel), .penable(penable), .pwrite(pwrite),
        .pwdata(pwdata), .prdata(prdata), .pready(pready), .pslverr(pslverr),
        .m_axi_araddr(araddr), .m_axi_arlen(arlen), .m_axi_arsize(arsize),
        .m_axi_arburst(arburst), .m_axi_arvalid(arvalid), .m_axi_arready(arready),
        .m_axi_rdata(rdata), .m_axi_rresp(rresp), .m_axi_rlast(rlast),
        .m_axi_rvalid(rvalid), .m_axi_rready(rready),
        .m_axi_awaddr(awaddr), .m_axi_awlen(awlen), .m_axi_awsize(awsize),
        .m_axi_awburst(awburst), .m_axi_awvalid(awvalid), .m_axi_awready(awready),
        .m_axi_wdata(wdata), .m_axi_wstrb(wstrb), .m_axi_wlast(wlast),
        .m_axi_wvalid(wvalid), .m_axi_wready(wready),
        .m_axi_bresp(bresp), .m_axi_bvalid(bvalid), .m_axi_bready(bready),
        .irq_b(irq_b), .dbg_fe_cidx_err());

axi_ram_model #(.MEM_WORDS(MEM_WORDS),.STRB_EN(1),.STALL(3),.ADDR_STALL_MAX(3),
    .ERR_LO(TEST==2?32'h80000:TEST==3?32'h61000:TEST==18?32'h80240:TEST==20?32'h70000:32'hffffffff),
    .ERR_HI(TEST==2?32'h80fff:TEST==3?32'h610ff:TEST==18?32'h8027f:TEST==20?32'h7001f:32'h0)) u_ram(
        .clk(clk), .rst_n(!rst),
        .awaddr(awaddr), .awlen(awlen), .awvalid(awvalid), .awready(awready),
        .wdata(wdata), .wstrb(wstrb), .wlast(wlast), .wvalid(wvalid),
        .wready(wready), .bresp(bresp), .bvalid(bvalid), .bready(bready),
        .araddr(araddr), .arlen(arlen), .arvalid(arvalid), .arready(arready),
        .rdata(rdata), .rresp(model_rresp), .rlast(rlast), .rvalid(rvalid),
        .rready(rready), .hold_aw(1'b0), .hold_ar(hold_ar));
    reg hold_ar=0,hold_seen=0;
    integer hold_count=0;
    always @(negedge clk)if(TEST==5&&!rst)begin
        if(u_eng.fd_instance&&u_eng.fd_valid&&u_eng.fd_ready&&!hold_seen)begin hold_ar=1;hold_seen=1;end
        if(hold_ar)begin hold_count=hold_count+1;if(hold_count>=5000)hold_ar=0;end
    end

    // ---------------------------------------------------------------- APB BFM
    integer errors=0;

    task apb_wr(input [15:0] a, input [31:0] d);
        begin
            @(posedge clk);
            psel <= 1'b1; penable <= 1'b0; pwrite <= 1'b1; paddr <= a; pwdata <= d;
            @(posedge clk);
            penable <= 1'b1;
            @(posedge clk);
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
            if (pslverr !== 1'b0) begin errors = errors + 1;
                $display("APB-FAIL 读 %03x: PSLVERR=%b", a, pslverr); end
            psel <= 1'b0; penable <= 1'b0;
        end
    endtask

    // 提交一张表并等结束（D54：DONE/ERR 置位即静止）
    task submit(input [31:0] base, input [31:0] cnt, output [31:0] code);
        reg [31:0] s;
        integer g;
        begin
            apb_wr(12'h020, base);
            apb_wr(12'h024, {16'd0, cnt[15:0]});
            apb_wr(12'h028, 32'h1);
            g = 0; s = 32'h0;
            while (((s & 32'hC) == 32'd0 || (s & 32'h1) != 0) && g < 2000000) begin
                apb_rd(12'h00C, s);
                g = g + 1;
            end
            if (g >= 2000000) begin
                errors = errors + 1;
                $display("TIMEOUT-WAIT base=%08x status=%08x", base, s);
                code = 32'hdead;
            end else begin
                code = s[7:4];
            end
        end
    endtask

    // 从机存储的 32 位车道（字 = (a>>4)，低 4 位选字节_lane）
    task mwrite(input [31:0] a, input [31:0] w);
        reg [127:0] old;
        integer     sh;
        begin
            sh = {a[3:2], 2'b00} * 8;
            old = u_ram.mem[a[21:4]];
            u_ram.mem[a[21:4]] = (old & ~(128'hFFFF_FFFF << sh)) | ({96'd0, w} << sh);
        end
    endtask


    integer rd_beats=0,wr_beats=0,rb=0,wb=0;
    always @(posedge clk)if(!rst)begin
        if(rvalid&&rready)rd_beats=rd_beats+1;
        if(wvalid&&wready)wr_beats=wr_beats+1;
        if(arvalid&&arready)rb=rb+1;
        if(awvalid&&awready)wb=wb+1;
        if(u_eng.band_start && u_ram.mem[16'h8000][95:64]!==32'd2)begin
            $display("FAIL LOAD before PB READY");errors=errors+1;
        end
    end
    task cmdword(input integer idx,input integer word,input [31:0] value);
        mwrite(32'h70000+idx*32+word*4,value);
    endtask
    task surface(input integer idx,input [31:0] base,input [31:0] dim,input [31:0] ctrl);
        begin apb_wr(16'h40+idx*12,base);apb_wr(16'h44+idx*12,dim);apb_wr(16'h48+idx*12,ctrl);end
    endtask
    integer i,x,y,guard;
    reg [31:0] status,value,expected;
    initial begin
        for(i=0;i<MEM_WORDS;i=i+1)u_ram.mem[i]={4{32'hdeadc0de}};
        for(i=0;i<4*8;i=i+1)mwrite(32'h70000+i*4,0);
        for(i=0;i<256;i=i+1)mwrite(32'h60000+i*4,32'hffaa5533);
        mwrite(32'h61000,32'h00010001);mwrite(32'h61004,32'h00110002);
        cmdword(0,0,1);cmdword(0,2,0);cmdword(0,3,32'h00200280);cmdword(0,4,32'h00112233);cmdword(0,7,10);
        cmdword(1,0,32'h00010005);cmdword(1,1,32'h61000);cmdword(1,2,2);cmdword(1,3,32'h00040004);cmdword(1,5,4);cmdword(1,7,11);
        cmdword(2,0,255);
        if(TEST==15)cmdword(1,5,32'h00020004);
        repeat(5)@(posedge clk);rst<=0;repeat(5)@(posedge clk);
        surface(0,32'h10000,32'h00200280,32'h8000000c);
        surface(1,32'h60000,32'h00100010,32'h80000006);
        surface(2,32'h30000,32'h00200280,32'h8000000c);
        if(TEST==8)surface(1,32'h10000,32'h00100010,32'h80000006);
        if(TEST==14)begin
            surface(1,32'h80000,32'h00200280,32'h8000000c);
            surface(3,32'h60000,32'h00100010,32'h80000006);
            cmdword(0,0,32'h00100001);cmdword(1,0,32'h00130005);
        end
        if(TEST==12||TEST==13)begin
            for(i=0;i<8;i=i+1)mwrite(32'h71000+i*4,0);
            mwrite(32'h71000,3);mwrite(32'h7100c,32'h00060006);
            apb_wr(16'h008,1);apb_wr(16'h020,32'h71000);apb_wr(16'h024,1);apb_wr(16'h028,1);
            status=0;while((status&12)==0)apb_rd(12'h00c,status);
            if(status!=4)errors=errors+1;
        end
        apb_wr(16'h02c,32/STRIP_H);apb_wr(16'h030,$clog2(STRIP_H));apb_wr(16'h034,TEST==14?32'h21:32'h20);
        apb_wr(16'h140,32'h80000);apb_wr(16'h144,2816);apb_wr(16'h148,TEST==6?2:16);apb_wr(16'h14c,(TEST==1||TEST==13||TEST==17)?3:128);
        apb_wr(16'h008,TEST==7?21:29);
        apb_wr(16'h020,TEST==25?32'h80020:32'h70000);apb_wr(16'h024,TEST==25?0:TEST==17?2:3);apb_wr(16'h028,1);
        if(TEST==4)begin
            wait(u_eng.u_pb.u_bin.inst_fd_valid);apb_wr(16'h008,3);
            status=3;guard=0;
            while((status&3)!=0&&guard<30000)begin apb_rd(12'h00c,status);guard=guard+1;end
            if(guard>=30000)begin $display("FAIL reset drain");errors=errors+1;end
            surface(0,32'h10000,32'h00200280,32'h8000000c);
            surface(1,32'h60000,32'h00100010,32'h80000006);
            surface(2,32'h30000,32'h00200280,32'h8000000c);
            apb_wr(16'h008,29);@(negedge clk);rd_beats=0;wr_beats=0;rb=0;wb=0;
            apb_wr(16'h028,1);
        end
        status=0;guard=0;
        while((status&12)==0 && guard<300000)begin apb_rd(12'h00c,status);guard=guard+1;end
        if(TEST==12||TEST==13)begin
            if(TEST==12?status!=4:status[7:4]!=2)errors=errors+1;
            for(i=0;i<8;i=i+1)mwrite(32'h71000+i*4,0);
            mwrite(32'h71000,1);mwrite(32'h7100c,32'h00200280);mwrite(32'h71010,32'h00556677);
            apb_wr(16'h008,1);apb_wr(16'h020,32'h71000);apb_wr(16'h024,1);apb_wr(16'h028,1);
            status=0;guard=0;while((status&12)==0&&guard<300000)begin apb_rd(12'h00c,status);guard=guard+1;end
            if(status!=4)errors=errors+1;
            for(y=0;y<32;y=y+1)for(x=0;x<1024;x=x+1)begin
                expected=TEST==13?32'hdeadc0de:(x<640?32'h00112233:32'hdeadc0de);
                if(TEST==12&&((y>=1&&y<5&&x>=1&&x<5)||(y>=17&&y<21&&x>=2&&x<6)))expected=32'h00aa5533;
                if(x<6&&y<6)expected=32'h00556677;
                value=u_ram.mem[(32'h10000+y*4096+x*4)>>4]>>((x%4)*32);
                if(value!==expected)begin if(errors<5)$display("OLD-CLIP-FAIL test=%d x=%d y=%d actual=%h expected=%h",TEST,x,y,value,expected);errors=errors+1;end
            end
            $display("RESULT: %s old_clip protocol=%d errors=%d",errors==0?"PASS":"FAIL",TEST,errors);$finish;
        end
        if(TEST==25)begin
            if(status!==4)begin $display("FAIL empty input inside PB status=%h",status);errors=errors+1;end
            $display("RESULT: %s protocol=25 errors=%d",errors==0?"PASS":"FAIL",errors);$finish;
        end
        if(TEST!=0&&TEST!=4)begin
            apb_rd(12'h03c,value);
            expected=TEST==2||TEST==18?4:TEST==3||TEST==5?2:
                TEST==7||TEST==8||TEST==15||TEST==20||TEST==22?1:TEST==21||TEST==23||TEST==24?5:3;
            if(!(status&8) || value!=expected || u_eng.cmd_busy || u_eng.pb_busy)begin
                $display("FAIL error status=%h stage=%d exp=%d",status,value,expected);errors=errors+1;
            end
            if(status[7:4]!=(TEST==2||TEST==18?11:TEST==3||TEST==24?10:TEST==5?12:TEST==8?7:TEST>=20&&TEST<=23?9:2))begin
                $display("FAIL error code=%d",status[7:4]);errors=errors+1;
            end
            if(TEST==15||TEST==17||TEST==18||(TEST>=20&&TEST<=24))begin
                apb_rd(12'h038,value);
                if(value!==(TEST==24?32'd0:32'hffffffff))begin $display("FAIL packet/frame ERROR_INSTANCE=%h",value);errors=errors+1;end
                apb_rd(12'h018,value);
                if(value!==((TEST==15||TEST==18||TEST==24)?32'd1:32'd0))begin $display("FAIL ERROR_INDEX=%h",value);errors=errors+1;end
                apb_rd(12'h01c,value);
                if(value!==((TEST==15||TEST==18||TEST==24)?32'd11:32'd0))begin $display("FAIL ERROR_TAG=%h",value);errors=errors+1;end
            end
            for(y=0;y<32;y=y+1)for(x=0;x<1024;x=x+1)
                if(u_ram.mem[((TEST==14?32'h80000:32'h10000)+y*4096+x*4)>>4][x%4*32+:32]!==32'hdeadc0de)errors=errors+1;
            $display("RESULT: %s protocol=%d errors=%d",errors==0?"PASS":"FAIL",TEST,errors);$finish;
        end
        if(status!==4)begin $display("FAIL status=%h state=%d bin=%d",status,u_eng.u_pb.state,u_eng.u_pb.u_bin.state);errors=errors+1;end
        apb_rd(12'h018,value);$display("ERROR_INDEX %d",value);
        apb_rd(12'h03c,value);$display("ERROR_STAGE %d",value);
        for(y=0;y<32;y=y+1)for(x=0;x<1024;x=x+1)begin
            expected=x<640?32'h00112233:32'hdeadc0de;
            if((y>=1&&y<5&&x>=1&&x<5)||(y>=17&&y<21&&x>=2&&x<6))expected=32'h00aa5533;
            value=u_ram.mem[(32'h10000+y*4096+x*4)>>4]>>((x%4)*32);
            if(value!==expected)begin if(errors<12)$display("FAIL pixel x=%d y=%d actual=%h expected=%h",x,y,value,expected);errors=errors+1;end
        end
        if(u_ram.mem[16'h8000][95:64]!==2)begin $display("FAIL PB status");errors=errors+1;end
        if(u_ram.mem[16'h8001][63:32]!==32/STRIP_H+2)begin $display("FAIL M=%h",u_ram.mem[16'h8001]);errors=errors+1;end
        apb_rd(12'h100,value);if(value!=2)begin $display("FAIL CMD=%d",value);errors=errors+1;end
        apb_rd(12'h124,value);if(value!=2)begin $display("FAIL INST=%d",value);errors=errors+1;end
        apb_rd(12'h128,value);if(value!=32/STRIP_H+2)begin $display("FAIL REF=%d",value);errors=errors+1;end
        apb_rd(12'h114,value);if(value!=rd_beats)begin $display("FAIL RD=%d obs=%d",value,rd_beats);errors=errors+1;end
        apb_rd(12'h118,value);if(value!=wr_beats)begin $display("FAIL WR=%d obs=%d",value,wr_beats);errors=errors+1;end
        apb_rd(12'h10c,value);if(value!=rb)begin $display("FAIL RB=%d obs=%d",value,rb);errors=errors+1;end
        apb_rd(12'h110,value);if(value!=wb)begin $display("FAIL WB=%d obs=%d",value,wb);errors=errors+1;end
        $display("RESULT: %s errors=%d guard=%d",errors==0?"PASS":"FAIL",errors,guard);$finish;
    end
    initial begin #20000000;$display("RESULT: FAIL watchdog pb=%d bin=%d dma=%d",u_eng.u_pb.state,u_eng.u_pb.u_bin.state,u_eng.u_pb.u_dma.state);$finish;end
endmodule
`default_nettype wire
