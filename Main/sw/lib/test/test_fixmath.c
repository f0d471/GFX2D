/******************************************************************************
* @file test_fixmath.c: 数值基础（SW7 2.1 节）的主机侧测试
*
* @brief  七组，判据见 Engineering/Todo/sw/SW7-游戏本体.md 第 3 节「正弦表与 atan2」「定点精度」两行：
*           F1  正弦表的结构：四个象限点、尾巴、奇对称与 sin(π-x) 对称，全是整数比较
*           F2  正弦表对双精度参考的最大误差（<= 1 LSB），sin²+cos² 的偏差登记
*           F3  任意 BAM 按 2.1 节的下标规则查表的误差（<= 51 LSB），与下标的四舍五入边界
*           F4  atan2 在 [-512, 511]² 上穷举，最大误差 < 64 BAM；atan2(0, 0) == 0
*           F5  atan2 在 Q.8 大范围随机样本与 int32 边界组合上，最大误差 < 64 BAM
*           F6  慢速圆环：32 发、0.5 像素/帧、600 帧，每颗弹离理想位置不超过 4 像素
*           F7  方向约定一致：atan2 对表上每一格的 (cos, sin) 取回的角与这一格相差不超过 8 BAM
*
*         **双精度只出现在这里**（SW1-SW9 总览约束 2 管的是 lib/ 与应用）。参考值取 libm 的
*         sin、atan2，所以测试要链 -lm。64 位与 -m32 两份输出必须逐字节相同：误差以千分之一为单位
*         向下取整后打印，libm 在两个位宽上最后一位的差别到不了这个量级；判定阈值离实测值都很远。
*
*         注错开关见 core/fixmath.h 的文件头，每个都必须让这里至少一行 FAIL：
*           FX_IDX_TRUNC     -> F3 两行
*           FX_VEL_Q4        -> F6
*           FX_ATAN_NOSCALE  -> F5 前两行；UBSan 在 stderr 报溢出，int32 边界组随后除以零崩掉，
*                               所以本文件的输出每行 flush，崩之前判出的 FAIL 不会丢
*           FX_ATAN_UNIT     -> F4、F5、F7
******************************************************************************/

#include <math.h>
#include <stdio.h>

#include "core/fixmath.h"
#include "core/format.h"
#include "core/xorshift32.h"
#include "test_fixmath.h"

static uint32_t g_fail;

/* 每行落地：注错 FX_ATAN_NOSCALE 会在后面的 int32 边界组里除以零崩掉，
   不 flush 的话前面已经判出的 FAIL 行留在缓冲区里一起丢失。 */
static void say(const char *s)
{
    fputs(s, stdout);
    fflush(stdout);
}

static void ck(const char *tag, int ok)
{
    char     line[128];
    core_out o;

    core_out_init(&o, line, (uint32_t)sizeof line);
    core_out_str(&o, tag);
    core_out_str(&o, ok ? " ok\n" : " FAIL\n");
    say(line);
    if (!ok)
        g_fail++;
}

/* 非负的双精度量按千分之一向下取整打印成「整数.三位小数」。 */
static void out_milli(core_out *o, double v)
{
    uint32_t m = (v <= 0.0) ? 0u : (uint32_t)(v * 1000.0);
    uint32_t frac = m % 1000u;

    core_out_u32(o, m / 1000u);
    core_out_ch(o, '.');
    core_out_ch(o, (char)('0' + frac / 100u));
    core_out_ch(o, (char)('0' + (frac / 10u) % 10u));
    core_out_ch(o, (char)('0' + frac % 10u));
}

static void out_i32(core_out *o, int32_t v)
{
    if (v < 0) {
        core_out_ch(o, '-');
        core_out_u32(o, (uint32_t)(-(int64_t)v));
    } else {
        core_out_u32(o, (uint32_t)v);
    }
}

