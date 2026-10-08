// G2 前期研究：分相复用的一读一写 RAM，未接入生产 RTL。
// PIXEL_MASK=1 时每个 32 位像素独立写使能，模拟 mover 的带内不写标记。
// PIXEL_MASK=0 是无掩码容量下界对照，不能直接充当正式 BAND。
`timescale 1ns/1ps
`default_nettype none
module probe_band_ram #(
    parameter integer DEPTH = 2560,
    parameter integer PIXEL_MASK = 1,
    parameter integer SEGMENTED = 0
)(
    input wire clk,
    input wire [11:0] wa, ra,
    input wire [127:0] wd,
    input wire [3:0] we,
    input wire re,
    output wire [127:0] q
);
    generate if (SEGMENTED != 0) begin : segmented
        reg sel;
        always @(posedge clk) if(re) sel<=ra[11];
        if(PIXEL_MASK != 0) begin : masked
            genvar p;
            for(p=0;p<4;p=p+1) begin : lane
                reg [31:0] mem_large[0:2047];
                reg [31:0] mem_small[0:511];
                reg [31:0] qlarge,qsmall;
                always @(posedge clk) begin
                    if(we[p]) begin
                        if(wa[11]) mem_small[wa[8:0]]<=wd[32*p +:32];
                        else mem_large[wa[10:0]]<=wd[32*p +:32];
                    end
                    if(re) begin
                        if(ra[11]) qsmall<=mem_small[ra[8:0]];
                        else qlarge<=mem_large[ra[10:0]];
                    end
                end
                assign q[32*p +:32]=sel ? qsmall : qlarge;
            end
        end else begin : wide
            reg [127:0] mem_large[0:2047];
            reg [127:0] mem_small[0:511];
            reg [127:0] qlarge,qsmall;
            always @(posedge clk) begin
                if(|we) begin
                    if(wa[11]) mem_small[wa[8:0]]<=wd;
                    else mem_large[wa[10:0]]<=wd;
                end
                if(re) begin
                    if(ra[11]) qsmall<=mem_small[ra[8:0]];
                    else qlarge<=mem_large[ra[10:0]];
                end
            end
            assign q=sel ? qsmall : qlarge;
        end
    end else if (PIXEL_MASK != 0) begin : masked
        genvar p;
        for (p=0;p<4;p=p+1) begin : lane
            reg [31:0] mem [0:DEPTH-1];
            reg [31:0] rq;
            always @(posedge clk) begin
                if (we[p]) mem[wa] <= wd[32*p +: 32];
                if (re) rq <= mem[ra];
            end
            assign q[32*p +: 32] = rq;
        end
    end else begin : wide
        reg [127:0] mem [0:DEPTH-1];
        reg [127:0] rq;
        always @(posedge clk) begin
            if (|we) mem[wa] <= wd;
            if (re) rq <= mem[ra];
        end
        assign q = rq;
    end endgenerate
endmodule
`default_nettype wire
