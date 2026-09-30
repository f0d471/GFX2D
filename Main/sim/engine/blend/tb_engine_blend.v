// engine_blend 测试台（引擎 P1 B1：单通道混合核）
//
// 判据（Engineering/TechReports/engine/01-计算核心.md 3.6 节与第五节）：
//   1. 六个方程、每通道**全定义域**穷举，与被测逐值相同。期望值在本测试台里用**精确商**
//      round(x/255) = (2x+255)/510 现算、输入按设计文档 3.5 节的 Fs/Fd 直接代入，
//      不使用被测的那条快速式，两个实现互相独立。
//      ALPHA 预乘跑满 2^24（不限定 src_pm <= a）：硬件会收到任意输入，
//      软件执行器 exec_sw.c 对任意输入都按同一条规则算，见 engine 第 01 章 4.1 节。
//      方程用不到的输入（例如 COPY 的 dst、a、c）不固定成 0，而是跟着循环变量变，
//      被测的输出必须与它们无关；否则「COPY 误用了 dst」这类抄错行的错会被 0 掩盖。
//   2. 抽样与 check_round255.c 的输出对一次（-DGOLDEN，读 build/golden.hex）。
//   3. 停顿（-DSTALL）：随机插入 en 拉低的拍，停顿期间输入换成乱值，
//      流水线与期望值的延迟链一起停住，恢复后逐值仍须相同。
//   4. 注错必须见红（-DINJECT=n，配 -DFAILFAST）：
//        INJECT=1   改错一个方程（CONST）
//        INJECT=2   中间量模截断到 16 位
//        INJECT=4   乘之后只寄存乘积，base×255 仍取当前输入
//        INJECT=8   COPY 误用 dst（抄了 ADD 行），只有不用的输入会变时才看得出
//        INJECT=16  级 2 的 r_x 不看 en，只有停顿时才看得出
//
// 驱动方式：每拍换一个向量，输入的延迟链给出同一拍的期望值，比较在 negedge 做，
// 因此总周期数约等于向量数，全量穷举（50462976 个向量）才跑得动。
//
// 开关：
//   -DONLY_EQ=n   只跑方程 n（0~5），注错用例用它把时间压下来
//   -DFAILFAST    第一处不符就收尾
//   -DSTRIDE=n    抽样步长（默认 1 = 全量）；停顿用例用 5，判据的穷举要跑默认值
//   -DSTALL       随机插入停顿拍（约 1/8 的拍）
//   -DGOLDEN      改跑 build/golden.hex 的抽样对账
//   -DINJECT=n    传给被测的注错口
`timescale 1ns/1ps
`default_nettype none

module tb_engine_blend;

    localparam integer L        = 3;          // engine_blend 的固定延迟（D24）
    localparam integer GOLDEN_N = 1024;       // check_round255.c --golden 的条数

`ifdef ONLY_EQ
    localparam integer ONLY_EQ = `ONLY_EQ;
`else
    localparam integer ONLY_EQ = -1;          // -1 = 六个方程都跑
`endif

`ifdef INJECT
    localparam integer INJECT_VAL = `INJECT;
`else
    localparam integer INJECT_VAL = 0;
`endif

`ifdef STRIDE
    localparam integer STRIDE = `STRIDE;