/* 「最大误差 <= limit」（strict 时是 <）的登记行：实测值与阈值都打出来。 */
static void ck_max(const char *tag, double got, double limit, int strict)
{
    char     line[160];
    core_out o;
    int      ok = strict ? (got < limit) : (got <= limit);

    core_out_init(&o, line, (uint32_t)sizeof line);
    core_out_str(&o, tag);
    core_out_str(&o, " max=");
    out_milli(&o, got);
    core_out_str(&o, strict ? " limit<" : " limit<=");
    out_milli(&o, limit);
    core_out_str(&o, ok ? " ok\n" : " FAIL\n");
    say(line);
    if (!ok)
        g_fail++;
}

/* BAM 的误差：got 与 atan2(y, x) 的差，按一周回绕到 (-32768, 32768] 再取绝对值。 */
static double bam_err(uint16_t got, double y, double x)
{
    double ref = atan2(y, x) * (65536.0 / (2.0 * M_PI));
    double d   = (double)got - ref;

    while (d > 32768.0)
        d -= 65536.0;
    while (d < -32768.0)
        d += 65536.0;
    return fabs(d);
}

/* --- F1 表的结构 ---------------------------------------------------------- */

static void f1_shape(void)
{
    uint32_t i, bad_tail = 0u, bad_odd = 0u, bad_mirror = 0u;

    ck("F1 sin_tab[0] == 0", fx_sin_tab[0] == 0);
    ck("F1 sin_tab[256] == 16384", fx_sin_tab[256] == 16384);
    ck("F1 sin_tab[512] == 0", fx_sin_tab[512] == 0);
    ck("F1 sin_tab[768] == -16384", fx_sin_tab[768] == -16384);

    for (i = 0u; i < FX_SIN_TAIL; i++)
        if (fx_sin_tab[FX_SIN_N + i] != fx_sin_tab[i])
            bad_tail++;
    /* 按绝对值四舍五入的直接后果：负半周与正半周逐项互为相反数 */
    for (i = 1u; i < FX_SIN_N; i++)
        if (fx_sin_tab[FX_SIN_N - i] != -fx_sin_tab[i])
            bad_odd++;
    for (i = 0u; i <= 512u; i++)
        if (fx_sin_tab[512u - i] != fx_sin_tab[i])
            bad_mirror++;

    ck("F1 tail repeats head (256)", bad_tail == 0u);
    ck("F1 sin(-x) == -sin(x) (1023)", bad_odd == 0u);
    ck("F1 sin(pi-x) == sin(x) (513)", bad_mirror == 0u);
}

/* --- F2 表对双精度 --------------------------------------------------------- */

static void f2_vs_double(void)
{
    double   worst = 0.0;
    int32_t  dmin = 0, dmax = 0;
    uint32_t i;

    for (i = 0u; i < FX_SIN_TAB_LEN; i++) {
        double ref = 16384.0 * sin(2.0 * M_PI * (double)i / 1024.0);
        double e   = fabs((double)fx_sin_tab[i] - ref);

        if (e > worst)
            worst = e;
    }
    ck_max("F2 sin_tab vs double (LSB, 1280)", worst, 1.0, 0);

    /* sin² + cos² 用整数算：两项各不超过 2^28，和不超过 2^29，int32 放得下。
       按 ±0.5 LSB 取整时偏差的上界是 16384·√2 + 0.5 约 23170.97。 */
    for (i = 0u; i < FX_SIN_N; i++) {
        int32_t s = fx_sin_tab[i];
        int32_t c = fx_sin_tab[i + FX_SIN_TAIL];
        int32_t d = s * s + c * c - (FX_SIN_ONE * FX_SIN_ONE);

        if (i == 0u || d < dmin)
            dmin = d;
        if (i == 0u || d > dmax)
            dmax = d;
    }
    {
        char     line[160];
        core_out o;
        int      ok = (dmin >= -23171) && (dmax <= 23171);

        core_out_init(&o, line, (uint32_t)sizeof line);
        core_out_str(&o, "F2 sin^2+cos^2-2^28 range [");
        out_i32(&o, dmin);
        core_out_str(&o, ", ");
        out_i32(&o, dmax);
        core_out_str(&o, "] bound 23171");
        core_out_str(&o, ok ? " ok\n" : " FAIL\n");
        say(line);
        if (!ok)
            g_fail++;
    }
}

