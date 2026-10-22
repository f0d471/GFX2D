
`timescale 1ns/1ps
module tb_tex_probe;
reg clk=0,rst=1,v=0;reg[11:0] w=0;
always #5 clk=~clk;
wire ready,rv;wire[31:0] rd;
`ifdef LAT2
localparam integer LAT=2;
`else
localparam integer LAT=1;
`endif
engine_tex #(.READ_LATENCY(LAT)) d(.clk(clk),.rst(rst),.paddr(14'd0),.psel(1'b0),.penable(1'b0),.pwrite(1'b0),.pwdata(32'd0),.req_addr(14'd0),.req_bytes(12'd0),.req_buf(2'd0),.req_cidx(4'd0),.req_valid(1'b0),.stat_ready(1'b1),.samp_valid(v),.samp_ready(ready),.samp_word(w),.samp_rvalid(rv),.samp_rdata(rd));
task sample;
input[11:0] word;
input[31:0] expected;
begin
@(negedge clk);while(!ready)@(negedge clk);w=word;v=1;
@(posedge clk);#1;v=0;
while(!rv)begin @(posedge clk);#1;end
$display("PROBE_TEX LAT=%0d word=%0d actual=%h expected=%h match=%b",LAT,word,rd,expected,rd===expected);
repeat(2)@(negedge clk);
end
endtask
initial begin
d.g_bank[0].mem[0]=32'h11000001;d.g_bank[1].mem[0]=32'h22000002;
d.g_bank[2].mem[0]=32'h33000003;d.g_bank[3].mem[0]=32'h44000004;
repeat(3)@(negedge clk);rst=0;
sample(1,32'h22000002);sample(0,32'h11000001);sample(2,32'h33000003);sample(3,32'h44000004);
$finish;
end
initial begin #10000;$display("PROBE_TEX_TIMEOUT");$finish;end
endmodule
