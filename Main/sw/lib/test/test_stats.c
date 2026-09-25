/******************************************************************************
* @file test_stats.c: 统计基础件（SW8 2.2、2.4 节）的主机侧测试
*
* @brief  帧时间直方图（lib/core/framehist），判据见 Engineering/Todo/sw/SW8-演示模式与统计埋点.md
*         第 3 节「帧时间直方图（A 批）」一行：
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
******************************************************************************/

#include <stdio.h>
#include <stdlib.h>

#include "core/framehist.h"
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

uint32_t test_stats_run(void)
{
    g_fail = 0u;

    say("libTest stats: frame-time histogram\n");
    h1_bounds();
    h2_known();
    h3_overflow();
    h4_exact();
    h5_random();

    return g_fail;
}