/* --- F3 任意 BAM 查表 ------------------------------------------------------ */

static void f3_lookup(void)
{
    double   worst = 0.0;
    uint32_t a;

    for (a = 0u; a < 65536u; a++) {
        double r  = 2.0 * M_PI * (double)a / 65536.0;
        double es = fabs((double)fx_sin((uint16_t)a) - 16384.0 * sin(r));
        double ec = fabs((double)fx_cos((uint16_t)a) - 16384.0 * cos(r));

        if (es > worst)
            worst = es;
        if (ec > worst)
            worst = ec;
    }
    /* 半格 32 BAM 乘最大斜率 16384·2π/65536 = π/2 LSB/BAM，约 50.27，再加表本身的 0.5 */
    ck_max("F3 sin/cos any BAM vs double (LSB, 65536x2)", worst, 51.0, 0);

    ck("F3 index rounding 31->0 32->1 95->1 96->2",
       fx_bam_index(31u) == 0u && fx_bam_index(32u) == 1u
       && fx_bam_index(95u) == 1u && fx_bam_index(96u) == 2u);
    ck("F3 index wraps 65503->1023 65504->0",
       fx_bam_index(65503u) == 1023u && fx_bam_index(65504u) == 0u);
}

/* --- F4 atan2 穷举 --------------------------------------------------------- */

static void f4_atan2_small(void)
{
    double   worst = 0.0;
    uint32_t n = 0u;
    int32_t  x, y;

    for (y = -512; y <= 511; y++) {
        for (x = -512; x <= 511; x++) {
            double e;

            if (x == 0 && y == 0)
                continue;
            e = bam_err(fx_atan2(y, x), (double)y, (double)x);
            if (e > worst)
                worst = e;
            n++;
        }
    }
    ck("F4 atan2 exhaustive count 1048575", n == 1048575u);
    ck_max("F4 atan2 [-512,511]^2 vs double (BAM)", worst, 64.0, 1);
    ck("F4 atan2(0,0) == 0", fx_atan2(0, 0) == 0u);
    ck("F4 atan2 axes 0 16384 32768 49152",
       fx_atan2(0, 5) == 0u && fx_atan2(5, 0) == 16384u
       && fx_atan2(0, -5) == 32768u && fx_atan2(-5, 0) == 49152u);
}

/* --- F5 atan2 大范围 ------------------------------------------------------- */

static const int32_t F5_EDGE[] = {
    INT32_MIN, INT32_MIN + 1, -65536, -32768, -32767, -1,
    0, 1, 32767, 32768, 65535, INT32_MAX
};

static void f5_atan2_large(void)
{
    double   worst_q8 = 0.0, worst_full = 0.0, worst_edge = 0.0;
    uint32_t i, j;
    xs32     g;

    /* Q.8 的坐标差：游戏区加边界约 420 像素，Q.8 下约 ±11 万，取 ±2^17 */
    xs32_seed(&g, 0x5EED0007u);
    for (i = 0u; i < 1000000u; i++) {
        int32_t x = (int32_t)(xs32_next(&g) % 262144u) - 131072;
        int32_t y = (int32_t)(xs32_next(&g) % 262144u) - 131072;
        double  e;

        if (x == 0 && y == 0)
            continue;
        e = bam_err(fx_atan2(y, x), (double)y, (double)x);
        if (e > worst_q8)
            worst_q8 = e;
    }
    ck_max("F5 atan2 Q.8 random +-2^17 x1e6 (BAM)", worst_q8, 64.0, 1);

    for (i = 0u; i < 200000u; i++) {
        int32_t x = (int32_t)xs32_next(&g);
        int32_t y = (int32_t)xs32_next(&g);
        double  e;

        if (x == 0 && y == 0)
            continue;
        e = bam_err(fx_atan2(y, x), (double)y, (double)x);
        if (e > worst_full)
            worst_full = e;
    }
    ck_max("F5 atan2 full int32 random x2e5 (BAM)", worst_full, 64.0, 1);

    for (i = 0u; i < (uint32_t)(sizeof F5_EDGE / sizeof F5_EDGE[0]); i++) {
        for (j = 0u; j < (uint32_t)(sizeof F5_EDGE / sizeof F5_EDGE[0]); j++) {
            int32_t y = F5_EDGE[i];
            int32_t x = F5_EDGE[j];
            double  e;

            if (x == 0 && y == 0)
                continue;
            e = bam_err(fx_atan2(y, x), (double)y, (double)x);
            if (e > worst_edge)
                worst_edge = e;
        }
    }
    ck_max("F5 atan2 int32 edge pairs 143 (BAM)", worst_edge, 64.0, 1);
}

