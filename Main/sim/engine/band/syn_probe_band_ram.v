// 隔离 RAM 综合：全部地址/使能/数据来自变量激励，全部输出参与观测。
`timescale 1ns/1ps
`default_nettype none
`ifndef BAND_DEPTH
`define BAND_DEPTH 2560
`endif
`ifndef BAND_MASK
`define BAND_MASK 1
`endif
`ifndef BAND_SEGMENTED
`define BAND_SEGMENTED 0
`endif
module syn_probe_band_ram(input wire clk,rst,output reg obs);
    reg [127:0] stim;
    wire [127:0] q;
    wire [11:0] wa=stim[11:0],ra=stim[23:12];
    wire [3:0] we=(wa < `BAND_DEPTH) ? stim[27:24] : 4'd0;
    wire re=stim[28] && (ra < `BAND_DEPTH);
    always @(posedge clk) begin
        if(rst) stim<=128'h19a563b7cfe001102233445566778899;
        else stim<={stim[126:0],stim[127]^stim[125]^stim[100]^stim[98]};
        obs<=^q;
    end
    probe_band_ram #(.DEPTH(`BAND_DEPTH),.PIXEL_MASK(`BAND_MASK),.SEGMENTED(`BAND_SEGMENTED)) dut(
        .clk(clk),.wa(wa),.ra(ra),.wd(stim),.we(we),.re(re),.q(q));
endmodule
`default_nettype wire
