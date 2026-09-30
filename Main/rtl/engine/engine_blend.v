// engine_blend：引擎 P1 B1 的单通道混合核
//
// 一个颜色通道的混合：源、目的、a、c 各 8 位与方程号进去，8 位出来。
// 六个方程（设计文档 3.5 节）都写成同一个整数恒等：
//
//     x   = base × 255 + m1 × m2          等价于  src × Fs + dst × Fd（255 满量程）
//     t   = x + 128
//     out = min(255, (t + (t >> 8)) >> 8)
//
//   eq  名称            Fs, Fd        base     m1        m2
//   0   COPY            255, 0        src      0         0
//   1   ALPHA 预乘      255, 255-a    src      dst       255-a
//   2   ADD             255, 255      src+dst  0         0
//   3   CONST           c, 255-c      dst      src-dst   c
//   4   MUL             dst, 0        0        src       dst
//   5   ALPHA 非预乘    a, 255-a      dst      src-dst   a
//
// base 9 位无符号（ADD 要 src+dst，上限 510）、m1 9 位有符号（CONST 与非预乘 ALPHA 要 src-dst）、
// m2 8 位无符号（进乘法器前零扩展成 9 位有符号，两个乘数同号才是有符号乘法）。
// 代入表与逐位等价在 Main/tools/check/check_subst.c 里对**全定义域**穷举过：
// 六方程共 50462976 组，与 src×Fs + dst×Fd 0 处不同；ALPHA 预乘不限定 src_pm <= a，
// 因为硬件会收到任意输入（BLIT 从表面读的源像素没有逐个检查）。
// eq 只有 0~5 会到这里（方程号 6~15 由控制层按 BAD_EQUATION 拒掉），case 的 default 按 5 算。
//
// 流水 3 级（D24，总览 2.4 节 I6 的 L = 3）：
//   级 1  判定（选 base/m1/m2）+ 乘 + base×255      -> r_p、r_c
//   级 2  x = r_p + r_c                             -> r_x
//   级 3  t = x + 128、除以 255、饱和到 255          -> out
// base×255 与乘积必须**同一拍寄存**：只寄存乘积会让第二级把两拍的操作数相加，
// 这是 2026-09-29 草案里真实出现过的错（42107137 个向量里 33618694 个不符）。
//
// en 是时钟使能（I6）：B4 的行缓冲没就绪时要能停住流水；不用时接 1'b1。
// en 为 0 时三级寄存器一起保持，停顿期间的输入不进流水。
// 五个数据输入（src/dst/a/c/eq）必须由寄存器直接驱动，核心前面不得有组合逻辑：
// 级 1 实测 5.280 ns、28 级逻辑，起点是寄存器，前面挂组合逻辑会把这一级的余量吃掉。
//
// 仿真注错口（0 是正式配置，综合时是常量、对应分支会被优化掉），
// 位定义与 sim/engine/blend/run.sh 的用例一一对应：
//   位 0  改错一个方程：CONST 用 base=src、m1=dst-src 代入（恒等不成立）
//   位 1  中间量模截断到 16 位：t 取 t & 0xFFFF（丢掉进位）
//   位 2  只寄存乘积，base×255 仍取当前输入（把两拍的操作数相加）
//   位 3  COPY 的 base 误取 src+dst（抄了 ADD 行），只有不用的输入会变时才看得出
//   位 4  级 2 的 r_x 不看 en，只有流水停顿时才看得出
//
// 设计依据与实测数据见 Engineering/TechReports/engine/01-计算核心.md。
// 单通道一份，12 个实例由 B4 的 engine_pix 例化。
`timescale 1ns/1ps
`default_nettype none

