/******************************************************************************
* @file test_stats.c: 统计基础件（帧时间直方图与日志队列）的主机侧测试
*
* @brief  帧时间直方图（lib/core/framehist）的判据：
*           H1  桶的边界：与「满足 t·128 >= b·F 的最小 t」二分查找的结果逐个相同（不用 ceil 公式）；
*               桶边界两侧各一例；不小于 2 场的样本进最后一个桶
*           H2  已知分布：最大值精确，99 分位等于独立算出的桶上界
*           H3  99 分位落在最后一个桶时报最大值；越界样本都进了最后一个桶
*           H4  最大值不取桶边界；没有样本时两个量都是 0
*           H5  随机一万个样本：报出的 50、99 分位不小于排序得到的真值、不超过最大值，
*               与真值之差小于一个桶宽
*
*         注错开关见 lib/core/framehist.h，每个都必须让这里至少一行 FAIL：
*           FHIST_P99_LOWER -> H2、H5
*           FHIST_DROP_OVER -> H3
*
*         日志队列（lib/core/logq）的判据：
*           Q1  满了整行丢弃并计数，比容量还长的行永远丢；空串不算一行
*           Q2  手算的回绕：跨过存储末端写入，取出的字节顺序不变
*           Q3  随机两万步（随机行长 1~80、随机取数额度 0~100）对一份**线性**参考：
*               参考是一段不回绕的长缓冲加读写两个下标，与环形缓冲不是同一段逻辑；
*               每次取出的字节、已用字节数与丢弃行数都逐步相同
*           Q4  取 0 个字节、从空队列取：返回 0，输出仍以 '\0' 结尾
*
*         注错开关见 lib/core/logq.h：
*           LOGQ_SPLIT -> Q1、Q3
*           LOGQ_WRAP  -> Q2、Q3
******************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/framehist.h"
#include "core/logq.h"
#include "core/format.h"
#include "core/xorshift32.h"
#include "test_stats.h"

static uint32_t g_fail;

static void say(const char *s)
{
    fputs(s, stdout);
}

static void ck(const char *tag, int ok)
{
    char     line[200];
    core_out o;

    core_out_init(&o, line, (uint32_t)sizeof line);
    core_out_str(&o, tag);
    core_out_str(&o, ok ? " ok\n" : " FAIL\n");
    say(line);
    if (!ok)
        g_fail++;
}

/* 第 b 个桶的第一个周期数：满足 t·128 >= b·F 的最小 t。二分查找，照定义算，不用 ceil 公式。 */
static uint64_t ref_first(uint32_t F, uint32_t b)
{
    uint64_t lo = 0u, hi = 0x100000000ull;

    while (lo < hi) {
        uint64_t mid = (lo + hi) / 2u;

        if (mid * 128u >= (uint64_t)b * F)
            hi = mid;
        else
            lo = mid + 1u;
    }
    return lo;
}

/* t 落在哪个桶：照定义线性找，最后一个桶收下其余。 */
static uint32_t ref_bucket(uint32_t F, uint32_t t)
{
    uint32_t b;

    for (b = 0u; b < FHIST_BUCKETS - 1u; b++)
        if ((uint64_t)t < ref_first(F, b + 1u))
            return b;
    return FHIST_BUCKETS - 1u;
}

/* --- H1 桶的边界 --------------------------------------------------------- */

