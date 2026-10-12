/* check_round255.c —— 定稿方程的舍入穷举校验
 *
 * 对应设计文档 3.5 节「逐像素规格」的第 2、3 条。
 *
 * 纯整数，不依赖板子，在 WSL 里编译即可：
 *     gcc -O2 -o check_round255 check_round255.c && ./check_round255
 * 退出码 0 表示全部符合预期；任何一项与预期不符都为非零。
 *
 * ## 校的是什么
 *
 * 定稿方程（设计文档 3.5 节）：
 *     x   = src_pm * Fs + dst * Fd          Fs、Fd 以 255 为满量程的整数表示
 *     out = min(255, (t + (t >> 8)) >> 8)   t = x + 128
 *
 * 六个方程按方程表逐个代入，一次只验一个：
 *
 *     行          Fs        Fd         代入后的通道算式
 *     COPY        255       0          src_pm * 255
 *     ALPHA       255       255 - a    src_pm * 255 + dst * (255 - a)     预乘约束 pm <= a
 *     ALPHA 非预乘 a        255 - a    src * a + dst * (255 - a)          对照开关
 *     ADD         255       255        src_pm * 255 + dst * 255
 *     CONST       c         255 - c    src * c + dst * (255 - c)
 *     MUL         dst       0          src * dst                          单项
 *
 * 另有 (d) 一节：预乘与非预乘两条路径相对实数定义的量化误差。
 *
 * 每个方程比较两种写法：
 *     U  两项各自 round(·/255) 舍入之后再相加、饱和
 *     V  两项相加之后一次 round(·/255) 舍入、饱和        <- 设计文档采纳
 *
 * ## 预期值全部写成断言
 *
 * 本文的每一项都带**期望的组合数与差异数**，不符即失败并指出差在哪一项。
 * 只检查「差异非零」是不够的：那样差异数从 4121020 变成 12345 也会通过。
 *
 * ## 三次订正（都在 2026-09-17）
 *
 * 1. 第一版把 ALPHA 的系数写成 kA = a，扫的是 `src * a + dst * (255 - a)`，
 *    与定稿的 `src_pm * 255 + dst * (255 - a)` 不是同一个式子。
 *    那一版报出的「ALPHA/CONST 4121020 处」实际全部属于 CONST。
 *    顺带正名：`src * a + dst * (255 - a)` 不是「错误模型」，它是设计文档 3.5 节
 *    保留的**非预乘 ALPHA 开关**，板上性能负载的九个用例里也有「BLIT ALPHA 非预乘」。
 * 2. 第一版把两项之和的位宽写成 18 位。实际 130050 与 130178 都小于 2^17 = 131072，
 *    17 位无符号就够；18 位是实现选择（例如 DSP 端口），不是数学上的必须。
 * 3. **`round255_fast()` 内部已经加过一次 128。** 第二版在验「截断」与「饱和到 16 位」
 *    两条路径时，先把 `t = x + 128` 算出来，又把它交给 `round255_fast()`，
 *    等于加了两次 128。当时据此写下「饱和到 16 位不是安全压窄、连 x = 0 也被改坏」，
 *    那是假象。改为直接用 `(tt + (tt >> 8)) >> 8` 之后：
 *    模截断 64643 处不符（原报 97283），**饱和到 16 位与正确结果全域一致（0 处，原报 32640）**。
 *    结论是「中间量不能模截断，但 17 位或 16 位饱和都可以」。
 *    这一条也是「两个实现共用同一个错误，互相证明不了什么」的实例：
 *    正式工具与复核程序当时都把 +128 加了两次，两边的错数一模一样。
 * ## 可选：导出抽样金标准给仿真对账（2026-09-29 新增）
 *
 *     ./check_round255 --golden <文件>
 *
 * 按下面的抽样规则生成 1024 条向量（前 96 条是边界组合，其余每个字段各取一次
 * 线性同余的最高 8 位），每行一个 43 位十六进制字：
 *     {eq[2:0], src[7:0], dst[7:0], a[7:0], c[7:0], out[7:0]}
 * 其中 out 用的是本文件同一条快速式（`mix_V`），与仿真测试台里用精确商现算的期望值互相独立。
 * 引擎的混合核（`rtl/engine/engine_blend.v`）拿它做抽样对账，见 `sim/engine/blend/`。
 * 不带 `--golden` 时输出与本文件以前逐字节相同。
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static int fail = 0;

/* half-up 精确商：round(x / 255) */
static uint32_t round255_ref(uint32_t x)
{
    return (2u * x + 255u) / 510u;
}

