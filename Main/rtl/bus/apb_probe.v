`timescale 1ns/1ps
//==============================================================================================//
// FILE: apb_probe.v
//
// DESCRIPTION: APB 探针寄存器（ID/VERSION/SCRATCH），上板验证 APB 译码与数据通路；
//   引擎接入后已从顶层撤下，源文件保留。寄存器表见 bus-manual.md。
//
// NOTE:
//   1. 16 位地址全部参与译码；写只读寄存器也回 PSLVERROR 且寄存器不变
//   2. PREADY 恒 1，读数据在 access 阶段组合输出
//==============================================================================================//
`default_nettype none

module apb_probe #(
    parameter [31:0] ID      = 32'h4746_5832,
    parameter [31:0] VERSION = 32'h0000_0001
)(
    input  wire        clk,
    input  wire        rst,        // 同步复位，高有效
    input  wire [15:0] paddr,
    input  wire        psel,
    input  wire        penable,
    input  wire        pwrite,
    input  wire [31:0] pwdata,
    output reg  [31:0] prdata,
    output wire        pready,
    output wire        pslverr
);

localparam [15:0] ADDR_ID      = 16'h0000;
localparam [15:0] ADDR_VERSION = 16'h0004;
localparam [15:0] ADDR_SCRATCH = 16'h0008;

reg  [31:0] scratch;
reg         hit;

wire access   = psel & penable;
wire wr_legal = (paddr == ADDR_SCRATCH);

always @(*) begin
    hit = 1'b1;
    case (paddr)
        ADDR_ID:      prdata = ID;
        ADDR_VERSION: prdata = VERSION;
        ADDR_SCRATCH: prdata = scratch;
        default: begin
            prdata = 32'h0;
            hit    = 1'b0;
        end
    endcase
end

assign pready  = 1'b1;
assign pslverr = access & (pwrite ? ~wr_legal : ~hit);

always @(posedge clk) begin
    if (rst)
        scratch <= 32'h0;
    else if (access & pwrite & wr_legal)
        scratch <= pwdata;
end

endmodule

`default_nettype wire