module engine_blend #(
    // 仿真注错口，见文件头；综合时是常量。
    parameter integer INJECT = 0
)(
    input  wire       clk,
    input  wire       en,       // 时钟使能，B4 不用时接 1'b1
    input  wire [7:0] src,      // 源通道
    input  wire [7:0] dst,      // 目的通道
    input  wire [7:0] a,        // 源 alpha（预乘/非预乘 ALPHA 用）
    input  wire [7:0] c,        // CONST 的常量系数
    input  wire [2:0] eq,       // 方程号，0~5
    output reg  [7:0] out       // 混合结果，固定延迟 3 拍
);

    localparam INJ_WRONG_EQ  = ((INJECT & 1) != 0);
    localparam INJ_TRUNC16   = ((INJECT & 2) != 0);
    localparam INJ_UNREG_C   = ((INJECT & 4) != 0);
    localparam INJ_COPY_DST  = ((INJECT & 8) != 0);
    localparam INJ_X_NO_EN   = ((INJECT & 16) != 0);

    // ---------------- 级 1：判定，按方程选 base / m1 / m2 ----------------
    reg  [8:0]        sel_base;
    reg  signed [8:0] sel_m1;
    reg  signed [8:0] sel_m2;

    always @(*) begin
        case (eq)
            3'd0: begin                                  // COPY     Fs=255 Fd=0
                if (INJ_COPY_DST)
                    sel_base = {1'b0, src} + {1'b0, dst};  // 注错：抄了 ADD 行，COPY 用到了 dst
                else
                    sel_base = {1'b0, src};
                sel_m1   = 9'sd0;
                sel_m2   = 9'sd0;
            end
            3'd1: begin                                  // ALPHA    Fs=255 Fd=255-a（源已预乘）
                sel_base = {1'b0, src};
                sel_m1   = $signed({1'b0, dst});
                sel_m2   = $signed({1'b0, (8'd255 - a)});
            end
            3'd2: begin                                  // ADD      Fs=255 Fd=255
                sel_base = {1'b0, src} + {1'b0, dst};
                sel_m1   = 9'sd0;
                sel_m2   = 9'sd0;
            end
            3'd3: begin                                  // CONST    Fs=c   Fd=255-c
                if (INJ_WRONG_EQ) begin
                    // 注错：base 与 m1 都取错，恒等不成立
                    sel_base = {1'b0, src};
                    sel_m1   = $signed({1'b0, dst}) - $signed({1'b0, src});
                end else begin
                    sel_base = {1'b0, dst};
                    sel_m1   = $signed({1'b0, src}) - $signed({1'b0, dst});
                end
                sel_m2   = $signed({1'b0, c});
            end
            3'd4: begin                                  // MUL      Fs=dst Fd=0
                sel_base = 9'd0;
                sel_m1   = $signed({1'b0, src});
                sel_m2   = $signed({1'b0, dst});
            end
            default: begin                               // ALPHA 非预乘  Fs=a Fd=255-a
                sel_base = {1'b0, dst};
                sel_m1   = $signed({1'b0, src}) - $signed({1'b0, dst});
                sel_m2   = $signed({1'b0, a});
            end
        endcase
    end

    // 乘积：9 位有符号 × 9 位有符号，值域 ±65025，17 位放得下（按 18 位寄存）
    wire signed [17:0] p_comb = sel_m1 * sel_m2;

    // base × 255：移位相减，不用乘法器；上限 510 × 255 = 130050，17 位放得下
    wire [17:0] base_ext  = {9'd0, sel_base};
    wire [17:0] c255_comb = (base_ext << 8) - base_ext;

    // ---------------- 级 1 的寄存器 ----------------
    reg signed [17:0] r_p;
    reg        [17:0] r_c;

    always @(posedge clk) begin
        if (en) begin
            r_p <= p_comb;
            r_c <= c255_comb;
        end
    end

    // ---------------- 级 2：x = 乘积 + base×255 ----------------
    // c255 恒正（<= 130050 < 2^17，第 17 位为 0），按 18 位有符号给进 DSP 的 C 口。
    wire signed [17:0] c_stage = INJ_UNREG_C ? $signed(c255_comb) : $signed(r_c);
    wire signed [18:0] x_comb  = r_p + c_stage;          // 值域 0 .. 130050

    reg signed [18:0] r_x;

    always @(posedge clk) begin
        if (en || INJ_X_NO_EN) r_x <= x_comb;          // 注错：级 2 不看 en
    end

    // ---------------- 级 3：t = x + 128、除以 255、饱和 ----------------
    wire [17:0] xv     = r_x[17:0];                      // 17 位无符号够（2^17 = 131072）
    wire [17:0] t_full = xv + 18'd128;                   // 上限 130178，18 位
    wire [17:0] t      = INJ_TRUNC16 ? {2'b00, t_full[15:0]} : t_full;

    // t + (t >> 8) 上限 130686，数学上 18 位就够；按 19 位写，免得综合报
    // expression size 19 truncated to fit in target size 18
    wire [18:0] qs = {1'b0, t} + {11'd0, t[17:8]};
    wire [9:0]  q  = qs[17:8];                           // 上限 510

    wire [7:0] out_comb = (q > 10'd255) ? 8'd255 : q[7:0];

    always @(posedge clk) begin
        if (en) out <= out_comb;
    end

endmodule

`default_nettype wire
