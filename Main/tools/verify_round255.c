/* verify_round255.c —— 独立复核：把候选模型分开，逐项断言
 *
 * 用途与 check_round255.c 不同。正式工具按方程表的 Fs、Fd 机械代入；
 * 本程序刻意把**容易混淆的几个候选模型**并列摆出来，各自的期望值写成断言，
 * 用来回答「哪一个模型才是定稿的那一个」以及「搞错了会差多少」。
 *
 * 2026-09-17 的独立复核就是靠这个程序发现正式工具扫错了式子：
 * 正式工具当时把 ALPHA 的系数写成 a，而它自己判自己「全部通过」。
 *
 * ## 本程序自己也错过一次（同日第二、第三轮复核）
 *
 * 1. `模型 2` 原先叫「错误模型」，其实它是设计文档 3.5 节保留的**非预乘 ALPHA 开关**，
 *    是一档真实方程。改名并说明它与 CONST 的差异处数相同（4121020）。
 * 2. 「位宽与饱和/截断」一节先把 `t = x + 128` 算出来，又交给 `round255_fast()`，
 *    等于加了两次 128。得到的 97283 / 32640 都是假象。改为 `(tt + (tt >> 8)) >> 8`
 *    之后：模截断 64643 处，**饱和到 16 位 0 处**。
 *    正式工具当时犯的是同一个错，两个工具拿同一个错值互相印证——
 *    这是「两个实现共用同一个错误互相证明不了什么」的具体例子。
 *
 * 编译运行（WSL）：
 *   gcc -O2 -o verify verify_round255.c && ./verify
 * 退出码 0 表示每个模型的组合数与差异数都与期望一致。
 *
 * 定稿方程不变式：out = min(255, round((src_pm*Fs + dst*Fd + 128) / 255))
 *   两种写法  U 两项各自 round(·/255) 后相加、饱和
 *            V 两项相加后一次 round(·/255)、饱和        <- 定稿采纳
 */
#include <stdio.h>
#include <stdint.h>

static int fail = 0;

static uint32_t round255_fast(uint32_t x)
{
    uint32_t t = x + 128u;
    return (t + (t >> 8)) >> 8;
}
static uint32_t round255_ref(uint32_t x)
{
    return (2u * x + 255u) / 510u;
}
static uint32_t sat8(uint32_t v) { return v > 255u ? 255u : v; }

static uint32_t U(uint32_t t1, uint32_t t2)
{
    return sat8(round255_fast(t1) + round255_fast(t2));
}
static uint32_t V(uint32_t t1, uint32_t t2)
{
    return sat8(round255_fast(t1 + t2));
}

static void expect(const char *what, uint64_t got, uint64_t want)
{
    if (got == want) {
        printf("  %-44s %12llu  ok\n", what, (unsigned long long)got);
    } else {
        printf("  %-44s %12llu  FAIL（期望 %llu）\n",
               what, (unsigned long long)got, (unsigned long long)want);
        fail = 1;
    }
}

