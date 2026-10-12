// 条带 RAM 端口实验：全容量、逐像素掩码、同步读与异址并发。
`timescale 1ns/1ps
`default_nettype none
module tb_probe_band_ram #(parameter integer SEGMENTED=0);
    reg clk=0; always #5 clk=~clk;
    reg [11:0] wa=0,ra=0;
    reg [127:0] wd=0;
    reg [3:0] we=0; reg re=0;
    wire [127:0] q;
    reg [127:0] golden[0:2559];
    integer i,p,errors=0;
    wire [3:0] actual_we;
`ifdef INJ_BAND_MASK
    assign actual_we=|we ? 4'hf : 4'h0;
`else
    assign actual_we=we;
`endif
    wire [11:0] actual_wa;
`ifdef INJ_BAND_ADDR
    assign actual_wa={1'b0,wa[10:0]};
`else
    assign actual_wa=wa;
`endif
    probe_band_ram #(.SEGMENTED(SEGMENTED)) dut(.clk(clk),.wa(actual_wa),.ra(ra),.wd(wd),.we(actual_we),.re(re),.q(q));
    function [127:0] pattern;
        input integer address;
        integer n;
        begin
            for(n=0;n<4;n=n+1) pattern[n*32 +:32]=(address*32'h9e3779b9) ^ (n*32'h1234567) ^ 32'h50617283;
        end
    endfunction
    task check_read;
        input integer address;
        begin
            @(negedge clk); ra=address[11:0];re=1;we=0;
            @(negedge clk);
            if(q!==golden[address]) begin
                errors=errors+1;
                if(errors<5) $display("RAM-DATA addr=%0d",address);
            end
            re=0;
        end
    endtask
    initial begin
        for(i=0;i<2560;i=i+1) begin
            @(negedge clk);wa=i[11:0];wd=pattern(i);we=4'hf;golden[i]=wd;
        end
        @(negedge clk);we=0;
        for(i=0;i<2560;i=i+1) check_read(i);
        for(i=0;i<2560;i=i+1) begin
            @(negedge clk);wa=i[11:0];wd=~pattern(i);we=(i%15)+1;
            for(p=0;p<4;p=p+1) if(we[p]) golden[i][32*p +:32]=wd[32*p +:32];
        end
        @(negedge clk);we=0;
        for(i=0;i<2560;i=i+1) check_read(i);
        // 同时读低半、写高半，刻意避免同址读写的器件相关语义。
        for(i=0;i<1280;i=i+1) begin
            @(negedge clk);ra=i[11:0];wa=12'd1280+i[11:0];
            re=1;we=4'hf;wd=pattern(i+9000);golden[i+1280]=wd;
            @(negedge clk);
            if(q!==golden[i]) errors=errors+1;
            re=0;we=0;
        end
        for(i=1280;i<2560;i=i+1) check_read(i);
        $display("RAM words=2560 full_reads=6400 masked_writes=2560 concurrent=1280 errors=%0d",errors);
        if(errors==0) $display("RESULT: PASS"); else $display("RESULT: FAIL");
        $finish;
    end
endmodule
`default_nettype wire
