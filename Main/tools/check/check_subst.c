/* check_subst.c —— 六个混合方程的「代入表」与逐位等价穷举
 *
 * 对应 Engineering/TechReports/engine/01-计算核心.md 3.1 节的代入表与第五节「代入表」一行判据。
 *
 * D9 要求六个方程都写成  x = 基数 × 255 + m1 × m2 （整数恒等），
 * 每通道只需要一个有符号乘法器。这个工具对每个方程的**全部输入**穷举，
 * 把硬件形式的结果与 V 顺序（先加后舍入一次、再饱和）的**精确商**逐值比较。
 *
 * 纯整数，不依赖板子，在 WSL 里编译即可：
 *     gcc -O2 -Wall -Wextra -o check_subst check_subst.c && ./check_subst
 * 退出码 0 表示全部符合预期。
 *
 * ## 硬件形式的位宽（这就是要验的东西）
 *
 *     base  : 9 位无符号   0 .. 510      （ADD 要 src+dst）
 *     m1    : 9 位有符号   -255 .. 255   （CONST 与非预乘 ALPHA 要 src-dst）
 *     m2    : 8 位无符号   0 .. 255      （进乘法器时按 9 位有符号零扩展）
 *     p     : m1 × m2      有符号，18 位
 *     c255  : base × 255   无符号，17 位
 *     x     : p + c255     有符号 19 位累加，结果落在 0 .. 130050（17 位无符号）
 *
 * ## 输入范围：每个方程都跑满自己的全定义域
 *
 * 2026-09-29 订正：第一版把 ALPHA 预乘限制在 `src_pm <= a`（spec 第 01 章的素材约束），
 * 只跑了 8421376 组。**硬件会收到任意输入**：spec 第 02 章只对 ALPHA FILL 的常量做
 * BAD_PREMULT 检查，BLIT 从表面读进来的源像素没有逐个检查（运行时画出来的表面
 * 再拿去当源就是这种情形），软件执行器 exec_sw.c 对任意输入都按同一条规则算。
 * 硬件要和它逐位一致，这个范围就必须覆盖到。现在 ALPHA 跑满 2^24 = 16777216 组。
 *
 * 全范围下 x 的上限不止 ADD 能到 130050：ALPHA 预乘在 `a = 0`（Fd = 255）时
 * `x = src × 255 + dst × 255` 也能到 130050，于是 **16 位模截断在 ALPHA 上同样会出错**。
 * 第 (c) 节按方程分别给期望值，正是为了把这一点钉住。
 *
 * V 顺序（设计文档 3.5 节、spec 第 01 章 3.3、3.4 节）：
 *     x   = src × Fs + dst × Fd            Fs、Fd 以 255 为满量程
 *     t   = x + 128
 *     out = min(255, (t + (t >> 8)) >> 8)
 * 参考值用 round255_ref(x) = (2x + 255)/510（half-up 精确商），不用快速式，
 * 与被测的写法独立。
 */
#include <stdio.h>
#include <stdint.h>

static int fail = 0;

/* half-up 精确商 round(x / 255) */
static uint32_t round255_ref(uint32_t x) { return (2u * x + 255u) / 510u; }
static uint32_t sat8(uint32_t v) { return v > 255u ? 255u : v; }

/* V 顺序，参考实现：精确商 + 饱和 */
static uint32_t blend_ref(uint32_t x) { return sat8(round255_ref(x)); }

/* V 顺序，快速式 + 饱和（将被综合成硬件的那一条） */
static uint32_t blend_fast(uint32_t x)
{
    uint32_t t = x + 128u;
    return sat8((t + (t >> 8)) >> 8);
}

/* V 顺序，快速式但中间量模截断到 16 位（注错版，必须见红） */
static uint32_t blend_fast_trunc16(uint32_t x)
{
    uint32_t t = (x + 128u) & 0xFFFFu;
    return sat8((t + (t >> 8)) >> 8);
}

/* ---------- 方程定义：编号与设计文档 3.5 节一致 ---------- */
enum { EQ_COPY = 0, EQ_ALPHA = 1, EQ_ADD = 2, EQ_CONST = 3, EQ_MUL = 4, EQ_ALPHA_STRAIGHT = 5 };
#define NEQ 6

static const char *eq_name[NEQ] = {
    "COPY           (255, 0)  ",
    "ALPHA 预乘     (255,255-a)",
    "ADD            (255, 255)",
    "CONST          (c, 255-c)",
    "MUL            (dst, 0)  ",
    "ALPHA 非预乘   (a, 255-a)"
};