int main(void)
{
    /* ---------- 基础检查 ---------- */
    printf("基础检查\n");
    {
        uint64_t bad1 = 0, bad2 = 0;
        for (uint32_t x = 0; x <= 65025u; x++)
            if (round255_fast(x) != round255_ref(x)) bad1++;
        for (uint32_t x = 0; x <= 130050u; x++) {
            uint32_t a = round255_fast(x), b = round255_ref(x);
            if ((a > 255u ? 255u : a) != (b > 255u ? 255u : b)) bad2++;
        }
        expect("快速式 vs 精确商  x=0..65025", bad1, 0);
        expect("min(255,快速式) vs 精确商  0..130050", bad2, 0);
    }

    /* ---------- 模型 1：ALPHA 定稿形式 ---------- */
    {
        uint64_t total = 0, diff = 0;
        for (uint32_t a = 0; a <= 255u; a++)
            for (uint32_t pm = 0; pm <= a; pm++)
                for (uint32_t dst = 0; dst <= 255u; dst++) {
                    total++;
                    if (U(pm * 255u, dst * (255u - a)) != V(pm * 255u, dst * (255u - a)))
                        diff++;
                }
        printf("\n模型 1  ALPHA  src_pm*255 + dst*(255-a)   （定稿形式）\n");
        expect("组合数", total, 8421376);
        expect("不一致", diff, 0);
    }

    /* ---------- 模型 1b：把系数表的 Fs=1 按字面实现 ---------- */
    {
        uint64_t total = 0, diff = 0;
        for (uint32_t a = 0; a <= 255u; a++)
            for (uint32_t pm = 0; pm <= a; pm++)
                for (uint32_t dst = 0; dst <= 255u; dst++) {
                    total++;
                    if (U(pm * 1u, dst * (255u - a)) != V(pm * 1u, dst * (255u - a)))
                        diff++;
                }
        printf("\n模型 1b ALPHA  src_pm*1 + dst*(255-a)     （把 Fs=1 按字面实现）\n");
        expect("组合数", total, 8421376);
        expect("不一致", diff, 2068638);
        printf("  说明：系数表的 1 是归一化值，实现要用 255。\n");
        printf("        这里的差异数只说明两种顺序在错误系数下也不同；按字面实现首先是结果本身错\n");
        printf("        （源色权重从 255 掉到 1），见 check_round255.c 的 (c) 节。\n");
    }

    /* ---------- 模型 2：ALPHA 非预乘（真实方程，不是错误模型） ---------- */
    {
        uint64_t total = 0, diff = 0;
        uint32_t fk = 0, fa = 0, fb = 0;
        for (uint32_t a = 0; a <= 255u; a++)
            for (uint32_t s = 0; s <= 255u; s++)
                for (uint32_t d = 0; d <= 255u; d++) {
                    total++;
                    if (U(s * a, d * (255u - a)) != V(s * a, d * (255u - a))) {
                        if (!diff) { fk = a; fa = s; fb = d; }
                        diff++;
                    }
                }
        printf("\n模型 2  ALPHA 非预乘  src*a + dst*(255-a)（设计文档 3.5 节的非预乘开关）\n");
        expect("组合数", total, 16777216);
        expect("不一致", diff, 4121020);
        if (fk != 1u || fa != 1u || fb != 128u) {
            printf("  首例 FAIL（期望 a=1 src=1 dst=128，实得 a=%u src=%u dst=%u）\n", fk, fa, fb);
            fail = 1;
        } else {
            printf("  %-44s a=1 src=1 dst=128  ok\n", "首例");
        }
        /* 这个式子与 CONST 的不一致处数恰好相同，这正是当初把两者混成一个数字的原因 */
        printf("  说明：它与模型 3 的差异处数相同（4121020），结构对称，因此极易混为一谈；\n");
        printf("        正式工具第一版就是把它当成了预乘 ALPHA。\n");
    }

    /* ---------- 模型 3：CONST ---------- */
    {
        uint64_t total = 0, diff = 0;
        uint32_t fk = 0, fa = 0, fb = 0;
        for (uint32_t c = 0; c <= 255u; c++)
            for (uint32_t s = 0; s <= 255u; s++)
                for (uint32_t d = 0; d <= 255u; d++) {
                    total++;
                    if (U(s * c, d * (255u - c)) != V(s * c, d * (255u - c))) {
                        if (!diff) { fk = c; fa = s; fb = d; }
                        diff++;
                    }
                }
        printf("\n模型 3  CONST  src*c + dst*(255-c)          （需要定序的两档之一）\n");
        expect("组合数", total, 16777216);
        expect("不一致", diff, 4121020);
        if (fk != 1u || fa != 1u || fb != 128u) {
            printf("  首例 FAIL（期望 c=1 src=1 dst=128，实得 c=%u src=%u dst=%u）\n", fk, fa, fb);
            fail = 1;
        } else {
            printf("  %-44s c=1 src=1 dst=128  ok\n", "首例");
        }
        expect("首例 U 的结果", U(1u, 128u * 254u), 127);
        expect("首例 V 的结果", V(1u, 128u * 254u), 128);
    }

    /* ---------- 模型 4：MUL 单项 ---------- */
    {
        uint64_t total = 0, diff = 0;
        for (uint32_t s = 0; s <= 255u; s++)
            for (uint32_t d = 0; d <= 255u; d++) {
                total++;
                if (U(s * d, 0u) != V(s * d, 0u))
                    diff++;
            }
        printf("\n模型 4  MUL  src*dst（单项）\n");
        expect("组合数", total, 65536);
        expect("不一致", diff, 0);
        printf("  说明：单项，与 COPY 同形。理由不是「255 的整数倍」，是「只有一个非零乘积」。\n");
    }

    /* ---------- 位宽与「截断 / 饱和到 16 位」 ----------
       注意不能把 t 再交给 round255_fast()：那个函数内部还会加一次 128。
       本程序的第一版就在这里多加了 128，于是把「饱和到 16 位」误判成错的。 */
    {
        uint64_t trunc = 0, sat16 = 0;
        for (uint32_t x = 0; x <= 130050u; x++) {
            uint32_t t = x + 128u;
            uint32_t correct = sat8((t + (t >> 8)) >> 8);
            uint32_t tt = t & 0xFFFFu;                        /* 模截断 */
            if (sat8((tt + (tt >> 8)) >> 8) != correct) trunc++;
            uint32_t s = t > 65535u ? 65535u : t;             /* 饱和到 16 位 */
            if (sat8((s + (s >> 8)) >> 8) != correct) sat16++;
        }
        printf("\n位宽与「截断 / 饱和到 16 位」\n");
        expect("两项之和上限 2*65025", 2u * 255u * 255u, 130050);
        expect("t = x+128 上限", 2u * 255u * 255u + 128u, 130178);
        printf("  2^16 = 65536，2^17 = 131072：130050 与 130178 都需要 17 位无符号\n");
        expect("模截断路径的不一致处数", trunc, 64643);
        expect("饱和到 16 位路径的不一致处数", sat16, 0);
        printf("  说明：中间量不能模截断；17 位或 16 位饱和都可以，算完商后 clamp 到 255。\n");
        printf("        本程序第一版把 +128 加了两次，曾把饱和误判为错（97283 / 32640），\n");
        printf("        两个工具当时拿同一个错值互相印证，是「共用错误互相证明不了什么」的实例。\n");
    }

    printf("\n总判定：%s\n", fail ? "FAIL" : "全部通过");
    return fail;
}
