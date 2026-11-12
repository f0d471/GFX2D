`timescale 1ns/1ps
//==============================================================================================//
// FILE: engine_blend.v
//
// DESCRIPTION: 单通道混合核：六个方程写成同一个整数恒等 base×255 + m1×m2，
//   三级流水（判定+乘、求和、除 255 饱和）。方程表与注错开关见 engine-manual.md。
//
// NOTE:
//   1. base×255 与乘积必须同一拍寄存，只寄存乘积会让级 2 相加两拍的操作数
//   2. 五个数据输入必须由寄存器直接驱动（级 1 是关键路径起点，前面不得挂组合逻辑）
//   3. en 为 0 三级一起保持；eq>5 已被控制层拒绝，case 的 default 按 5 算
//==============================================================================================//
`default_nettype none
`include "engine_feature.vh"

module engine_blend (
    input  wire       clk,
    input  wire       en,       // 时钟使能
    input  wire [7:0] src,      // 源通道
    input  wire [7:0] dst,      // 目的通道
    input  wire [7:0] a,        // 源 alpha
    input  wire [7:0] c,        // CONST 的常量系数
    input  wire [2:0] eq,       // 方程号，0~5
    output reg  [7:0] out       // 混合结果
);

    // 级 1：判定，按方程选 base / m1 / m2
    reg         [8:0] sel_base;
    reg  signed [8:0] sel_m1;
    reg  signed [8:0] sel_m2;

    always @(*) begin
        case (eq)
            3'd0: begin // COPY     Fs=255 Fd=0

`ifdef INJ_COPY_DST // 注错：抄了 ADD 行，COPY 用到了 dst
                sel_base = {1'b0, src} + {1'b0, dst};
`else
                sel_base = {1'b0, src};
`endif

                sel_m1   = 9'sd0;
                sel_m2   = 9'sd0;
            end
`ifdef GFX_ALPHA
            3'd1: begin //  ALPHA    Fs=255 Fd=255-a
                sel_base = {1'b0, src};
                sel_m1   = $signed({1'b0, dst});
                sel_m2   = $signed({1'b0, (8'd255 - a)});
            end
`endif
            3'd2: begin // ADD      Fs=255 Fd=255
                sel_base = {1'b0, src} + {1'b0, dst};
                sel_m1   = 9'sd0;
                sel_m2   = 9'sd0;
            end
`ifdef GFX_EQ_EXT
            3'd3: begin // CONST    Fs=c   Fd=255-c

`ifdef INJ_WRONG_EQ // 注错：base 与 m1 都取错，恒等不成立
                sel_base = {1'b0, src};
                sel_m1   = $signed({1'b0, dst}) - $signed({1'b0, src});
`else
                sel_base = {1'b0, dst};
                sel_m1   = $signed({1'b0, src}) - $signed({1'b0, dst});
`endif

                sel_m2   = $signed({1'b0, c});
            end
            3'd4: begin // MUL      Fs=dst Fd=0
                sel_base = 9'd0;
                sel_m1   = $signed({1'b0, src});
                sel_m2   = $signed({1'b0, dst});
            end
`endif
`ifdef GFX_ALPHA
            default: begin // ALPHA 非预乘  Fs=a Fd=255-a
                sel_base = {1'b0, dst};
                sel_m1   = $signed({1'b0, src}) - $signed({1'b0, dst});
                sel_m2   = $signed({1'b0, a});
            end
`else
            // 无 ALPHA：未使能的方程号按 COPY 处理，避免出现组合环或不定值
            default: begin
                sel_base = {1'b0, src};
                sel_m1   = 9'sd0;
                sel_m2   = 9'sd0;
            end
`endif
        endcase
    end

    wire signed [17:0] p_comb = sel_m1 * sel_m2;

    // base × 255
    wire [17:0] base_ext  = {9'd0, sel_base};
    wire [17:0] c255_comb = (base_ext << 8) - base_ext;

    // 级 1 寄存器
    reg signed [17:0] r_p;
    reg        [17:0] r_c;

    always @(posedge clk) begin
        if (en) begin
            r_p <= p_comb;
            r_c <= c255_comb;
        end
    end

    // 级 2：x = 乘积 + base×255

`ifdef INJ_UNREG_C // 注错：base×255 不取寄存值，级 2 会把两拍的操作数相加
    wire signed [17:0] c_stage = $signed(c255_comb);
`else
    wire signed [17:0] c_stage = $signed(r_c);  // c255 恒正（<= 130050 < 2^17，第 17 位为 0），按 18 位有符号给进 DSP 的 C 口。
`endif

    wire signed [18:0] x_comb  = r_p + c_stage;

    reg signed [18:0] r_x;

    always @(posedge clk) begin

`ifdef INJ_X_NO_EN // 注错：级 2 不看 en
        r_x <= x_comb;
`else
        if (en) r_x <= x_comb;
`endif

    end

    // 级 3：t = x + 128、除以 255、饱和
    wire [17:0] xv     = r_x[17:0];
    wire [17:0] t_full = xv + 18'd128; // 四舍五入

`ifdef INJ_TRUNC16 // 注错：模截断到 16 位，丢掉进位
    wire [17:0] t      = {2'b00, t_full[15:0]};
`else
    wire [17:0] t      = t_full;
`endif

    wire [18:0] qs = {1'b0, t} + {11'd0, t[17:8]};
    wire [9:0]  q  = qs[17:8];

    wire [7:0] out_comb = (q > 10'd255) ? 8'd255 : q[7:0];

    always @(posedge clk) begin
        if (en) out <= out_comb;
    end

endmodule

`default_nettype wire