`else
    localparam integer STRIDE = 1;
`endif

    // 全量穷举的向量数，用来确认输入域一个都没漏
    localparam integer NVEC_ALL = 256 + 16777216 + 65536 + 16777216 + 65536 + 16777216;

    reg clk = 1'b0;
    always #5 clk = ~clk;

    // eq 的初值故意取 7（不是任何方程号）：iverilog 的 always @(*) 在 0 时刻不会自己求值，
    // 要等敏感表里有信号变化才第一次求值。若第一个向量的输入与初值完全相同，
    // 组合块一次都不会被触发，流水线会一直带 X。取 7 保证第一次送向量时 eq 一定变化。
    reg  [7:0] src = 8'd0, dst = 8'd0, a = 8'd0, c = 8'd0;
    reg  [2:0] eq  = 3'd7;
    reg        en  = 1'b1;
    wire [7:0] out;

    engine_blend #(.INJECT(INJECT_VAL)) dut (
        .clk (clk),
        .en  (en),
        .src (src),
        .dst (dst),
        .a   (a),
        .c   (c),
        .eq  (eq),
        .out (out)
    );

    // 输入端与金标准的延迟链：ds[L-1] 与 out 在同一拍对应同一个向量。
    // 链与被测用同一个 en，停顿时一起停住。
    reg [7:0] ds [0:L-1];
    reg [7:0] dd [0:L-1];
    reg [7:0] da [0:L-1];
    reg [7:0] dc [0:L-1];
    reg [2:0] de [0:L-1];
    reg [7:0] dg [0:L-1];
    reg [7:0] g_want = 8'd0;
    integer   si;

    always @(posedge clk) begin
        if (en) begin
            ds[0] <= src; dd[0] <= dst; da[0] <= a; dc[0] <= c; de[0] <= eq;
            dg[0] <= g_want;
            for (si = 1; si < L; si = si + 1) begin
                ds[si] <= ds[si-1]; dd[si] <= dd[si-1]; da[si] <= da[si-1];
                dc[si] <= dc[si-1]; de[si] <= de[si-1];
                dg[si] <= dg[si-1];
            end
        end
    end

    // 期望值：精确商 + 饱和，输入按 Fs/Fd 直接代入（不用被测的快速式）
    function [7:0] expected;
        input [7:0] s, d, aa, cc;
        input [2:0] e;
        reg [31:0] x, q;
        begin
            case (e)
                3'd0:    x = s * 255;
                3'd1:    x = s * 255 + d * (255 - aa);
                3'd2:    x = s * 255 + d * 255;
                3'd3:    x = s * cc + d * (255 - cc);
                3'd4:    x = s * d;
                default: x = s * aa + d * (255 - aa);
            endcase
            q = (2 * x + 255) / 510;
            if (q > 32'd255) q = 32'd255;
            expected = q[7:0];
        end
    endfunction

    integer errors  = 0;
    integer checks  = 0;
    integer skipped = 0;
    integer nvec    = 0;
    integer stalls  = 0;
    integer stall_seed = 32'h2026_0930;
    reg [7:0] expv;

    // 送一个向量，并检查 L 拍之前那一个
    task step;
        input [7:0] s, d, aa, cc;
        input [2:0] e;
        input [7:0] want;
        begin
            @(negedge clk);
`ifdef STALL
            // 约 1/8 的拍拉低 en，输入换成乱值；流水必须原样停住，乱值不得进流水
            while (($random(stall_seed) & 7) == 0) begin
                en  = 1'b0;
                src = $random(stall_seed); dst = $random(stall_seed);
                a   = $random(stall_seed); c   = $random(stall_seed);
                eq  = $random(stall_seed);
                stalls = stalls + 1;
                @(negedge clk);
            end
            en = 1'b1;
`endif
            if (skipped < L) begin
                skipped = skipped + 1;
            end else begin
`ifdef GOLDEN
                expv = dg[L-1];
`else
                expv = expected(ds[L-1], dd[L-1], da[L-1], dc[L-1], de[L-1]);
`endif
                if (out !== expv) begin
                    if (errors < 10)
                        $display("  MISMATCH #%0d: eq=%0d src=%0d dst=%0d a=%0d c=%0d  got=%0d want=%0d",
                                 nvec, de[L-1], ds[L-1], dd[L-1], da[L-1], dc[L-1], out, expv);
                    errors = errors + 1;
`ifdef FAILFAST
                    $display("检查 %0d 个向量，不一致 %0d 处", checks, errors);
                    $display("RESULT: FAIL");
                    $finish;
`endif
                end
`ifdef DBG
                if (checks < 8)
                    $display("  DBG check#%0d: t=%0t src=%0d dst=%0d a=%0d c=%0d eq=%0d got=%0d want=%0d | rp=%0d rc=%0d rx=%0d",
                             checks, $time, ds[L-1], dd[L-1], da[L-1], dc[L-1], de[L-1], out, expv,
                             dut.r_p, dut.r_c, dut.r_x);
`endif
                checks = checks + 1;
                nvec   = nvec + 1;
            end
            src = s; dst = d; a = aa; c = cc; eq = e;
            g_want = want;
        end
    endtask

`ifdef GOLDEN
    reg [42:0] gold [0:GOLDEN_N-1];

    task golden_run;
        integer g;
        begin
            $readmemh("build/golden.hex", gold);
            for (g = 0; g < GOLDEN_N; g = g + 1)
                step(gold[g][39:32], gold[g][31:24], gold[g][23:16], gold[g][15:8],
                     gold[g][42:40], gold[g][7:0]);
        end
    endtask
`endif

    integer ii, jj, kk, fi;

    initial begin
        $display("== tb_engine_blend INJECT=%0d ONLY_EQ=%0d STRIDE=%0d ==",
                 INJECT_VAL, ONLY_EQ, STRIDE);

        for (si = 0; si < L; si = si + 1) begin
            ds[si] = 8'd0; dd[si] = 8'd0; da[si] = 8'd0; dc[si] = 8'd0; de[si] = 3'd0;
            dg[si] = 8'd0;
        end

`ifdef GOLDEN
        golden_run;
`else
        // 方程用不到的输入跟着循环变量变（取值覆盖 0~255），不固定成 0；
        // expected() 不看它们，被测的输出也必须与它们无关。

        // ---- 方程 0  COPY：Fs=255 Fd=0；dst、a、c 用不到 ----
        if (ONLY_EQ == -1 || ONLY_EQ == 0)
            for (ii = 0; ii <= 255; ii = ii + STRIDE)
                step(ii[7:0], ii ^ 8'hA5, ii + 8'd77, ~ii, 3'd0, 8'd0);

        // ---- 方程 1  ALPHA 预乘：Fs=255 Fd=255-a，跑满 2^24；c 用不到 ----
        if (ONLY_EQ == -1 || ONLY_EQ == 1)
            for (ii = 0; ii <= 255; ii = ii + STRIDE)          // a
                for (jj = 0; jj <= 255; jj = jj + STRIDE)      // src（= src_pm）
                    for (kk = 0; kk <= 255; kk = kk + STRIDE)  // dst
                        step(jj[7:0], kk[7:0], ii[7:0], jj ^ kk, 3'd1, 8'd0);

        // ---- 方程 2  ADD：Fs=255 Fd=255；a、c 用不到 ----
        if (ONLY_EQ == -1 || ONLY_EQ == 2)
            for (ii = 0; ii <= 255; ii = ii + STRIDE)
                for (jj = 0; jj <= 255; jj = jj + STRIDE)
                    step(ii[7:0], jj[7:0], ii ^ jj, ii + jj, 3'd2, 8'd0);

        // ---- 方程 3  CONST：Fs=c Fd=255-c；a 用不到 ----
        if (ONLY_EQ == -1 || ONLY_EQ == 3)
            for (ii = 0; ii <= 255; ii = ii + STRIDE)          // c
                for (jj = 0; jj <= 255; jj = jj + STRIDE)      // src
                    for (kk = 0; kk <= 255; kk = kk + STRIDE)  // dst
                        step(jj[7:0], kk[7:0], jj ^ kk, ii[7:0], 3'd3, 8'd0);

        // ---- 方程 4  MUL：Fs=dst Fd=0；a、c 用不到 ----
        if (ONLY_EQ == -1 || ONLY_EQ == 4)
            for (ii = 0; ii <= 255; ii = ii + STRIDE)
                for (jj = 0; jj <= 255; jj = jj + STRIDE)
                    step(ii[7:0], jj[7:0], ii ^ jj, ii + jj, 3'd4, 8'd0);

        // ---- 方程 5  ALPHA 非预乘：Fs=a Fd=255-a；c 用不到 ----
        if (ONLY_EQ == -1 || ONLY_EQ == 5)
            for (ii = 0; ii <= 255; ii = ii + STRIDE)          // a
                for (jj = 0; jj <= 255; jj = jj + STRIDE)      // src
                    for (kk = 0; kk <= 255; kk = kk + STRIDE)  // dst
                        step(jj[7:0], kk[7:0], ii[7:0], jj ^ kk, 3'd5, 8'd0);
`endif

        // 收尾：把还留在流水线里的最后 L 个向量也检查掉
        for (fi = 0; fi < L; fi = fi + 1)
            step(8'd0, 8'd0, 8'd0, 8'd0, 3'd0, 8'd0);

        $display("检查 %0d 个向量，不一致 %0d 处", checks, errors);

`ifdef STALL
        // 一拍都没停过，停顿用例就什么也没验
        $display("  停顿拍 %0d 个", stalls);
        if (stalls == 0) begin
            $display("  停顿用例没有插入任何停顿拍");
            errors = errors + 1;
        end
`endif

`ifndef GOLDEN
        if (ONLY_EQ == -1 && STRIDE == 1) begin
            if (checks == NVEC_ALL)
                $display("  向量数与六个方程全定义域一致（%0d）", NVEC_ALL);
            else begin
                $display("  向量数 %0d 与全定义域 %0d 不符", checks, NVEC_ALL);
                errors = errors + 1;
            end
        end
`endif

        if (errors == 0) $display("RESULT: PASS");
        else             $display("RESULT: FAIL");
        $finish;
    end

endmodule

`default_nettype wire
