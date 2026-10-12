// RAM 组织研究：四个 1024x32 bank；APB 每次写一个 bank，读出整拍。
// APB 读回复用宽读口，正式实现须在引擎空闲时切换读地址并锁存字选择。
`timescale 1ns/1ps
`default_nettype none
module probe_tex_ram(
    input wire clk,
    input wire we,
    input wire [11:0] wa,
    input wire [31:0] wd,
    input wire re,
    input wire [9:0] ra,
    output wire [127:0] q
);
    genvar b;
    generate for (b=0; b<4; b=b+1) begin: bank
        reg [31:0] mem [0:1023];
        reg [31:0] rq;
        always @(posedge clk) begin
            if (we && wa[1:0] == b) mem[wa[11:2]] <= wd;
            if (re) rq <= mem[ra];
        end
        assign q[32*b +:32] = rq;
    end endgenerate
endmodule

module syn_tex_ram(input wire clk, rst, output reg obs);
    reg [127:0] stimulus;
    wire [127:0] q;
    always @(posedge clk) begin
        if (rst) stimulus <= 128'h193759abcde;
        else stimulus <= {stimulus[126:0], stimulus[127]^stimulus[125]^stimulus[100]^stimulus[98]};
        obs <= ^q;
    end
    probe_tex_ram u_ram(.clk(clk), .we(stimulus[0]), .wa(stimulus[12:1]),
        .wd(stimulus[44:13]), .re(stimulus[45]), .ra(stimulus[55:46]), .q(q));
endmodule
`default_nettype wire