/* 设计文档 3.5 节的快速式 */
static uint32_t round255_fast(uint32_t x)
{
    uint32_t t = x + 128u;
    return (t + (t >> 8)) >> 8;
}

static uint32_t sat8(uint32_t v) { return v > 255u ? 255u : v; }

/* U：两项各自舍入后相加饱和。V：两项相加后一次舍入饱和 */
static uint32_t mix_U(uint32_t t1, uint32_t t2)
{
    return sat8(round255_fast(t1) + round255_fast(t2));
}
static uint32_t mix_V(uint32_t t1, uint32_t t2)
{
    return sat8(round255_fast(t1 + t2));
}

/* 断言一个统计量 */
static void expect_u64(const char *what, uint64_t got, uint64_t want)
{
    if (got == want) {
        printf("  %-46s %12llu  ok\n", what, (unsigned long long)got);
    } else {
        printf("  %-46s %12llu  FAIL（期望 %llu）\n",
               what, (unsigned long long)got, (unsigned long long)want);
        fail = 1;
    }
}

/* ---------- 抽样金标准（--golden）：给仿真对账用 ---------- */
#define GOLDEN_N 1024

/* 按方程把两项乘出来，再用本文件的 mix_V（快速式 + 饱和）出结果。
   变量名与注释里的方程表一致：src 在 ALPHA 行是已经预乘过的 src_pm。 */
static uint32_t golden_out(int eq, int src, int dst, int a, int c)
{
    uint32_t t1, t2;
    switch (eq) {
    case 0:  t1 = (uint32_t)src * 255u;                        t2 = 0;                              break;
    case 1:  t1 = (uint32_t)src * 255u;                        t2 = (uint32_t)dst * (uint32_t)(255 - a); break;
    case 2:  t1 = (uint32_t)src * 255u;                        t2 = (uint32_t)dst * 255u;           break;
    case 3:  t1 = (uint32_t)src * (uint32_t)c;                 t2 = (uint32_t)dst * (uint32_t)(255 - c); break;
    case 4:  t1 = (uint32_t)src * (uint32_t)dst;               t2 = 0;                              break;
    default: t1 = (uint32_t)src * (uint32_t)a;                 t2 = (uint32_t)dst * (uint32_t)(255 - a); break;
    }
    return mix_V(t1, t2);
}

/* 线性同余每步取最高 8 位。低位的周期很短（第 k 位周期 2^(k+1)），
   几个字段也不能从同一个 32 位数里切，否则位段重叠、互相关联，
   第一版就因此让 c 的最高位恒为 0（c 只取到 0~127）、方程号 0 与 1 各多一倍。 */
static uint32_t lcg_byte(uint32_t *s)
{
    *s = *s * 1664525u + 1013904223u;
    return *s >> 24;
}

