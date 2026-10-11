// F5 构建期的包校验。标量谓词复用原命令层的 ec_fields/ec_struct_ok。
`timescale 1ns/1ps
`default_nettype none
module engine_packet_check(
    input wire clk,
    input wire [255:0] packet,
    input wire [31:0] src_base,src_dim,src_ctrl,dst_base,dst_dim,dst_ctrl,
    input wire [3:0] target,
    input wire [31:0] pb_base,pb_bytes,input_base,input_bytes,bg_base,bg_bytes,
    output reg [3:0] error
);
    wire [7:0] op=packet[7:0];
    wire [3:0] eq=packet[11:8];
    wire drawing=op==1 || op==2 || op==5;
    wire blit=op==2 || op==5;
    wire inst=op==5;
    wire fields_bad,src_ok,dst_ok;
    ec_fields u_f(.cmd(packet),.bad(fields_bad));
    ec_struct_ok u_s(.s0(src_base),.s1(src_dim),.s2(src_ctrl),.ok(src_ok));
    ec_struct_ok u_d(.s0(dst_base),.s1(dst_dim),.s2(dst_ctrl),.ok(dst_ok));
    wire [15:0] step=packet[175:160];
    wire [1:0] layout=packet[177:176];
    wire [24:0] length=packet[79:64]==0?25'd0:
        ({9'd0,packet[79:64]}-25'd1)*{1'b0,step[8:0]}+(layout==0?25'd4:25'd8);
    wire [32:0] array_bytes=({8'd0,length}+33'd15)&~33'd15;
    // 调用方在 CHECK_INPUT/CHECK_LATCH 期间持有同一包；实例地址范围单独打一拍。
    reg [32:0] array_end;
    always @(posedge clk)array_end<={1'b0,packet[63:32]}+array_bytes;
    wire inst_bad=packet[15:13]!=0 || packet[35:32]!=0 || packet[95:80]!=0 ||
        layout>1 || packet[191:178]!=0 || step<(layout==0?4:8) || step>256 || step[1:0]!=0 ||
        (layout==1 && packet[223:192]!=0) || (!packet[12] && packet[159:128]!=0) ||
        (eq!=3 && packet[31:24]!=0) || array_end>33'h100000000;
    wire [32:0] send={1'b0,src_base}+(({17'd0,src_dim[31:16]}-33'd1)<<src_ctrl[4:0])+{15'd0,src_dim[15:0],2'b0};
    wire [32:0] dend={1'b0,dst_base}+(({17'd0,dst_dim[31:16]}-33'd1)<<dst_ctrl[4:0])+{15'd0,dst_dim[15:0],2'b0};
    function overlap;
        input [32:0] a,ae,b,be;
        begin overlap=a<be && b<ae;end
    endfunction
    wire array_alias=inst && length!=0 && (
        overlap({1'b0,packet[63:32]},array_end,{1'b0,pb_base},{1'b0,pb_base}+{1'b0,pb_bytes}) ||
        overlap({1'b0,packet[63:32]},array_end,{1'b0,input_base},{1'b0,input_base}+{1'b0,input_bytes}) ||
        overlap({1'b0,packet[63:32]},array_end,{1'b0,dst_base},dend) ||
        overlap({1'b0,packet[63:32]},array_end,{1'b0,bg_base},{1'b0,bg_base}+{1'b0,bg_bytes}));
    wire source_alias=blit && src_ctrl[9:8]==0 && (
        overlap({1'b0,src_base},send,{1'b0,dst_base},dend) ||
        overlap({1'b0,src_base},send,{1'b0,pb_base},{1'b0,pb_base}+{1'b0,pb_bytes}));
    always @* begin
        error=0;
        if(!(op==0 || drawing || op==3 || op==255))error=1;
        else if(inst?inst_bad:fields_bad)error=2;
        else if(eq>5)error=3;
        else if((blit&&!src_ok)||(drawing&&!dst_ok))error=4;
        else if((blit&&src_ctrl[9:8]>1)||(drawing&&dst_ctrl[9:8]!=0))error=5;
        else if((drawing&&dst_ctrl[15:12]!=0) || (blit &&
            ((eq==1 && src_ctrl[15:12]>1)||(eq==5 && src_ctrl[15:12]!=0 && src_ctrl[15:12]!=2)||src_ctrl[15:12]>2)))error=6;
        else if(op==1 && eq==1 && (packet[151:144]>packet[159:152] ||
            packet[143:136]>packet[159:152] || packet[135:128]>packet[159:152]))error=8;
        else if(source_alias || array_alias)error=7;
        else if(drawing && packet[23:20]!=target)error=2;
    end
endmodule
`default_nettype wire
