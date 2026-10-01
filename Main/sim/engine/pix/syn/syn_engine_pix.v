// B4 formal RTL isolated synthesis wrapper. Every input is LFSR driven;
// every output affects obs so RAM and DSP logic cannot be pruned.
`timescale 1ns/1ps
`default_nettype none
module syn_engine_pix(input wire clk, input wire rst, output reg [127:0] obs);
    reg [127:0] lfsr;
    always @(posedge clk) begin
        if(rst) lfsr<=128'h0123456789ABCDEF_FEDCBA9876543210;
        else lfsr<={lfsr[126:0],lfsr[127]^lfsr[125]^lfsr[124]^lfsr[121]^obs[0]};
    end
    wire [127:0] data; wire [15:0] keep; wire valid,ready,sb_ready;
    wire [1:0] free_half;
    engine_pix u_path(
        .clk(clk),.rst(rst),.sb_row(lfsr[15:0]),
        .sb_npx({1'b0,lfsr[25:16]}|11'd1),.sb_beats(lfsr[33:26]),
        .sb_lead_dst(lfsr[35:34]),.sb_lead_src(lfsr[37:36]),
        .sb_op(lfsr[38]?8'd2:8'd1),.sb_eq({1'b0,lfsr[41:39]}),
        .sb_key_en(lfsr[42]),.sb_key(lfsr[74:43]),
        .sb_w4({lfsr[31:0]}),.sb_c(lfsr[82:75]),
        .sb_sfmt({2'b0,lfsr[84:83]}),.sb_read_dst(lfsr[85]),
        .sb_valid(lfsr[86]),.sb_ready(sb_ready),.row_free(free_half),
        .rs_tdata(lfsr^obs),.rs_tkeep(lfsr[15:0]),
        .rs_tlast(lfsr[87]),.rs_tvalid(lfsr[88]),.rs_tready(ready),
        .rs_buf(lfsr[90:89]),.ws_tdata(data),.ws_tkeep(keep),
        .ws_tvalid(valid),.ws_tready(lfsr[91]));
    always @(posedge clk) begin
        if(rst) obs<=0;
        else obs<=obs^data^{107'd0,keep,valid,ready,sb_ready,free_half};
    end
endmodule
`default_nettype wire