/* 代入表：给出 base / m1 / m2 三个表达式 */
static const char *base_expr[NEQ] = { "src",   "src",   "src+dst", "dst",   "0",   "dst"   };
static const char *m1_expr[NEQ]   = { "0",     "dst",   "0",       "src-dst","src", "src-dst" };
static const char *m2_expr[NEQ]   = { "0",     "255-a", "0",       "c",     "dst", "a"     };
static const char *fs_fd[NEQ]     = { "255, 0", "255, 255-a", "255, 255", "c, 255-c", "dst, 0", "a, 255-a" };

/* 各方程在**全定义域**上 x 的上限 */
static const uint32_t x_max_expect[NEQ] = { 65025u, 130050u, 130050u, 65025u, 65025u, 65025u };

/* 各方程在**全定义域**上 16 位模截断注错版与正确输出不同的处数（第 (c) 节断言） */
static const uint64_t trunc_expect[NEQ] = { 0u, 4113532u, 32385u, 0u, 0u, 0u };

/* 各方程在**全定义域**上的组合数 */
static const uint64_t combos_expect[NEQ] = {
    256u, 16777216u, 65536u, 16777216u, 65536u, 16777216u
};

/* 硬件的代入（与 RTL 的 case 一一对应） */
static void hw_subst(int eq, int src, int dst, int a, int c,
                     int *base, int *m1, int *m2)
{
    switch (eq) {
    case EQ_COPY:           *base = src;         *m1 = 0;             *m2 = 0;         break;
    case EQ_ALPHA:          *base = src;         *m1 = dst;           *m2 = 255 - a;   break;
    case EQ_ADD:            *base = src + dst;   *m1 = 0;             *m2 = 0;         break;
    case EQ_CONST:          *base = dst;         *m1 = src - dst;     *m2 = c;         break;
    case EQ_MUL:            *base = 0;           *m1 = src;           *m2 = dst;       break;
    default:                *base = dst;         *m1 = src - dst;     *m2 = a;         break;
    }
}

/* 替代代入（2026-09-29 新增）：按 src 与 dst 谁大来选，让 m1、m2 都落进 8 位无符号。
 * 只有 CONST 与非预乘 ALPHA 需要这条分支；其余四个方程的 m1、m2 本来就 <= 255。 */
static void hw_subst_alt(int eq, int src, int dst, int a, int c,
                         int *base, int *m1, int *m2)
{
    switch (eq) {
    case EQ_COPY:  *base = src;       *m1 = 0;       *m2 = 0;       break;
    case EQ_ALPHA: *base = src;       *m1 = dst;     *m2 = 255 - a; break;
    case EQ_ADD:   *base = src + dst; *m1 = 0;       *m2 = 0;       break;
    case EQ_MUL:   *base = 0;         *m1 = src;     *m2 = dst;     break;
    case EQ_CONST:
        if (src >= dst) { *base = dst; *m1 = src - dst; *m2 = c;       }
        else            { *base = src; *m1 = dst - src; *m2 = 255 - c; }
        break;
    default:       /* ALPHA 非预乘 */
        if (src >= dst) { *base = dst; *m1 = src - dst; *m2 = a;       }
        else            { *base = src; *m1 = dst - src; *m2 = 255 - a; }
        break;
    }
}

/* 精确定义：x = src × Fs + dst × Fd，Fs/Fd 以 255 为满量程 */
static uint32_t exact_x(int eq, int src, int dst, int a, int c)
{
    switch (eq) {
    case EQ_COPY:           return (uint32_t)src * 255u;
    case EQ_ALPHA:          return (uint32_t)src * 255u + (uint32_t)dst * (uint32_t)(255 - a);
    case EQ_ADD:            return (uint32_t)src * 255u + (uint32_t)dst * 255u;
    case EQ_CONST:          return (uint32_t)src * (uint32_t)c + (uint32_t)dst * (uint32_t)(255 - c);
    case EQ_MUL:            return (uint32_t)src * (uint32_t)dst;
    default:                return (uint32_t)src * (uint32_t)a + (uint32_t)dst * (uint32_t)(255 - a);
    }
}

