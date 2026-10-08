// G1 正式 TEX 的单独综合：全部输入来自变量激励，全部输出参与观测。
`timescale 1ns/1ps
`default_nettype none
module syn_engine_tex(input wire clk, rst, output reg obs);
    reg [127:0] stim;
    wire [31:0] prdata;
    wire pready, pslverr, req_ready, stat_valid, busy;
    wire [127:0] data;
    wire valid, last;
    wire [1:0] buf_id, stat_buf;
    wire [3:0] stat_cidx;
    wire [7:0] beats = {1'b0,stim[75:69]} + 8'd1;
    always @(posedge clk) begin
        if (rst) stim <= 128'h19a563b7cfe;
        else stim <= {stim[126:0],stim[127]^stim[125]^stim[100]^stim[98]};
        obs <= ^{prdata,pready,pslverr,req_ready,data,valid,last,buf_id,
                 stat_valid,stat_cidx,stat_buf,busy};
    end
    engine_tex dut(.clk(clk), .rst(rst), .paddr(stim[13:0]), .psel(stim[14]),
        .penable(stim[15]), .pwrite(stim[16]), .pwdata(stim[48:17]),
        .prdata(prdata), .pready(pready), .pslverr(pslverr),
        .req_addr({stim[58:49],4'd0}), .req_bytes({beats,4'd0}),
        .req_buf(stim[60:59]), .req_cidx(stim[64:61]), .req_valid(stim[65]),
        .req_ready(req_ready), .rs_tdata(data), .rs_tvalid(valid), .rs_tlast(last),
        .rs_buf(buf_id), .stat_valid(stat_valid), .stat_ready(stim[66]),
        .stat_cidx(stat_cidx), .stat_buf(stat_buf), .busy(busy));
endmodule
`default_nettype wire
