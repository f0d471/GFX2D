// 新采样口：完整地址域、连续换 bank、固定延迟、稳态 II=1、legacy 交接。
`timescale 1ns/1ps
`default_nettype none
module tb_rast_tex_port;
    reg clk=0,rst=1,v=0,req=0;
    always #5 clk=~clk;
`ifdef RAST_TEX_LAT2
    localparam L=2;
`else
    localparam L=1;
`endif
    reg [11:0] word=0;
    wire ready,rv,legacy_ready,legacy_v,legacy_last;
    wire [31:0] data;
    wire [127:0] legacy_data;
    integer cycle=0,sent=0,received=0,legacy_beats=0,k;
    integer sent_at[0:4095];
    reg [11:0] sent_word[0:4095];
    function [31:0] pattern;
        input [11:0] w;
        begin pattern=32'h75000000+{20'd0,w};end
    endfunction
    engine_tex #(.READ_LATENCY(L)) d(.clk(clk),.rst(rst),
        .paddr(14'd0),.psel(1'b0),.penable(1'b0),.pwrite(1'b0),.pwdata(32'd0),
        .req_addr(14'd16368),.req_bytes(12'd16),.req_buf(2'd0),.req_cidx(4'd0),
        .req_valid(req),.req_ready(legacy_ready),.rs_tdata(legacy_data),
        .rs_tvalid(legacy_v),.rs_tlast(legacy_last),.stat_ready(1'b1),
        .samp_valid(v),.samp_ready(ready),.samp_word(word),.samp_rvalid(rv),.samp_rdata(data));
    always @(posedge clk) begin
        cycle<=cycle+1;
        if(v && ready) begin sent_at[sent]<=cycle;sent_word[sent]<=word;sent<=sent+1;end
        if(rv) begin
            if(received>=sent || data!==pattern(sent_word[received]) ||
                cycle-sent_at[received]!=L) $fatal(1,"sample alignment L=%0d word=%0d got=%h",L,sent_word[received],data);
            received<=received+1;
        end
        if(legacy_v) begin
            if(!legacy_last || legacy_data!=={pattern(4095),pattern(4094),pattern(4093),pattern(4092)})
                $fatal(1,"legacy TEX after sample");
            legacy_beats<=legacy_beats+1;
        end
        if(req && v && ready) $fatal(1,"legacy/sample dual grant");
        if(cycle>5000) $fatal(1,"port watchdog");
    end
    initial begin
        for(k=0;k<1024;k=k+1) begin
            d.g_bank[0].mem[k]=pattern(k*4); d.g_bank[1].mem[k]=pattern(k*4+1);
            d.g_bank[2].mem[k]=pattern(k*4+2); d.g_bank[3].mem[k]=pattern(k*4+3);
        end
        repeat(5)@(negedge clk);rst=0;
        for(k=0;k<4096;k=k+1) begin
            v=1;word=(k*997)%4096;
            #1;if(!ready) $fatal(1,"sampling II exceeds 1");
            @(negedge clk);
        end
        v=0; wait(received==4096);@(negedge clk);
        req=1;v=1;word=0;
        while(!legacy_ready)@(negedge clk);
        @(negedge clk);req=0;v=0;
        wait(legacy_beats==1);repeat(5)@(negedge clk);
        $display("RESULT: PASS TEX latency=%0d II=1 samples=%0d legacy=%0d",L,received,legacy_beats);
        $finish;
    end
endmodule
`default_nettype wire
