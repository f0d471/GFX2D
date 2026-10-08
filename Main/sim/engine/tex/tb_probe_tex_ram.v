// RAM bank 的端口级实验：32 位装载、128 位同步读，覆盖每个字与边界。
`timescale 1ns/1ps
`default_nettype none
module tb_probe_tex_ram;
    reg clk=0; always #5 clk=~clk;
    reg we=0, re=0;
    reg [11:0] wa=0;
    reg [9:0] ra=0;
    reg [31:0] wd=0;
    wire [127:0] q;
    integer p, n, b, errors=0;
    reg [127:0] want;
    probe_tex_ram dut(.clk(clk), .we(we), .wa(wa), .wd(wd), .re(re), .ra(ra), .q(q));
    function [31:0] pattern(input integer word_i, input integer pass_i);
        pattern = (32'h9e3779b9 * word_i) ^ (pass_i ? 32'ha5a55a5a : 32'h5a5aa5a5);
    endfunction
    initial begin
        for (p=0; p<2; p=p+1) begin
            for (n=0; n<4096; n=n+1) begin
                @(negedge clk); we=1; wa=n; wd=pattern(n,p);
            end
            @(negedge clk); we=0;
            for (n=0; n<1024; n=n+1) begin
                re=1; ra=n;
                for (b=0; b<4; b=b+1) want[b*32 +:32]=pattern(n*4+b,p);
                @(negedge clk);
                if (q !== want) begin
                    errors=errors+1;
                    if (errors<5) $display("RAM_MISMATCH pass=%0d beat=%0d",p,n);
                end
            end
            re=0;
        end
        $display("RAM checks=2048 writes=8192 errors=%0d",errors);
        if (errors) $fatal(1,"RAM RESULT: FAIL");
        $display("RAM RESULT: PASS");
        $finish;
    end
    initial begin #200000; $fatal(1,"RAM watchdog"); end
endmodule
`default_nettype wire
