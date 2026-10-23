// 构型功能判据：真实引擎与随机反压 DDR，逐像素比对手算常量及关闭态错误三元组。
// 三行非对齐窗口跨多笔读写，覆盖命令取数与像素读的反复换主。
`timescale 1ns/1ps
`default_nettype none
`include "engine_feature.vh"
`include "engine_regs_offsets.vh"
module tb_r3;
`ifdef R3_LAT2
    localparam integer LAT=2;
`else
    localparam integer LAT=1;
`endif
    reg hold_ar=0,ctrl_reset_only=0;
    reg clk=0, rst=1;
    always #5 clk=~clk;
    reg [15:0] paddr=0;
    reg psel=0, penable=0, pwrite=0;
    reg [31:0] pwdata=0;
    wire [31:0] prdata;
    wire pready, pslverr, irq_b;
    wire [31:0] araddr, awaddr;
    wire [7:0] arlen, awlen;
    wire [2:0] arsize, awsize;
    wire [1:0] arburst, awburst, rresp, bresp;
    wire arvalid, arready, rvalid, rready, rlast;
    wire awvalid, awready, wvalid, wready, wlast, bvalid, bready;
    wire [127:0] rdata, wdata;
    wire [15:0] wstrb;
    engine_top #(.TEX_READ_LATENCY(LAT)) dut(
        .clk(clk),.rst_ctrl(rst || ctrl_reset_only),.rst_ddr_n(~rst),
        .paddr(paddr),.psel(psel),.penable(penable),.pwrite(pwrite),.pwdata(pwdata),
        .prdata(prdata),.pready(pready),.pslverr(pslverr),
        .m_axi_araddr(araddr),.m_axi_arlen(arlen),.m_axi_arsize(arsize),.m_axi_arburst(arburst),
        .m_axi_arvalid(arvalid),.m_axi_arready(arready),.m_axi_rdata(rdata),
        .m_axi_rresp(rresp),.m_axi_rlast(rlast),.m_axi_rvalid(rvalid),.m_axi_rready(rready),
        .m_axi_awaddr(awaddr),.m_axi_awlen(awlen),.m_axi_awsize(awsize),.m_axi_awburst(awburst),
        .m_axi_awvalid(awvalid),.m_axi_awready(awready),.m_axi_wdata(wdata),.m_axi_wstrb(wstrb),
        .m_axi_wlast(wlast),.m_axi_wvalid(wvalid),.m_axi_wready(wready),
        .m_axi_bresp(bresp),.m_axi_bvalid(bvalid),.m_axi_bready(bready),
        .irq_b(irq_b),.dbg_fe_cidx_err());
    axi_ram_model #(.MEM_WORDS(524288),.STRB_EN(1)) ram(
        .clk(clk),.rst_n(~rst),.awaddr(awaddr),.awlen(awlen),.awvalid(awvalid),.awready(awready),
        .wdata(wdata),.wstrb(wstrb),.wlast(wlast),.wvalid(wvalid),.wready(wready),
        .bresp(bresp),.bvalid(bvalid),.bready(bready),.araddr(araddr),.arlen(arlen),
        .arvalid(arvalid),.arready(arready),.rdata(rdata),.rresp(rresp),.rlast(rlast),
        .rvalid(rvalid),.rready(rready),.hold_aw(1'b0),.hold_ar(hold_ar));

    localparam [31:0] BG=32'h100000,DST=32'h300000,LIST=32'h500000,
                      TRI=32'h600fa0,SUB=32'h700ff0;
    reg [31:0] records[0:511],subwords[0:269],expected[0:307199],meta[0:3];
    string vectors, file_name;
    integer i,j,k,f,frames=1,n,loads=0,clears=0,fdreads=0,cycles=0,checks=0;
    reg [31:0] value,status;
    reg access_error;
    always @(posedge clk) begin
        cycles<=cycles+1;
        if(!rst) begin
            if(ctrl_reset_only && dut.ras_wr_valid)$fatal(1,"pixel write at control reset");
            if(arvalid && arready && ({1'b0,araddr[11:0]}+({5'd0,arlen}+13'd1)*16)>4096)
                $fatal(1,"AXI crossed page");
            if(dut.kick) begin loads<=0;clears<=0;fdreads<=0;end
            else begin
                if(dut.band_start && dut.band_start_ready) loads<=loads+1;
                if(dut.r3_clear) clears<=clears+1;
                if(dut.r3_fd_valid && dut.fd_ready) begin
                    fdreads<=fdreads+1;
                    if(!dut.u_mover.fd_raster) $fatal(1,"RASTER OWNER");
                end
            end
            if(dut.done_set && dut.u_r3.selected && !dut.r3_done)
                $fatal(1,"DONE before raster tail check");
            if(dut.r3_phase && (!dut.u_r3.clear_done || !dut.u_cmd.ras_load_seen))
                $fatal(1,"RAST before both completion events");
            if(dut.u_r3.started && !dut.r3_drain && dut.u_r3.u_rlst.tri_base!==TRI)
                $fatal(1,"live parameter changed current frame");
        end
    end
    task read_reg(input [15:0] address, output [31:0] data);
        begin
            @(negedge clk);paddr=address;psel=1;penable=0;pwrite=0;
            @(negedge clk);penable=1;
            @(negedge clk);data=prdata;psel=0;penable=0;
        end
    endtask
    task write_reg(input [15:0] address,input [31:0] data);
        begin
            @(negedge clk);paddr=address;psel=1;penable=0;pwrite=1;pwdata=data;
            @(negedge clk);penable=1;
            @(posedge clk);access_error=pslverr;
            @(negedge clk);psel=0;penable=0;pwrite=0;
        end
    endtask
    task put(input [31:0] address,input [31:0] data);
        begin ram.mem[address>>4][32*address[3:2]+:32]=data;end
    endtask
    function [31:0] get(input [31:0] address);
        begin get=ram.mem[address>>4][32*address[3:2]+:32];end
    endfunction
    task setup(input integer scene);
        begin
            file_name=$sformatf("%s/f%0d.meta.hex",vectors,scene);$readmemh(file_name,meta);
            file_name=$sformatf("%s/f%0d.records.hex",vectors,scene);$readmemh(file_name,records);
            file_name=$sformatf("%s/f%0d.sub.hex",vectors,scene);
            $readmemh(file_name,subwords,0,(meta[1]+3)/4-1);
            file_name=$sformatf("%s/f%0d.expected.hex",vectors,scene);$readmemh(file_name,expected);
            for(i=0;i<512;i=i+1)put(TRI+i*4,records[i]);
            for(i=0;i<(meta[1]+15)/16*4;i=i+1)put(SUB+i*4,i<(meta[1]+3)/4 ? subwords[i]:0);
            for(i=0;i<32;i=i+1)put(LIST+i*4,i<30 ? 1:0);
            for(i=0;i<30;i=i+1) begin
                put(LIST+(i+1)*32768,32'h00100001); // FILL, dst slot 1
                put(LIST+(i+1)*32768+4,0);put(LIST+(i+1)*32768+8,32'h00080000);
                put(LIST+(i+1)*32768+12,32'h00010008);
                put(LIST+(i+1)*32768+16,32'h00fedcba);
                put(LIST+(i+1)*32768+20,0);put(LIST+(i+1)*32768+24,0);
                put(LIST+(i+1)*32768+28,32'hc0000000+i);
            end
            for(j=0;j<480;j=j+1)for(i=0;i<1024;i=i+1)begin
                put(BG+j*4096+i*4,32'h12345678);put(DST+j*4096+i*4,32'hdeadbeef);
            end
            write_reg(16'h040,BG);write_reg(16'h044,32'h01e00280);write_reg(16'h048,32'h8000000c);
            write_reg(16'h04c,DST);write_reg(16'h050,32'h01e00280);write_reg(16'h054,32'h8000000c);
            write_reg(16'h008,9);write_reg(16'h020,LIST);write_reg(16'h024,0);
            write_reg(16'h02c,30);write_reg(16'h030,4);write_reg(16'h034,1);
            write_reg(16'h1008,TRI);write_reg(16'h100c,SUB);write_reg(16'h1010,meta[1]);
            write_reg(16'h1014,16);write_reg(16'h1018,32'hff000000);write_reg(16'h101c,0);
            write_reg(16'h010,3);write_reg(16'h014,3);
        end
    endtask
    task finish(input integer code,input integer stage);
        begin
            n=0;status=1;
            while((status&3)!=0 || (status&12)==0) begin
                repeat(128)@(negedge clk);read_reg(16'h00c,status);n=n+1;
                if(n>20000) begin
                    $display("STUCK cmd=%0d fe=%0d rlst=%0d rast=%0d band=%0d debug=%h",
                        dut.u_cmd.st,dut.u_cmd.fe_st,dut.u_r3.u_rlst.state,
                        dut.u_r3.u_rast.state,dut.u_mover.g_band.u_band.phase,
                        dut.u_r3.read_value);
                    $fatal(1,"completion watchdog");
                end
            end
            if((status>>4 & 15)!=code || (status&12)!=(code ? 8:4) || !irq_b)
                $fatal(1,"terminal status got=%h code=%0d",status,code);
            read_reg(16'h03c,value);if(value!=stage)$fatal(1,"error stage %0d != %0d",value,stage);
            if(!dut.r3_idle || dut.band_busy || ram.wlast_err) $fatal(1,"terminal not quiet");
            checks=checks+1;
        end
    endtask
    task compare;
        begin
            for(j=0;j<480;j=j+1)for(i=0;i<1024;i=i+1) begin
                value=get(DST+j*4096+i*4);
                if(value !== (i<640 ? expected[j*640+i]:32'hdeadbeef))
                    $fatal(1,"pixel/padding mismatch f=%0d x=%0d y=%0d got=%h want=%h",
                        f,i,j,value,i<640 ? expected[j*640+i]:32'hdeadbeef);
            end
        end
    endtask
    task error_tuple(input [31:0] idx,input [31:0] tag);
        begin
            read_reg(16'h018,value);if(value!==idx)$fatal(1,"error index %h != %h",value,idx);
            read_reg(16'h01c,value);if(value!==tag)$fatal(1,"error tag %h != %h",value,tag);
        end
    endtask
    initial begin
        if(!$value$plusargs("VECTORS=%s",vectors))$fatal(1,"missing vectors");
        if($value$plusargs("FRAMES=%d",frames))begin end
        repeat(20)@(negedge clk);rst=0;
`ifdef GFX_HAS_RAST_TEX
        file_name={vectors,"/texture.hex"};
        begin:tex_load
            reg[31:0] tex[0:4095];
            $readmemh(file_name,tex);
            for(i=0;i<4096;i=i+1)write_reg(16'h4000+i*4,tex[i]);
        end
`endif
        for(f=0;f<frames;f=f+1)begin
            setup(f);write_reg(16'h028,1);
            write_reg(16'h4000,32'hbadbad00);
            if(!access_error)$fatal(1,"busy TEX accepted");
            write_reg(16'h1008,32'hfffffff1); // shadow only; active base must stay TRI
            finish(0,0);compare;
            read_reg(16'h1044,value);if(value!=meta[2])$fatal(1,"retired reference counter");
            if(loads!=30 || clears!=30 || fdreads==0)$fatal(1,"missing phases");
            $display("FRAME PASS %0d loads=%0d clear=%0d fd=%0d",f,loads,clears,fdreads);
        end
        if($test$plusargs("PROTOCOLS"))begin
            // Make LOAD arrive first: exercise the actual clear engine late.
            setup(0);force dut.u_r3.u_rast.z_clear=1'b0;
            write_reg(16'h028,1);wait(dut.band_loaded);
            @(negedge clk);force dut.u_r3.u_rast.z_clear=1'b1;
            @(negedge clk);release dut.u_r3.u_rast.z_clear;
            finish(0,0);f=0;compare;$display("LOAD FIRST BARRIER PASS");
            // Reject full-width bad fields before any 3D work.
            for(k=0;k<11;k=k+1) begin
                setup(0);
                case(k)
                    0:write_reg(16'h1014,65536);
                    1:write_reg(16'h02c,1);
                    2:write_reg(16'h02c,29);
                    3:write_reg(16'h101c,1);
                    4:write_reg(16'h1010,32'h01000078);
                    5:write_reg(16'h1008,32'hfffffa00);
                    6:write_reg(16'h100c,32'hfffffff0);
                    7:write_reg(16'h1008,TRI+1);
                    8:write_reg(16'h100c,SUB+2);
                    9:begin write_reg(16'h044,32'h00100280);write_reg(16'h050,32'h00100280);end
                    10:write_reg(16'h008,1);
                endcase
                write_reg(16'h028,1);finish(2,7);
                error_tuple(32'hffff,0);
                $display("CONFIG REJECT PASS %0d",k);
                if(loads || clears || fdreads)$fatal(1,"bad config side effect");
            end
            setup(0);put(SUB+4,32'h0000ffff);write_reg(16'h028,1);finish(13,7);error_tuple(32'hffff,0);$display("BAD INDEX PASS");
            setup(0);put(TRI,32'h80000000);write_reg(16'h028,1);finish(2,7);error_tuple(0,0);$display("BAD RECORD PASS");
            setup(0);write_reg(16'h1010,meta[1]+2);write_reg(16'h028,1);finish(9,7);error_tuple(32'hffff,32'h001d0000|meta[3]);$display("TAIL ERROR PASS");
            setup(0);ram.err_lo_r=TRI;ram.err_hi_r=TRI+127;
            write_reg(16'h028,1);finish(9,7);error_tuple(0,0);
            ram.err_lo_r=32'hffffffff;ram.err_hi_r=0;$display("RRESP ERROR PASS");
            setup(0);write_reg(16'h028,1);wait(dut.r3_phase);
            @(negedge clk);force dut.u_r3.timeout_event=1;force dut.u_r3.core_error=1;force dut.u_r3.list_error=1;
            @(negedge clk);release dut.u_r3.timeout_event;release dut.u_r3.core_error;release dut.u_r3.list_error;
            finish(12,7);error_tuple(32'hffff,0);$display("FIRST ERROR PRIORITY PASS");
            // First subtable read is accepted while the frame controller still waits in BLOAD.
            setup(0);write_reg(16'h028,1);wait(dut.u_r3.u_rlst.state==2 && dut.u_mover.rd_busy);
            if(dut.r3_phase)$fatal(1,"missing pre-RAST timeout witness");
            @(negedge clk);force dut.u_mover.rd_tmo=13'd4096;
            @(negedge clk);release dut.u_mover.rd_tmo;
            finish(12,7);error_tuple(32'hffff,0);$display("OWNER TIMEOUT BEFORE RAST PASS");
            setup(0);write_reg(16'h028,1);wait(dut.u_r3.u_rlst.state==12 && dut.u_mover.rd_busy);
            @(negedge clk);force dut.u_mover.rd_tmo=13'd4096;
            @(negedge clk);release dut.u_mover.rd_tmo;
            finish(12,7);error_tuple(0,0);$display("OWNER TIMEOUT RECORD PASS");
        setup(0);put(LIST,20);
        for(k=0;k<20;k=k+1)begin
            put(LIST+32768+k*32,32'h00100001);
            put(LIST+32768+k*32+4,0);put(LIST+32768+k*32+8,32'h00080000);
            put(LIST+32768+k*32+12,32'h00010008);put(LIST+32768+k*32+16,32'h00fedcba);
            put(LIST+32768+k*32+20,0);put(LIST+32768+k*32+24,0);
            put(LIST+32768+k*32+28,32'hcafe0000+k);
        end
        put(LIST+32768,32'h00100077);put(LIST+32768+28,32'habcdef01);
        write_reg(16'h028,1);
        wait(dut.u_cmd.fe_owner==1 && dut.old_fd_valid);
        @(negedge clk);hold_ar=1;
        wait(dut.u_cmd.st==12 && dut.u_cmd.pend_err);
        @(negedge clk);
        if(!dut.u_cmd.fe_busy_any || !dut.u_mover.rd_busy)$fatal(1,"fixture missing pending prefetch");
        repeat(128)begin
            if(dut.r3_drain || dut.band_abort)$fatal(1,"EARLY LEGACY ABORT steals pending command prefetch");
            @(negedge clk);
        end
        hold_ar=0;finish(1,0);error_tuple(0,32'habcdef01);
        $display("LEGACY PREFETCH ERROR PASS");

            // Cancel at z clear, an accepted record read, and pixel/TEX work.
            for(k=0;k<3;k=k+1)begin
                setup(0);write_reg(16'h028,1);
                case(k)
                    0:wait(dut.u_r3.z_busy);
                    1:wait(dut.u_r3.u_rlst.state==12);
                    2:wait(dut.u_r3.u_rast.in_flight!=0);
                endcase
                write_reg(16'h008,3);
                n=0;status=3;
                while(status&3)begin
                    read_reg(16'h00c,status);n=n+1;
                    if(n>5000)$fatal(1,"reset did not drain");
                end
                if(!dut.r3_idle || dut.band_busy)$fatal(1,"reset completed early");
                $display("CANCEL PASS %0d",k);
            end
            setup(0);write_reg(16'h028,1);finish(0,0);f=0;compare;
            setup(0);write_reg(16'h028,1);wait(dut.u_r3.u_rast.in_flight!=0);
            @(negedge clk);ctrl_reset_only=1;
            repeat(5)@(negedge clk);ctrl_reset_only=0;
            wait(!dut.r3_ctrl_recovery);repeat(3)@(negedge clk);
            if(!dut.r3_idle)$fatal(1,"control reset left raster work");
            setup(0);write_reg(16'h028,1);finish(0,0);f=0;compare;
            $display("CONTROL RESET PASS");
            setup(0);force dut.u_r3.u_rast.dbg_retire_cnt=32'hfffffff8;
            @(negedge clk);release dut.u_r3.u_rast.dbg_retire_cnt;
            write_reg(16'h028,1);finish(0,0);
            read_reg(16'h1044,value);if(value!=meta[2])$fatal(1,"retirement counter wrap");
            $display("RETIREMENT WRAP PASS");
            // R3 bypass: legacy partial strip frame must have no raster cost.
            setup(0);write_reg(16'h1014,0);write_reg(16'h02c,1);
            write_reg(16'h1008,1);write_reg(16'h1010,32'hffff0001);write_reg(16'h101c,1);
            write_reg(16'h044,32'h00100280);write_reg(16'h050,32'h00100280);
            write_reg(16'h028,1);finish(0,0);
            if(clears || fdreads || loads!=1)$fatal(1,"zero-count bypass cost");
            $display("PROTOCOL PASS full-width/bad-index/bad-record/tail/reset/restart/bypass");
        end
        $display("RESULT: PASS R3 frames=%0d checks=%0d cycles=%0d",frames,checks,cycles);
        $finish;
    end
    initial begin #2000000000; $fatal(1,"global watchdog");end
endmodule
`default_nettype wire