static void h1_bounds(void)
{
    static const uint32_t FS[] = { 1666667u, 1668335u, 128000u, 128u };
    static const uint32_t BS[] = { 0u, 1u, 2u, 63u, 127u, 128u, 129u, 200u, 254u, 255u };
    uint32_t              i, j, bad = 0u, n = 0u;

    for (i = 0u; i < (uint32_t)(sizeof FS / sizeof FS[0]); i++) {
        fhist h;

        fhist_init(&h, FS[i]);
        for (j = 0u; j < (uint32_t)(sizeof BS / sizeof BS[0]); j++) {
            uint32_t b  = BS[j];
            uint32_t lo = fhist_bucket_lo(&h, b);

            n++;
            if ((uint64_t)lo != ref_first(FS[i], b))
                bad++;
            if (fhist_bucket(&h, lo) != b)
                bad++;
            if (b > 0u && fhist_bucket(&h, lo - 1u) != b - 1u)
                bad++;
            if (b < FHIST_BUCKETS - 1u) {
                uint32_t hi = fhist_bucket_hi(&h, b);

                if ((uint64_t)hi + 1u != ref_first(FS[i], b + 1u))
                    bad++;
                if (fhist_bucket(&h, hi) != b || fhist_bucket(&h, hi + 1u) != b + 1u)
                    bad++;
            }
        }
        /* 不小于 2 场的都进最后一个桶；2F - 1 正好是最后一个桶自己的范围 */
        if (fhist_bucket(&h, 2u * FS[i]) != 255u || fhist_bucket(&h, 2u * FS[i] - 1u) != 255u
         || fhist_bucket(&h, 0xFFFFFFFFu) != 255u)
            bad++;
    }
    {
        char     line[120];
        core_out o;

        core_out_init(&o, line, (uint32_t)sizeof line);
        core_out_str(&o, "H1 bucket bounds vs bisection of t*128 >= b*F (4 F x 10 b = ");
        core_out_u32(&o, n);
        core_out_str(&o, "), >= 2 fields -> 255");
        ck(line, bad == 0u);
    }
}

/* --- H2/H3/H4 已知分布 --------------------------------------------------- */

#define H_F     1666667u

static void h2_known(void)
{
    fhist    h;
    uint32_t i, b, want;
    char     line[160];
    core_out o;

    fhist_init(&h, H_F);
    for (i = 0u; i < 990u; i++)
        fhist_add(&h, 1000000u);
    for (i = 0u; i < 9u; i++)
        fhist_add(&h, 1500000u);
    fhist_add(&h, 4000000u);                    /* 2.4 场，进最后一个桶 */

    /* 秩 990 落在 1000000 那个桶：期望是那个桶的上界，照定义另算 */
    b    = ref_bucket(H_F, 1000000u);
    want = (uint32_t)(ref_first(H_F, b + 1u) - 1u);
    if (want > 4000000u)
        want = 4000000u;

    core_out_init(&o, line, (uint32_t)sizeof line);
    core_out_str(&o, "H2 990x1.0M 9x1.5M 1x4.0M: p99 ");
    core_out_u32(&o, fhist_quantile(&h, 99u, 100u));
    core_out_str(&o, " == bucket ");
    core_out_u32(&o, b);
    core_out_str(&o, " upper ");
    core_out_u32(&o, want);
    ck(line, fhist_quantile(&h, 99u, 100u) == want);
    ck("H2 max exact 4000000, n 1000", fhist_max(&h) == 4000000u && fhist_count(&h) == 1000u);
}

static void h3_overflow(void)
{
    fhist    h;
    uint32_t i;

    fhist_init(&h, H_F);
    for (i = 0u; i < 980u; i++)
        fhist_add(&h, 1000000u);
    for (i = 0u; i < 20u; i++)
        fhist_add(&h, 5000000u + i);            /* 3 场出头，进最后一个桶 */
    ck("H3 p99 in last bucket -> max 5000019; last bucket holds all 20 overflow samples",
       fhist_quantile(&h, 99u, 100u) == 5000019u && h.count[FHIST_BUCKETS - 1u] == 20u);
}

static void h4_exact(void)
{
    fhist h;

    fhist_init(&h, H_F);
    ck("H4 empty: p99 0, max 0", fhist_quantile(&h, 99u, 100u) == 0u && fhist_max(&h) == 0u);
    fhist_add(&h, 1234567u);
    fhist_add(&h, 17u);
    ck("H4 max is the exact sample 1234567, not a bucket bound", fhist_max(&h) == 1234567u);
    fhist_clear(&h);
    ck("H4 clear keeps F, drops samples", h.field_cyc == H_F && fhist_count(&h) == 0u);
}

