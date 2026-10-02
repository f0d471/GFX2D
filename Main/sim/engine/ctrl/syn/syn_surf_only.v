// 隔离实验：只综合 check_surf16(PRECALC=1)
`timescale 1ns/1ps
`default_nettype none
module syn_surf_only(input wire clk, input wire rst, output reg [127:0] obs);
    reg [255:0] lfsr;
    always @(posedge clk) begin
        if (rst) lfsr <= 256'h0123456789ABCDEF_FEDCBA9876543210_00000000DEADBEEF;
        else lfsr <= {lfsr[254:0], lfsr[255] ^ lfsr[253] ^ lfsr[251] ^ lfsr[246] ^ obs[0]};
    end
    wire [31:0] a0,a1,a2,b0,b1,b2; wire [15:0] sok;
    check_surf16 #(.PRECALC(1)) u(
        .clk(clk), .rst(rst),
        .we(lfsr[0]), .slot(lfsr[4:1]), .wsel(lfsr[6:5]), .wdata(lfsr[38:7]),
        .ra_id(lfsr[42:39]), .rb_id(lfsr[46:43]),
        .ra_w0(a0), .ra_w1(a1), .ra_w2(a2),
        .rb_w0(b0), .rb_w1(b1), .rb_w2(b2),
        .struct_ok(sok));
    always @(posedge clk) begin
        if (rst) obs <= 128'b0;
        else obs <= obs ^ {a0[15:0] ^ b2[31:16], a1[7:0], b1[15:8], sok, lfsr[7:0]};
    end
endmodule
`default_nettype wire
