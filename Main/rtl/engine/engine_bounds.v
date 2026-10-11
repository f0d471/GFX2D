`timescale 1ns/1ps
//==============================================================================================//
// FILE: engine_bounds.v
//
// DESCRIPTION: 全帧坐标到条带区间的求交：目的矩形、屏幕、clip、目的表面与源有效区
//   五者在 18 位有符号端点上求交，输出空标志与首/末条带号。
//
// NOTE:
//   1. 端点全程 18 位有符号，不能先截回 i16（65535 宽高会溢出）
//   2. op 为 BLIT/BLIT_INST（2/5）时源有效区按 (dx-sx, dy-sy) 反推后参与求交
//==============================================================================================//
`default_nettype none

module engine_bounds #(parameter integer STRIP_H = 16) (
    input  wire [255:0] packet,
    input  wire [31:0]  xy,
    input  wire [31:0]  uv,
    input  wire [63:0]  clip,
    input  wire [31:0]  src_dim,
    input  wire [31:0]  dst_dim,
    output reg          empty,
    output reg [5:0]    first_strip,
    output reg [5:0]    last_strip
);

    // 全帧坐标与求交端点，统一扩到 18 位有符号
    reg signed [17:0] dx, dy, sx, sy, x0, y0, x1, y1, a;

    always @* begin
        dx = $signed({{2{xy[15]}}, xy[15:0]});
        dy = $signed({{2{xy[31]}}, xy[31:16]});
        sx = $signed({{2{uv[15]}}, uv[15:0]});
        sy = $signed({{2{uv[31]}}, uv[31:16]});

        // 目的矩形 = (dx,dy) 起的 w×h，先夹到屏幕
        x0 = dx;
        y0 = dy;
        x1 = dx + $signed({2'b0, packet[111:96]}) - 18'sd1;
        y1 = dy + $signed({2'b0, packet[127:112]}) - 18'sd1;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > 639) x1 = 639;
        if (y1 > 479) y1 = 479;

        // clip 四端点（各 16 位有符号）与目的表面边界
        a = $signed({{2{clip[15]}}, clip[15:0]});
        if (x0 < a) x0 = a;
        a = $signed({{2{clip[31]}}, clip[31:16]});
        if (y0 < a) y0 = a;
        a = $signed({{2{clip[47]}}, clip[47:32]});
        if (x1 > a) x1 = a;
        a = $signed({{2{clip[63]}}, clip[63:48]});
        if (y1 > a) y1 = a;
        a = $signed({2'b0, dst_dim[15:0]}) - 18'sd1;
        if (x1 > a) x1 = a;
        a = $signed({2'b0, dst_dim[31:16]}) - 18'sd1;
        if (y1 > a) y1 = a;

        // BLIT 源有效区：(dx-sx, dy-sy) 起的源尺寸，同样求交
        if (packet[7:0] == 2 || packet[7:0] == 5) begin
            a = dx - sx;
            if (x0 < a) x0 = a;
            a = dy - sy;
            if (y0 < a) y0 = a;
            a = dx - sx + $signed({2'b0, src_dim[15:0]}) - 18'sd1;
            if (x1 > a) x1 = a;
            a = dy - sy + $signed({2'b0, src_dim[31:16]}) - 18'sd1;
            if (y1 > a) y1 = a;
        end

        // 空 = 零宽高或交出空矩形；条带号 = y 端点整除 STRIP_H
        empty = packet[111:96] == 0 || packet[127:112] == 0 || x0 > x1 || y0 > y1;
        first_strip = empty ? 6'd0 : y0[$clog2(STRIP_H)+5 : $clog2(STRIP_H)];
        last_strip  = empty ? 6'd0 : y1[$clog2(STRIP_H)+5 : $clog2(STRIP_H)];
    end

endmodule
`default_nettype wire