/* 硬件形式：定宽有符号运算，模拟 RTL 的位宽 */
static int32_t hw_x(int base, int m1, int m2)
{
    int32_t p    = (int32_t)m1 * (int32_t)m2;   /* 18 位有符号 */
    int32_t c255 = (int32_t)base * 255;         /* 17 位无符号 */
    return p + c255;                            /* 19 位有符号累加 */
}

/* ---------- 逐方程穷举 ---------- */
typedef struct {
    uint64_t combos;
    uint64_t subst_bad;      /* 整数恒等不成立 */
    uint64_t alt_bad;        /* 替代代入的整数恒等不成立 */
    uint64_t ref_bad;        /* 快速式输出与精确商不同 */
    uint64_t trunc_bad;      /* 16 位模截断注错版与正确输出不同 */
    int32_t  alt_m1_max, alt_m2_max;   /* 替代代入的两个乘数上限 */
    int32_t  base_min, base_max, m1_min, m1_max, m2_min, m2_max;
    int32_t  p_min, p_max, x_min, x_max;
} stat_t;

static void stat_init(stat_t *s)
{
    s->combos = s->subst_bad = s->alt_bad = s->ref_bad = s->trunc_bad = 0;
    s->alt_m1_max = s->alt_m2_max = 0;
    s->base_min = s->m1_min = s->m2_min = 1 << 30;
    s->base_max = s->m1_max = s->m2_max = -(1 << 30);
    s->p_min = s->x_min = 1 << 30;
    s->p_max = s->x_max = -(1 << 30);
}

static void stat_one(stat_t *s, int eq, int src, int dst, int a, int c)
{
    int base, m1, m2, abase, am1, am2;
    hw_subst(eq, src, dst, a, c, &base, &m1, &m2);
    hw_subst_alt(eq, src, dst, a, c, &abase, &am1, &am2);

    int32_t p    = (int32_t)m1 * (int32_t)m2;
    int32_t x_hw = hw_x(base, m1, m2);
    int32_t x_al = hw_x(abase, am1, am2);
    uint32_t x_ex = exact_x(eq, src, dst, a, c);

    s->combos++;
    if (x_hw != (int32_t)x_ex) s->subst_bad++;
    if (x_al != (int32_t)x_ex) s->alt_bad++;
    if (am1 > s->alt_m1_max) s->alt_m1_max = am1;
    if (am2 > s->alt_m2_max) s->alt_m2_max = am2;

    if (base < s->base_min) s->base_min = base;
    if (base > s->base_max) s->base_max = base;
    if (m1 < s->m1_min) s->m1_min = m1;
    if (m1 > s->m1_max) s->m1_max = m1;
    if (m2 < s->m2_min) s->m2_min = m2;
    if (m2 > s->m2_max) s->m2_max = m2;
    if (p < s->p_min) s->p_min = p;
    if (p > s->p_max) s->p_max = p;
    if (x_hw < s->x_min) s->x_min = x_hw;
    if (x_hw > s->x_max) s->x_max = x_hw;

    uint32_t o_ref   = blend_ref((uint32_t)x_hw);
    uint32_t o_fast  = blend_fast((uint32_t)x_hw);
    uint32_t o_trunc = blend_fast_trunc16((uint32_t)x_hw);
    if (o_fast != o_ref)  s->ref_bad++;
    if (o_trunc != o_ref) s->trunc_bad++;
}

static void expect_u64(const char *what, uint64_t got, uint64_t want)
{
    if (got == want) {
        printf("  %-52s %14llu  ok\n", what, (unsigned long long)got);
    } else {
        printf("  %-52s %14llu  FAIL（期望 %llu）\n",
               what, (unsigned long long)got, (unsigned long long)want);
        fail = 1;
    }
}

