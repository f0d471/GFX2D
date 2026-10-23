// 构型功能判据：真实引擎与随机反压 DDR，逐像素比对手算常量及关闭态错误三元组。
// 三行非对齐窗口跨多笔读写，覆盖命令取数与像素读的反复换主。
`timescale 1ns/1ps
`default_nettype none
`include "engine_feature.vh"
`include "engine_regs_offsets.vh"
module tb_feature_function;
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
    engine_top dut(
        .clk(clk),.rst_ctrl(rst),.rst_ddr_n(~rst),
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
    axi_ram_model #(.MEM_WORDS(65536),.STRB_EN(1)) ram(
        .clk(clk),.rst_n(~rst),.awaddr(awaddr),.awlen(awlen),.awvalid(awvalid),.awready(awready),
        .wdata(wdata),.wstrb(wstrb),.wlast(wlast),.wvalid(wvalid),.wready(wready),
        .bresp(bresp),.bvalid(bvalid),.bready(bready),.araddr(araddr),.arlen(arlen),
        .arvalid(arvalid),.arready(arready),.rdata(rdata),.rresp(rresp),.rlast(rlast),
        .rvalid(rvalid),.rready(rready),.hold_aw(1'b0),.hold_ar(1'b0));
    localparam [31:0] DST=32'h1000, SRC=32'h2000, LIST=32'h10000;
    integer failures=0, checks=0, i, eqn, enabled, pb_guard;
    reg [31:0] value, status, expected, before_count;
    reg access_error;
    reg [255:0] packet;
    task read_reg(input [15:0] address, output [31:0] data);
        begin
            @(negedge clk);paddr=address;psel=1;penable=0;pwrite=0;
            @(negedge clk);penable=1;
            @(negedge clk);data=prdata;access_error=pslverr;psel=0;penable=0;
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
    task check(input [255:0] label,input [31:0] actual,input [31:0] wanted);
        begin
            checks=checks+1;
            if(actual !== wanted)begin
                failures=failures+1;
                $display("CHECK-FAIL %0s got=%08x want=%08x",label,actual,wanted);
            end
        end
    endtask
    task put_word(input [31:0] address,input [31:0] data);
        begin ram.mem[address>>4][32*address[3:2]+:32]=data;end
    endtask
    function [31:0] get_word(input [31:0] address);
        begin get_word=ram.mem[address>>4][32*address[3:2]+:32];end
    endfunction
    function [255:0] blit_packet(input [3:0] equation);
        reg [255:0] result;
        begin
            result=0;
            result[31:0]=32'h00010002 | ({28'd0,equation}<<8);
            if(equation==3)result[31:24]=64;
            result[63:32]=1;
            result[95:64]=1;
            result[127:96]=32'h00030006;
            result[255:224]=32'h5a123456;
            blit_packet=result;
        end
    endfunction
    task surfaces(input [1:0] space,input [3:0] format);
        begin
            write_reg(16'h040,DST);write_reg(16'h044,32'h00040040);
            write_reg(16'h048,32'h80000008);
            write_reg(16'h04c,space==1?0:SRC);write_reg(16'h050,32'h00040040);
            write_reg(16'h054,32'h80000008 | ({30'd0,space}<<8) | ({28'd0,format}<<12));
        end
    endtask
    task execute_packet(input [255:0] command,input [3:0] error_code,input [31:0] pixel);
        integer n,g,x,y;
        reg [31:0] wanted, actual;
        begin
            for(n=0;n<256;n=n+1)begin
                put_word(DST+4*n,32'h00102030);put_word(SRC+4*n,32'h80604020);
            end
            for(n=0;n<8;n=n+1)begin
                put_word(LIST+4*n,command[32*n+:32]);
                put_word(LIST+32+4*n,n==0?32'hff:0);
            end
            write_reg(16'h020,LIST);write_reg(16'h024,2);write_reg(16'h028,1);
            check("door accepted",access_error,0);
            g=0;status=0;
            while((status[0] || !status[3:2]) && g<10000)begin
                read_reg(16'h00c,status);g=g+1;
            end
            check("completed",g<10000,1);
            check("error code",status[7:4],error_code);
            check("idle",status[0],0);
            if(error_code!=0)begin
                read_reg(16'h018,actual);check("error index",actual,0);
                read_reg(16'h01c,actual);check("error tag",actual,command[255:224]);
            end
            for(n=0;n<256;n=n+1)begin
                x=n%64;y=n/64;
                wanted=error_code==0 && x>=1 && x<7 && y<3?pixel:32'h00102030;
                check("destination and guards",get_word(DST+4*n),wanted);
                check("source unchanged",get_word(SRC+4*n),32'h80604020);
            end
            check("WLAST",ram.wlast_err,0);
        end
    endtask
    initial begin
        repeat(8)@(negedge clk);rst=0;
        repeat(4)@(negedge clk);
        read_reg(16'h004,value);check("CAPS",value,EC_CAPS_VALUE);
        read_reg(16'h1000,value);
`ifdef GFX_HAS_RAST
        check("RAST ID",value,EC_R3_ID_VALUE);check("RAST read",access_error,0);
`else
        check("RAST absent data",value,0);check("RAST absent read",access_error,1);
`endif
        write_reg(16'h1014,1);
`ifdef GFX_HAS_RAST
        check("RAST parameter write",access_error,0);write_reg(16'h1014,0);
`else
        check("RAST absent write",access_error,1);
`endif
        write_reg(16'h008,1);
        surfaces(0,1);
        packet=blit_packet(0);packet[31:0]=1;packet[63:32]=0;
        packet[159:128]=32'h00a0b0c0;
        execute_packet(packet,0,32'h00a0b0c0);
        for(eqn=0;eqn<6;eqn=eqn+1)begin
            enabled=1;
`ifndef GFX_ALPHA
            if(eqn==1 || eqn==5)enabled=0;
`endif
`ifndef GFX_EQ_EXT
            if(eqn==3 || eqn==4)enabled=0;
`endif
            case(eqn)
                0:expected=32'h00604020;
                1:expected=32'h00685038;
                2:expected=32'h00706050;
                3:expected=32'h0024282c;
                4:expected=32'h00060806;
                5:expected=32'h00383028;
            endcase
            surfaces(0,eqn==5?2:1);
            execute_packet(blit_packet(eqn),enabled?0:3,expected);
        end
        surfaces(0,1);packet=blit_packet(0);packet[12]=1;packet[191:160]=32'h80604020;
`ifdef GFX_KEY
        execute_packet(packet,0,32'h00102030);
`else
        execute_packet(packet,2,0);
`endif
        surfaces(1,1);
`ifdef GFX_HAS_TEX
        for(i=0;i<256;i=i+1)write_reg(16'h4000+4*i,32'h80604020);
        execute_packet(blit_packet(0),0,32'h00604020);

`else
        execute_packet(blit_packet(0),5,0);
        write_reg(16'h4000,0);check("no TEX window",access_error,1);
`endif
        surfaces(0,1);packet=blit_packet(0);
        packet[31:0]=32'h00010005;packet[63:32]=32'h3000;
        packet[95:64]=1;packet[127:96]=32'h00030006;packet[175:160]=4;
        packet[223:192]=1;put_word(32'h3000,1);
`ifdef GFX_HAS_INST
        execute_packet(packet,0,32'h00604020);
`else
        execute_packet(packet,1,0);
`endif
        read_reg(16'h100,before_count);
`ifndef GFX_HAS_STRIP
        write_reg(16'h008,32'h0e);check("reject BAND write",access_error,1);
        read_reg(16'h008,value);check("reject BAND preserves EN",value[0],1);
        read_reg(16'h100,value);check("reject BAND preserves PERF",value,before_count);
`endif
`ifndef GFX_HAS_PB
        write_reg(16'h008,32'h16);check("reject BIN write",access_error,1);
        read_reg(16'h008,value);check("reject BIN preserves EN",value[0],1);
        read_reg(16'h100,value);check("reject BIN preserves PERF",value,before_count);
`endif
        execute_packet(blit_packet(0),0,32'h00604020);
`ifdef GFX_HAS_PB
        // 真实 PB LOAD/EXEC/FLUSH：同一组参数在 STRIP 开关两种构型都必须生效。
        for(i=0;i<16384;i=i+1)begin
            put_word(32'h20000+4*i,32'h00102030);
            put_word(32'h40000+4*i,32'h00deadbe);
        end
        write_reg(16'h040,32'h40000);write_reg(16'h044,32'h00100280);
        write_reg(16'h048,32'h8000000c);
        write_reg(16'h058,32'h20000);write_reg(16'h05c,32'h00100280);
        write_reg(16'h060,32'h8000000c);
        write_reg(16'h02c,1);write_reg(16'h030,4);write_reg(16'h034,32'h20);
        check("PB band config accepted",access_error,0);
        write_reg(16'h140,32'h80000);write_reg(16'h144,32'h20000);
        write_reg(16'h148,16);write_reg(16'h14c,1024);
        write_reg(16'h008,32'h19);check("PB CTRL accepted",access_error,0);
        write_reg(16'h024,1);write_reg(16'h028,1);
        pb_guard=0;status=0;
        while((status[0] || !status[3:2]) && pb_guard<100000)begin
            read_reg(16'h00c,status);pb_guard=pb_guard+1;
        end
        check("PB completed",pb_guard<100000,1);
        check("PB error",status[7:4],0);
        for(i=0;i<16384;i=i+1)begin
            if(i%1024<640)
                check("PB pixels and guards",get_word(32'h40000+4*i),
                      i%1024>=1 && i%1024<7 && i/1024<3?32'h00604020:32'h00102030);
            else
                check("PB row padding",get_word(32'h40000+4*i),32'h00deadbe);
        end
`endif
`ifdef GFX_HAS_STRIP
        // PB 完成后直接切到 STRIP；门铃必须锁存新请求的后端，而非上一帧所有者。
        for(i=0;i<16384;i=i+1)begin
            put_word(32'h20000+4*i,32'h00102030);
            put_word(32'h40000+4*i,32'h00deadbe);
        end
        write_reg(16'h040,32'h40000);write_reg(16'h044,32'h00100280);
        write_reg(16'h048,32'h8000000c);
        write_reg(16'h058,32'h20000);write_reg(16'h05c,32'h00100280);
        write_reg(16'h060,32'h8000000c);
        for(i=0;i<32;i=i+1)put_word(LIST+4*i,i==0?1:0);
        packet=blit_packet(0);
        for(i=0;i<8;i=i+1)put_word(LIST+32768+4*i,packet[32*i+:32]);
        write_reg(16'h02c,1);write_reg(16'h030,4);write_reg(16'h034,32'h20);
        write_reg(16'h008,9);write_reg(16'h024,0);write_reg(16'h028,1);
        pb_guard=0;status=0;
        while((status[0] || !status[3:2]) && pb_guard<100000)begin
            read_reg(16'h00c,status);pb_guard=pb_guard+1;
        end
        check("STRIP completed",pb_guard<100000,1);
        check("STRIP error",status[7:4],0);
        for(i=0;i<16384;i=i+1)
            check("STRIP pixels and guards",get_word(32'h40000+4*i),
                  i%1024>=640?32'h00deadbe:
                  i%1024>=1 && i%1024<7 && i/1024<3?32'h00604020:32'h00102030);
`endif
        packet=0;packet[31:0]=3;packet[63:32]=32'h00140014;packet[127:96]=32'h000b000b;
        // 切回普通表并设置完全位于小测试面的窗口外的 clip。
        write_reg(16'h008,1);surfaces(0,1);
        execute_packet(packet,0,32'h00102030);
        write_reg(16'h008,3);
        pb_guard=0;read_reg(16'h00c,status);
        while(status[1:0]!=0 && pb_guard<10000)begin
            read_reg(16'h00c,status);pb_guard=pb_guard+1;
        end
        check("soft reset idle",pb_guard<10000,1);
        surfaces(0,1);
        execute_packet(blit_packet(0),0,32'h00604020);
        if(failures==0)$display("FEATURE-FUNCTION-PASS checks=%0d",checks);
        else $display("FEATURE-FUNCTION-FAIL checks=%0d failures=%0d",checks,failures);
        $finish;
    end
    initial begin
        #2000000;
        $display("FEATURE-FUNCTION-FAIL watchdog");$finish;
    end
endmodule
`default_nettype wire
