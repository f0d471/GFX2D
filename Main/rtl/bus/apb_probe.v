// APB 探针寄存器：挂在 SoC 的 APB Slave 0（CPU 地址 0xF810_0000，64KB 窗口）
//
// 用途：上板验证 APB 的 16 位地址译码与 32 位数据通路；加速器寄存器接入之前在这里占位。
//
//   偏移    名称     读写  内容
//   0x0000  ID       只读  32'h4746_5832（ASCII "GFX2"）
//   0x0004  VERSION  只读  32'h0000_0001
//   0x0008  SCRATCH  读写  复位值 0，写什么读回什么
//   其余              读回 0；读写都置 PSLVERROR
//
// 16 位地址全部参与译码，高位不同的地址不会命中寄存器。写只读寄存器也置 PSLVERROR，寄存器不变。
// PREADY 恒为 1，读数据在 access 阶段组合输出。
`timescale 1ns/1ps
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