static void run_eq(int eq)
{
    stat_t s;
    stat_init(&s);

    switch (eq) {
    case EQ_COPY:
        for (int src = 0; src <= 255; src++)
            stat_one(&s, eq, src, 0, 0, 0);
        break;
    case EQ_ALPHA:                       /* 全定义域：src（= src_pm）、a、dst 各 0..255 */
        for (int a = 0; a <= 255; a++)
            for (int src = 0; src <= 255; src++)
                for (int dst = 0; dst <= 255; dst++)
                    stat_one(&s, eq, src, dst, a, 0);
        break;
    case EQ_ADD:
        for (int src = 0; src <= 255; src++)
            for (int dst = 0; dst <= 255; dst++)
                stat_one(&s, eq, src, dst, 0, 0);
        break;
    case EQ_CONST:
        for (int c = 0; c <= 255; c++)
            for (int src = 0; src <= 255; src++)
                for (int dst = 0; dst <= 255; dst++)
                    stat_one(&s, eq, src, dst, 0, c);
        break;
    case EQ_MUL:
        for (int src = 0; src <= 255; src++)
            for (int dst = 0; dst <= 255; dst++)
                stat_one(&s, eq, src, dst, 0, 0);
        break;
    default:                             /* ALPHA 非预乘 */
        for (int a = 0; a <= 255; a++)
            for (int src = 0; src <= 255; src++)
                for (int dst = 0; dst <= 255; dst++)
                    stat_one(&s, eq, src, dst, a, 0);
        break;
    }

    printf("\n方程 %d  %s   Fs,Fd = %s\n", eq, eq_name[eq], fs_fd[eq]);
    printf("  base = %-8s  m1 = %-8s  m2 = %-8s\n", base_expr[eq], m1_expr[eq], m2_expr[eq]);
    printf("  实测取值范围：base [%d,%d]   m1 [%d,%d]   m2 [%d,%d]   p [%d,%d]   x [%d,%d]\n",
           s.base_min, s.base_max, s.m1_min, s.m1_max, s.m2_min, s.m2_max,
           s.p_min, s.p_max, s.x_min, s.x_max);
    printf("  组合数 %llu\n", (unsigned long long)s.combos);
    expect_u64("组合数（全定义域）", s.combos, combos_expect[eq]);
    expect_u64("整数恒等 base*255 + m1*m2 == src*Fs + dst*Fd 不同处数", s.subst_bad, 0);
    expect_u64("替代代入（两个乘数都 8 位）恒等不同处数", s.alt_bad, 0);
    expect_u64("快速式输出与精确商不同处数", s.ref_bad, 0);
    expect_u64("x 上限", (uint64_t)s.x_max, x_max_expect[eq]);
    expect_u64("16 位模截断注错版与正确输出的不同处数", s.trunc_bad, trunc_expect[eq]);
    if (s.alt_m1_max > 255 || s.alt_m2_max > 255) {
        printf("  替代代入的乘数超出 8 位：m1max=%d m2max=%d  FAIL\n", s.alt_m1_max, s.alt_m2_max);
        fail = 1;
    } else {
        printf("  %-52s m1max=%d m2max=%d  ok\n", "替代代入的两个乘数都在 8 位内", s.alt_m1_max, s.alt_m2_max);
    }
}

int main(void)
{
    printf("六个方程的代入表与逐位等价穷举（每个方程都跑满自己的全定义域）\n");
    printf("硬件形式：base 9 位无符号、m1 9 位有符号、m2 8 位无符号（进乘法器前零扩展）、累加 18/19 位有符号\n");

    for (int eq = 0; eq < NEQ; eq++)
        run_eq(eq);

    /* ---------- 位宽结论的显式断言 ---------- */
    printf("\n位宽结论\n");
    {
        int32_t p_max = 255 * 255;                 /* 65025 */
        int32_t x_max = 2 * 255 * 255;             /* 130050 */
        int32_t t_max = x_max + 128;               /* 130178 */
        int32_t q_max = t_max + (t_max >> 8);      /* 130686 */
        printf("  |m1×m2| 上限 %d，需 17 位有符号（2^16 = 65536）\n", p_max);
        printf("  base×255 上限 %d，需 17 位无符号\n", x_max);
        printf("  x 上限 %d，需 17 位无符号；2^17 = 131072\n", x_max);
        printf("  t = x + 128 上限 %d，需 18 位无符号\n", t_max);
        printf("  t + (t>>8) 上限 %d，需 18 位无符号（RTL 按 19 位写）\n", q_max);
        expect_u64("x 上限 < 2^17", (uint64_t)(x_max < 131072), 1);
        expect_u64("t 上限 < 2^18", (uint64_t)(t_max < 262144), 1);

        printf("\n  能到 x = 130050 的只有 ADD 与 ALPHA 预乘（a = 0 时 Fd = 255）：\n");
        printf("    ADD          x = (src+dst) × 255\n");
        printf("    ALPHA 预乘   x = src × 255 + dst × (255-a)，a = 0 时到 130050\n");
        printf("  所以 16 位模截断在 ALPHA 与 ADD 上都会出错，其余四个方程 x <= 65025、截断是空操作。\n");
    }

    printf("\n总判定：%s\n", fail ? "FAIL" : "全部通过");
    return fail;
}
