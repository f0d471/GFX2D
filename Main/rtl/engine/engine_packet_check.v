`timescale 1ns/1ps
//==============================================================================================//
// FILE: engine_packet_check.v
//
// DESCRIPTION: PB 构建期的单包预检：操作码、字段、表面结构、能力布局与地址别名
//   逐类检查，输出一个错误码；标量谓词复用命令层的 ec_fields 与 ec_struct_ok。
//
// NOTE:
//   1. 调用方在 CHECK_INPUT/CHECK_LATCH 期间持有同一包，实例地址范围单独打一拍
//   2. 错误码语义见同目录 engine-manual.md
//==============================================================================================//
`default_nettype none
`include "engine_feature.vh"

module engine_packet_check (
    input  wire         clk,
    input  wire [255:0] packet,
    input  wire [31:0]  src_base,
    input  wire [31:0]  src_dim,
    input  wire [31:0]  src_ctrl,
    input  wire [31:0]  dst_base,
    input  wire [31:0]  dst_dim,
    input  wire [31:0]  dst_ctrl,
    input  wire [3:0]   target,
    input  wire [31:0]  pb_base,
    input  wire [31:0]  pb_bytes,
    input  wire [31:0]  input_base,
    input  wire [31:0]  input_bytes,
    input  wire [31:0]  bg_base,
    input  wire [31:0]  bg_bytes,
    output reg [3:0]    error
);

    wire [7:0] op      = packet[7:0];
    wire [3:0] eq      = packet[11:8];
    wire       drawing = op == 1 || op == 2 || op == 5;
    wire       blit    = op == 2 || op == 5;
    wire       inst    = op == 5;

    // 标量字段谓词与表面结构谓词，与命令层同源
    wire fields_bad, src_ok, dst_ok;
    ec_fields u_f(.cmd(packet), .bad(fields_bad));
    ec_struct_ok u_s(.s0(src_base), .s1(src_dim), .s2(src_ctrl), .ok(src_ok));
    ec_struct_ok u_d(.s0(dst_base), .s1(dst_dim), .s2(dst_ctrl), .ok(dst_ok));

    // 实例数组的字节长度与 16 B 对齐的末端
    wire [15:0] step   = packet[175:160];
    wire [1:0]  layout = packet[177:176];
    wire [24:0] length = packet[79:64] == 0 ? 25'd0 :
        ({9'd0, packet[79:64]} - 25'd1) * {1'b0, step[8:0]} + (layout == 0 ? 25'd4 : 25'd8);
    wire [32:0] array_bytes = ({8'd0, length} + 33'd15) & ~33'd15;

    // 调用方在 CHECK_INPUT/CHECK_LATCH 期间持有同一包；实例地址范围单独打一拍。
    reg [32:0] array_end;
    always @(posedge clk) array_end <= {1'b0, packet[63:32]} + array_bytes;

    // 实例包专用字段：布局、步长、保留位必须全部合法
    wire inst_bad = packet[15:13] != 0 || packet[35:32] != 0 || packet[95:80] != 0 ||
        layout > 1 || packet[191:178] != 0 || step < (layout == 0 ? 4 : 8) || step > 256 || step[1:0] != 0 ||
        (layout == 1 && packet[223:192] != 0) || (!packet[12] && packet[159:128] != 0) ||
        (eq != 3 && packet[31:24] != 0) || array_end > 33'h100000000;

    // 源/目的表面的末字节地址
    wire [32:0] send = {1'b0, src_base} + (({17'd0, src_dim[31:16]} - 33'd1) << src_ctrl[4:0]) + {15'd0, src_dim[15:0], 2'b0};
    wire [32:0] dend = {1'b0, dst_base} + (({17'd0, dst_dim[31:16]} - 33'd1) << dst_ctrl[4:0]) + {15'd0, dst_dim[15:0], 2'b0};

    function overlap;
        input [32:0] a, ae, b, be;
        begin overlap = a < be && b < ae; end
    endfunction

    // 实例数组不得与 PB、输入、目的、背景重叠
    wire array_alias = inst && length != 0 && (
        overlap({1'b0, packet[63:32]}, array_end, {1'b0, pb_base},    {1'b0, pb_base} + {1'b0, pb_bytes}) ||
        overlap({1'b0, packet[63:32]}, array_end, {1'b0, input_base}, {1'b0, input_base} + {1'b0, input_bytes}) ||
        overlap({1'b0, packet[63:32]}, array_end, {1'b0, dst_base},   dend) ||
        overlap({1'b0, packet[63:32]}, array_end, {1'b0, bg_base},    {1'b0, bg_base} + {1'b0, bg_bytes}));

    // 平面源与目的/PB 重叠拒绝；槽内反馈沿命令层语义
    wire source_alias = blit && src_ctrl[9:8] == 0 && (
        overlap({1'b0, src_base}, send, {1'b0, dst_base}, dend) ||
        overlap({1'b0, src_base}, send, {1'b0, pb_base},  {1'b0, pb_base} + {1'b0, pb_bytes}));

    always @* begin
        error = 0;
        if (!(op == 0 || drawing || op == 3 || op == 255)) error = 1;
        else if (inst ? inst_bad : fields_bad) error = 2;
        else if (eq > 5) error = 3;
        else if ((blit && !src_ok) || (drawing && !dst_ok)) error = 4;
        else if ((blit && src_ctrl[9:8] > 1) || (drawing && dst_ctrl[9:8] != 0)) error = 5;
        else if ((drawing && dst_ctrl[15:12] != 0) || (blit &&
            ((eq == 1 && src_ctrl[15:12] > 1) || (eq == 5 && src_ctrl[15:12] != 0 && src_ctrl[15:12] != 2) || src_ctrl[15:12] > 2))) error = 6;
        else if (op == 1 && eq == 1 && (packet[151:144] > packet[159:152] ||
            packet[143:136] > packet[159:152] || packet[135:128] > packet[159:152])) error = 8;
        else if (source_alias || array_alias) error = 7;
        else if (drawing && packet[23:20] != target) error = 2;
    end

endmodule
`default_nettype wire