static int write_golden(const char *path)
{
    /* 前 96 条是每个方程的边界组合，其余用固定种子的线性同余补齐，每个字段各取一次 */
    static const int edge[16][4] = {
        {   0,   0,   0,   0 }, { 255, 255, 255, 255 }, {   0, 255, 255,   0 }, { 255,   0,   0, 255 },
        {   1, 128,   1,   1 }, { 128,   1, 255, 128 }, { 254, 255, 128, 254 }, { 255, 254,   0, 127 },
        { 128, 128, 128, 128 }, {   2, 253, 127,   3 }, { 253,   2,   3, 127 }, {  64, 192,  64, 192 },
        { 192,  64, 192,  64 }, {   0,   1,   0, 255 }, { 255, 254, 254, 255 }, { 127, 128, 128, 127 }
    };
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); return 1; }

    uint32_t s = 0x20260929u;
    for (int i = 0; i < GOLDEN_N; i++) {
        int eq, src, dst, a, c;
        if (i < 96) {
            eq = i % 6;
            src = edge[i / 6][0]; dst = edge[i / 6][1];
            a   = edge[i / 6][2]; c   = edge[i / 6][3];
        } else {
            eq  = (int)(lcg_byte(&s) % 6u);
            src = (int)lcg_byte(&s);
            dst = (int)lcg_byte(&s);
            a   = (int)lcg_byte(&s);
            c   = (int)lcg_byte(&s);
        }
        uint32_t o = golden_out(eq, src, dst, a, c);
        uint64_t w = ((uint64_t)eq << 40) | ((uint64_t)src << 32) | ((uint64_t)dst << 24) |
                     ((uint64_t)a << 16) | ((uint64_t)c << 8) | (uint64_t)o;
        fprintf(f, "%011llx\n", (unsigned long long)w);
    }
    fclose(f);
    printf("golden: %d 条抽样写入 %s（位域 {eq,src,dst,a,c,out}）\n", GOLDEN_N, path);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--golden") == 0)
        return write_golden(argv[2]);
    if (argc != 1) {
        fprintf(stderr, "用法: %s [--golden <文件>]\n", argv[0]);
        return 2;
    }
    /* ---------- (a) 快速式在两个取值域上是否等于精确商 ---------- */
    printf("(a) 快速式 vs half-up 精确商\n");
    {
        uint64_t bad_a = 0, bad_e = 0;
        for (uint32_t x = 0; x <= 65025u; x++)
            if (round255_fast(x) != round255_ref(x))
                bad_a++;
        for (uint32_t x = 0; x <= 130050u; x++) {
            uint32_t f = round255_fast(x), r = round255_ref(x);
            if ((f > 255u ? 255u : f) != (r > 255u ? 255u : r))
                bad_e++;
        }
        expect_u64("x = 0..65025 不一致", bad_a, 0);
        expect_u64("x = 0..130050 且 clamp 后不一致", bad_e, 0);

        /* 裸快速式在 17 位域上失效的处数：253 */
        uint64_t bare = 0;
        for (uint32_t x = 0; x <= 130050u; x++)
            if (round255_fast(x) != round255_ref(x))
                bare++;
        expect_u64("裸快速式在 130050 域上不一致", bare, 253);
        printf("    说明：t = x + 128 的上限 %u 需要 17 位；把 min(255, q) 并进来之后\n",
               130050u + 128u);
        printf("          与精确商一致。裸快速式因 (t >> 8) 超过 8 位而失效。\n");
    }

    /* ---------- (b) 位宽：17 位够，截断会错，饱和到 16 位可以 ---------- */
    printf("\n(b) 中间量位宽与「截断 / 饱和到 16 位」的区别\n");
    {
        uint32_t maxprod = 255u * 255u;          /* 65025 */
        uint32_t maxsum  = 2u * maxprod;         /* 130050 */
        uint32_t maxt    = maxsum + 128u;        /* 130178 */
        printf("    单项乘积上限 %u，16 位（2^16 = 65536）放得下\n", maxprod);
        printf("    两项之和上限 %u，需要 17 位；2^17 = 131072\n", maxsum);
        printf("    t = x + 128 上限 %u，同样 17 位\n", maxt);
        if (!(maxsum < 131072u) || !(maxt < 131072u)) {
            printf("    FAIL 17 位不够，与预期不符\n");
            fail = 1;
        }
        if (!(maxsum >= 65536u)) {
            printf("    FAIL 两项之和并没有超出 16 位，与预期不符\n");
            fail = 1;
        }

        /* 注意：下面两条逐值比较直接用「已加过 128 的 t」算商，
           不能再把它交给 round255_fast()——那个函数内部还会再加 128。
           第一版就是在这里多加了 128，得到的错处数是假象（见文件头的第三次订正）。 */

        /* 模截断：取 t 的低 16 位，然后 (tt + (tt >> 8)) >> 8 */
        uint32_t trunc = maxt & 0xFFFFu;                       /* 130178 & 0xFFFF = 64642 */
        uint32_t trunc_q = (trunc + (trunc >> 8)) >> 8;
        uint32_t want_q  = (maxt + (maxt >> 8)) >> 8;          /* 正确但会超过 255，故下面 clamp */
        printf("    模截断 t & 0xFFFF = %u -> q = %u\n", trunc, trunc_q);
        expect_u64("  模截断路径在 x = 130050 上的结果", trunc_q, 253);

        uint64_t bad_trunc = 0;
        for (uint32_t x = 0; x <= maxsum; x++) {
            uint32_t t = x + 128u;
            uint32_t tt = t & 0xFFFFu;
            uint32_t a = sat8((tt + (tt >> 8)) >> 8);
            uint32_t b = sat8((t + (t >> 8)) >> 8);
            if (a != b)
                bad_trunc++;
        }
        expect_u64("模截断路径的全域不一致处数", bad_trunc, 64643);

        /* 饱和到 16 位（饱和加法器的写法）：min(65535, t) 之后再算商。
           这一条与正确结果全域一致——「饱和」不是「截断」，它是安全的压窄。 */
        uint64_t bad_sat16 = 0;
        for (uint32_t x = 0; x <= maxsum; x++) {
            uint32_t t = x + 128u;
            uint32_t s = t > 65535u ? 65535u : t;
            uint32_t a = sat8((s + (s >> 8)) >> 8);
            uint32_t b = sat8((t + (t >> 8)) >> 8);
            if (a != b)
                bad_sat16++;
        }
        expect_u64("饱和到 16 位路径的不一致处数", bad_sat16, 0);
        printf("    说明：模截断（丢掉进位）会错 %u 处；饱和到 16 位与正确结果**全域一致**。\n",
               (unsigned)64643);
        printf("          即：中间量不能模截断，但用 17 位或 16 位饱和都可以。\n");
        printf("          这条曾因本文件多算了一次 +128 而被误判为「饱和也不行」，见文件头。\n");
        (void)want_q;
    }

    /* ---------- (c) 逐方程比较 U 与 V，全部写成断言 ---------- */
    printf("\n(c) 各方程 U（先各自舍入）与 V（先加后舍入）的差异处数\n");

    /* COPY: Fs=255, Fd=0 -> src_pm*255，只有一项 */
    {
        uint64_t total = 0, diff = 0;
        for (uint32_t s = 0; s <= 255u; s++) {
            total++;
            if (mix_U(s * 255u, 0u) != mix_V(s * 255u, 0u))
                diff++;
        }
        expect_u64("COPY   255, 0                 组合数", total, 256);
        expect_u64("COPY   255, 0                 不一致", diff, 0);
    }

    /* ALPHA: Fs=255, Fd=255-a -> src_pm*255 + dst*(255-a)，预乘约束 pm <= a */
    {
        uint64_t total = 0, diff = 0;
        for (uint32_t a = 0; a <= 255u; a++)
            for (uint32_t pm = 0; pm <= a; pm++)
                for (uint32_t dst = 0; dst <= 255u; dst++) {
                    total++;
                    if (mix_U(pm * 255u, dst * (255u - a)) !=
                        mix_V(pm * 255u, dst * (255u - a)))
                        diff++;
                }
        expect_u64("ALPHA  255, 255-a（pm <= a）  组合数", total, 8421376);
        expect_u64("ALPHA  255, 255-a（pm <= a）  不一致", diff, 0);
    }

    /* ALPHA 非预乘：Fs=a, Fd=255-a -> src*a + dst*(255-a)。
       这是设计文档 3.5 节保留的「非预乘开关」，E7 的九个用例里也有
       「BLIT ALPHA 非预乘」。它是一档**真实存在的方程**，不是「错误模型」。
       第一版工具把它当成 ALPHA 的预乘形式，才误得出「ALPHA 与顺序有关」。 */
    {
        uint64_t total = 0, diff = 0;
        uint32_t fk = 0, fs = 0, fd = 0;
        for (uint32_t a = 0; a <= 255u; a++)
            for (uint32_t s = 0; s <= 255u; s++)
                for (uint32_t d = 0; d <= 255u; d++) {
                    total++;
                    if (mix_U(s * a, d * (255u - a)) != mix_V(s * a, d * (255u - a))) {
                        if (!diff) { fk = a; fs = s; fd = d; }
                        diff++;
                    }
                }
        expect_u64("ALPHA 非预乘 a, 255-a        组合数", total, 16777216);
        expect_u64("ALPHA 非预乘 a, 255-a        不一致", diff, 4121020);
        if (fk != 1u || fs != 1u || fd != 128u) {
            printf("  ALPHA 非预乘首个反例 FAIL（期望 a=1 src=1 dst=128，实得 a=%u src=%u dst=%u）\n",
                   fk, fs, fd);
            fail = 1;
        } else {
            printf("  %-44s a=1 src=1 dst=128  ok\n", "ALPHA 非预乘首个反例");
        }
    }

    /* ALPHA 把系数表的 Fs=1 按字面实现。
       注意这一项的问题不是「与顺序有关」，而是**结果本身就错**：
       源色的权重变成 1/255 而不是 1，整个源色贡献被除掉了。 */
    {
        uint64_t total = 0, wrong = 0;
        for (uint32_t a = 0; a <= 255u; a++)
            for (uint32_t pm = 0; pm <= a; pm++)
                for (uint32_t dst = 0; dst <= 255u; dst++) {
                    total++;
                    /* 正确：src_pm*255 + dst*(255-a)；错误：src_pm*1 + dst*(255-a) */
                    if (mix_V(pm * 1u, dst * (255u - a)) != mix_V(pm * 255u, dst * (255u - a)))
                        wrong++;
                }
        expect_u64("ALPHA  Fs=1 按字面（结果错） 组合数", total, 8421376);
        expect_u64("ALPHA  Fs=1 按字面（结果错） 与正确值不同的组数", wrong, 8355712);
        printf("    说明：系数表的 1 是实现里的 255。按字面实现不是「顺序问题」，\n");
        printf("          而是源色权重从 255 掉到 1，结果全面错误。\n");
    }

    /* ADD: Fs=255, Fd=255 */
    {
        uint64_t total = 0, diff = 0;
        for (uint32_t s = 0; s <= 255u; s++)
            for (uint32_t d = 0; d <= 255u; d++) {
                total++;
                if (mix_U(s * 255u, d * 255u) != mix_V(s * 255u, d * 255u))
                    diff++;
            }
        expect_u64("ADD    255, 255               组合数", total, 65536);
        expect_u64("ADD    255, 255               不一致", diff, 0);
    }

    /* CONST: Fs=c, Fd=255-c */
    {
        uint64_t total = 0, diff = 0;
        uint32_t fa = 0, fb = 0, fk = 0;
        for (uint32_t c = 0; c <= 255u; c++)
            for (uint32_t s = 0; s <= 255u; s++)
                for (uint32_t d = 0; d <= 255u; d++) {
                    total++;
                    if (mix_U(s * c, d * (255u - c)) != mix_V(s * c, d * (255u - c))) {
                        if (!diff) { fk = c; fa = s; fb = d; }
                        diff++;
                    }
                }
        expect_u64("CONST  c, 255-c               组合数", total, 16777216);
        expect_u64("CONST  c, 255-c               不一致", diff, 4121020);
        if (fk != 1u || fa != 1u || fb != 128u) {
            printf("  CONST 首个反例 FAIL（期望 c=1 src=1 dst=128，实得 c=%u src=%u dst=%u）\n",
                   fk, fa, fb);
            fail = 1;
        } else {
            printf("  %-46s c=1 src=1 dst=128  ok\n", "CONST 首个反例");
        }
        /* 最小反例的两个结果 */
        expect_u64("CONST 反例 U 的结果", mix_U(1u * 1u, 128u * 254u), 127);
        expect_u64("CONST 反例 V 的结果", mix_V(1u * 1u, 128u * 254u), 128);
    }

    /* MUL: Fs=dst, Fd=0 -> src*dst，单项 */
    {
        uint64_t total = 0, diff = 0;
        for (uint32_t s = 0; s <= 255u; s++)
            for (uint32_t d = 0; d <= 255u; d++) {
                total++;
                if (mix_U(s * d, 0u) != mix_V(s * d, 0u))
                    diff++;
            }
        expect_u64("MUL    dst, 0（单项）         组合数", total, 65536);
        expect_u64("MUL    dst, 0（单项）         不一致", diff, 0);
    }

    /* ---------- (d) 预乘与非预乘的量化误差 ----------
     * 基准是非预乘源色按实数定义混合再四舍五入：round((s*a + d*(255-a)) / 255)。
     * 预乘路径：素材侧 pm = round(s*a/255)，混合时 round((pm*255 + d*(255-a)) / 255)。
     * 非预乘路径（V 顺序）：round((s*a + d*(255-a)) / 255)。
     * 偏差以 1/255 为单位记整数：|out*255 - x|，避免浮点。 */
    printf("\n(d) 预乘与非预乘的量化误差（以非预乘源色的实数混合为基准）\n");
    {
        uint64_t total = 0, diff_pm = 0, diff_np = 0;
        uint32_t max_pm = 0, max_np = 0;
        for (uint32_t a = 0; a <= 255u; a++)
            for (uint32_t s = 0; s <= 255u; s++)
                for (uint32_t d = 0; d <= 255u; d++) {
                    uint32_t x = s * a + d * (255u - a);
                    uint32_t ideal = round255_ref(x);
                    uint32_t pm = round255_fast(s * a);
                    uint32_t out_pm = mix_V(pm * 255u, d * (255u - a));
                    uint32_t out_np = mix_V(s * a, d * (255u - a));
                    uint32_t e_pm = out_pm * 255u > x ? out_pm * 255u - x : x - out_pm * 255u;
                    uint32_t e_np = out_np * 255u > x ? out_np * 255u - x : x - out_np * 255u;
                    total++;
                    diff_pm += (out_pm != ideal);
                    diff_np += (out_np != ideal);
                    if (e_pm > max_pm) max_pm = e_pm;
                    if (e_np > max_np) max_np = e_np;
                }
        expect_u64("组合数", total, 16777216);
        expect_u64("预乘路径 与基准不同的组数", diff_pm, 4121020);
        expect_u64("预乘路径 最大偏差（x/255）", max_pm, 254);
        expect_u64("非预乘路径 与基准不同的组数", diff_np, 0);
        expect_u64("非预乘路径 最大偏差（x/255）", max_np, 127);
        printf("    说明：预乘在素材侧多量化一次，最大偏差 254/255 约 1.0；\n");
        printf("          非预乘只量化一次，最大偏差 127/255 约 0.498，与基准逐值相同。\n");
    }

    /* ---------- (e) 结论 ---------- */
    printf("\n(e) 结论\n");
    printf("    与舍入顺序无关的：COPY、ADD、MUL（单项或 255 的整数倍系数）\n");
    printf("    与舍入顺序有关的两档：CONST 与非预乘 ALPHA，各 4121020 / 16777216 = 24.56%%\n");
    printf("    预乘 ALPHA 与顺序无关（系数 255 与 255-a，第一项是 255 的整数倍，不产生舍入）\n");
    printf("    采纳 V（先加后舍入）：把混合定义算完之后只量化一次，对六个方程是同一套写法\n");
    printf("    中间量不能模截断；17 位或 16 位饱和都可以，算完商后 clamp 到 255\n");

    printf("\n总判定：%s\n", fail ? "FAIL" : "全部通过");
    return fail;
}