/* --- F6 慢速圆环 ----------------------------------------------------------- */

static void f6_ring(void)
{
    double   worst = 0.0, rmin = 1e9, rmax = 0.0;
    uint32_t k;

    for (k = 0u; k < 32u; k++) {
        uint16_t a = (uint16_t)(k * 2048u);
        int32_t  vx, vy, x = 0, y = 0;
        uint32_t f;
        double   th = 2.0 * M_PI * (double)k / 32.0;
        double   px, py, dx, dy, r, e;

        fx_vel(FX_ONE / 2, a, &vx, &vy);        /* 0.5 像素/帧 */
        for (f = 0u; f < 600u; f++) {
            x += vx;
            y += vy;
        }
        px = (double)x / 256.0;
        py = (double)y / 256.0;
        dx = px - 300.0 * cos(th);
        dy = py - 300.0 * sin(th);
        e  = sqrt(dx * dx + dy * dy);
        r  = sqrt(px * px + py * py);
        if (e > worst)
            worst = e;
        if (r < rmin)
            rmin = r;
        if (r > rmax)
            rmax = r;
    }
    ck_max("F6 ring 32 shots 0.5px/f 600f dev (px)", worst, 4.0, 0);
    {
        char     line[128];
        core_out o;

        core_out_init(&o, line, (uint32_t)sizeof line);
        core_out_str(&o, "F6 ring radius range ");
        out_milli(&o, rmin);
        core_out_str(&o, "~");
        out_milli(&o, rmax);
        core_out_str(&o, " (ideal 300)\n");
        say(line);
    }
}

/* --- F7 表与 atan2 的方向约定 ---------------------------------------------- */

static void f7_convention(void)
{
    uint32_t i, worst = 0u;

    for (i = 0u; i < FX_SIN_N; i++) {
        uint16_t got  = fx_atan2(fx_sin_tab[i], fx_sin_tab[i + FX_SIN_TAIL]);
        uint16_t want = (uint16_t)(i * FX_BAM_STEP);
        uint16_t d1   = (uint16_t)(got - want);
        uint16_t d2   = (uint16_t)(want - got);
        uint32_t d    = (d1 < d2) ? d1 : d2;

        if (d > worst)
            worst = d;
    }
    {
        char     line[128];
        core_out o;
        int      ok = worst <= 8u;

        core_out_init(&o, line, (uint32_t)sizeof line);
        core_out_str(&o, "F7 atan2(sin_tab[i], cos_tab[i]) vs i*64 max=");
        core_out_u32(&o, worst);
        core_out_str(&o, " limit<=8");
        core_out_str(&o, ok ? " ok\n" : " FAIL\n");
        say(line);
        if (!ok)
            g_fail++;
    }
}

uint32_t test_fixmath_run(void)
{
    g_fail = 0u;

    say("libTest fixmath: Q.8/BAM/sin_tab/atan2\n");
    _Static_assert(sizeof fx_sin_tab == 2u * FX_SIN_TAB_LEN, "正弦表必须是 1280 项 int16");

    f1_shape();
    f2_vs_double();
    f3_lookup();
    f4_atan2_small();
    f5_atan2_large();
    f6_ring();
    f7_convention();

    return g_fail;
}