/* --- H5 随机样本与排序真值 ----------------------------------------------- */

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;

    return (x > y) - (x < y);
}

#define H5_N    10000u

static void h5_random(void)
{
    static uint32_t v[H5_N];
    fhist           h;
    xs32            g;
    uint32_t        i, q, bad = 0u;
    static const uint32_t QS[] = { 50u, 99u };

    fhist_init(&h, H_F);
    xs32_seed(&g, 0x4157A001u);
    for (i = 0u; i < H5_N; i++) {
        /* 大多在一场以内，约 3% 超过一场，约 0.5% 超过两场 */
        uint32_t r = xs32_next(&g) % 1000u;

        if (r < 5u)
            v[i] = 2u * H_F + xs32_next(&g) % H_F;
        else if (r < 35u)
            v[i] = H_F + xs32_next(&g) % H_F;
        else
            v[i] = 300000u + xs32_next(&g) % 1200000u;
        fhist_add(&h, v[i]);
    }
    qsort(v, H5_N, sizeof v[0], cmp_u32);

    for (q = 0u; q < 2u; q++) {
        uint32_t rank = (QS[q] * H5_N + 99u) / 100u;
        uint32_t truth = v[rank - 1u];
        uint32_t got = fhist_quantile(&h, QS[q], 100u);
        uint32_t width = (H_F + 127u) / 128u;

        if (got < truth || got > fhist_max(&h))
            bad++;
        if (fhist_bucket(&h, truth) < FHIST_BUCKETS - 1u && got - truth >= width)
            bad++;
    }
    ck("H5 10000 random: p50/p99 >= sorted truth, <= max, within one bucket", bad == 0u);
}

/* --- Q 日志队列 --------------------------------------------------------- */

static void q1_full(void)
{
    static char mem[64];
    logq        q;
    char        line[128];
    uint32_t    i;
    int         r1, r2, r3, r4, r5, r6, r7;

    logq_init(&q, mem, 64u);
    memset(line, 'a', sizeof line);
    line[20] = '\0';                            /* 20 字节 */
    r1 = logq_put(&q, line);
    r2 = logq_put(&q, line);
    r3 = logq_put(&q, line);                    /* 60 */
    r4 = logq_put(&q, line);                    /* 放不下：整行丢 */
    line[4] = '\0';
    r5 = logq_put(&q, line);                    /* 4 字节正好放下：64 */
    line[1] = '\0';
    r6 = logq_put(&q, line);                    /* 满了：丢 */
    ck("Q1 3x20 in, 4th dropped, 4-byte fits exactly, then 1 byte dropped: used 64 dropped 2",
       r1 == 0 && r2 == 0 && r3 == 0 && r4 == -1 && r5 == 0 && r6 == -1
       && logq_used(&q) == 64u && logq_dropped(&q) == 2u && logq_free(&q) == 0u);

    logq_init(&q, mem, 64u);
    for (i = 0u; i < 100u; i++)
        line[i] = 'b';
    line[100] = '\0';
    r7 = logq_put(&q, line);
    ck("Q1 line longer than capacity always dropped, empty string is a no-op",
       r7 == -1 && logq_dropped(&q) == 1u && logq_used(&q) == 0u
       && logq_put(&q, "") == 0 && logq_used(&q) == 0u && logq_dropped(&q) == 1u);
}

static void q2_wrap(void)
{
    static char mem[16];
    logq        q;
    char        out[32];
    uint32_t    n1, n2;

    logq_init(&q, mem, 16u);
    (void)logq_put(&q, "abcdefghij");           /* 0..9 */
    n1 = logq_take(&q, out, 8u);                /* 取走 abcdefgh，head = 8 */
    ck("Q2 first take 8: abcdefgh", n1 == 8u && strcmp(out, "abcdefgh") == 0);
    (void)logq_put(&q, "0123456789");           /* 写在 10..15 与 0..3，跨过末端 */
    n2 = logq_take(&q, out, 31u);
    ck("Q2 across the end: ij0123456789, queue empty",
       n2 == 12u && strcmp(out, "ij0123456789") == 0 && logq_used(&q) == 0u);
}

#define Q3_CAP      256u
#define Q3_STEPS    20000u
#define Q3_REF_MAX  (Q3_STEPS * 82u)

static void q3_random(void)
{
    static char mem[Q3_CAP];
    static char ref[Q3_REF_MAX];                /* 线性参考：只追加，不回绕 */
    char        line[96], out[128];
    logq        q;
    xs32        g;
    uint32_t    k, rd = 0u, wr = 0u, ref_drop = 0u, bad = 0u;
    uint32_t    puts_ok = 0u, took = 0u, wraps = 0u, last_head = 0u;

    logq_init(&q, mem, Q3_CAP);
    xs32_seed(&g, 0x106A0001u);

    for (k = 0u; k < Q3_STEPS; k++) {
        if (xs32_next(&g) % 3u != 0u) {
            uint32_t len = 1u + xs32_next(&g) % 80u, i;
            int      rc;

            for (i = 0u; i + 2u < len; i++)
                line[i] = (char)('A' + (k + i) % 26u);
            if (len >= 2u) {
                line[len - 2u] = '\r';
                line[len - 1u] = '\n';
            } else {
                line[0] = (char)('a' + k % 26u);
            }
            line[len] = '\0';

            rc = logq_put(&q, line);
            if (len <= Q3_CAP - (wr - rd)) {    /* 参考：放得下就整行追加 */
                memcpy(ref + wr, line, len);
                wr += len;
                if (rc != 0)
                    bad++;
                puts_ok++;
            } else {
                ref_drop++;
                if (rc != -1)
                    bad++;
            }
        } else {
            uint32_t budget = xs32_next(&g) % 101u;
            uint32_t want = (budget < wr - rd) ? budget : wr - rd;
            uint32_t n = logq_take(&q, out, budget);

            if (n != want || memcmp(out, ref + rd, n) != 0 || out[n] != '\0')
                bad++;
            rd += want;
            took += n;
            if (q.head < last_head)
                wraps++;
            last_head = q.head;
        }
        if (logq_used(&q) != wr - rd || logq_dropped(&q) != ref_drop)
            bad++;
    }
    /* 收尾：全部取完，逐字节对 */
    while (wr - rd > 0u) {
        uint32_t n = logq_take(&q, out, 100u);

        if (n == 0u || memcmp(out, ref + rd, n) != 0) {
            bad++;
            break;
        }
        rd += n;
        took += n;
    }

    {
        char     msg[160];
        core_out o;

        core_out_init(&o, msg, (uint32_t)sizeof msg);
        core_out_str(&o, "Q3 20000 steps vs linear reference: lines in ");
        core_out_u32(&o, puts_ok);
        core_out_str(&o, " dropped ");
        core_out_u32(&o, ref_drop);
        core_out_str(&o, " bytes out ");
        core_out_u32(&o, took);
        core_out_str(&o, " wraps ");
        core_out_u32(&o, wraps);
        ck(msg, bad == 0u && ref_drop > 0u && wraps > 0u);
    }
}

static void q4_edges(void)
{
    static char mem[8];
    logq        q;
    char        out[4];

    logq_init(&q, mem, 8u);
    out[0] = 'x';
    ck("Q4 take from empty -> 0, out is \"\"", logq_take(&q, out, 3u) == 0u && out[0] == '\0');
    (void)logq_put(&q, "hi");
    out[0] = 'x';
    ck("Q4 take 0 bytes -> 0, nothing consumed", logq_take(&q, out, 0u) == 0u && out[0] == '\0'
       && logq_used(&q) == 2u);
}

uint32_t test_stats_run(void)
{
    g_fail = 0u;

    say("libTest stats: frame-time histogram, log queue\n");
    h1_bounds();
    h2_known();
    h3_overflow();
    h4_exact();
    h5_random();
    q1_full();
    q2_wrap();
    q3_random();
    q4_edges();

    return g_fail;
}
