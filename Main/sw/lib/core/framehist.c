/******************************************************************************
* @file framehist.c: 帧时间直方图
*
* @brief  口径见 framehist.h 与 SW8 2.2 节。纯整数，两个平台编同一份。
******************************************************************************/

#include "core/framehist.h"

#define FHIST_LAST      (FHIST_BUCKETS - 1u)
#define FHIST_PER_FIELD 128u            /* 256 个桶覆盖 2 场 */

void fhist_clear(fhist *h)
{
    uint32_t b;

    h->n   = 0u;
    h->max = 0u;
    for (b = 0u; b < FHIST_BUCKETS; b++)
        h->count[b] = 0u;
}

void fhist_init(fhist *h, uint32_t field_cyc)
{
    h->field_cyc = (field_cyc == 0u) ? 1u : field_cyc;
    fhist_clear(h);
}

uint32_t fhist_bucket(const fhist *h, uint32_t cyc)
{
    uint64_t b = ((uint64_t)cyc * FHIST_PER_FIELD) / h->field_cyc;

    return (b > FHIST_LAST) ? FHIST_LAST : (uint32_t)b;
}

/* ceil(b * F / 128)：第 b 个桶的第一个周期数。b 最大 256、F 小于 2^32，乘积在 64 位里。 */
uint32_t fhist_bucket_lo(const fhist *h, uint32_t b)
{
    uint64_t lo = ((uint64_t)b * h->field_cyc + (FHIST_PER_FIELD - 1u)) / FHIST_PER_FIELD;

    return (lo > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)lo;
}

uint32_t fhist_bucket_hi(const fhist *h, uint32_t b)
{
    if (b >= FHIST_LAST)
        return 0xFFFFFFFFu;
    return fhist_bucket_lo(h, b + 1u) - 1u;
}

void fhist_add(fhist *h, uint32_t cyc)
{
#ifdef INJECT_FHIST_DROP_OVER
    /* 注错：不小于 2 场的样本丢掉，不进最后一个桶 */
    if ((uint64_t)cyc * FHIST_PER_FIELD < (uint64_t)FHIST_BUCKETS * h->field_cyc)
        h->count[fhist_bucket(h, cyc)]++;
#else
    h->count[fhist_bucket(h, cyc)]++;
#endif
    h->n++;
    if (cyc > h->max)
        h->max = cyc;
}

uint32_t fhist_quantile(const fhist *h, uint32_t num, uint32_t den)
{
    uint64_t rank;
    uint32_t b, cum = 0u;

    if (h->n == 0u || den == 0u)
        return 0u;

    /* 秩 r = ceil(num * n / den)，至少是 1 */
    rank = ((uint64_t)num * h->n + den - 1u) / den;
    if (rank == 0u)
        rank = 1u;

    for (b = 0u; b < FHIST_BUCKETS; b++) {
        cum += h->count[b];
        if (cum >= rank) {
            uint32_t hi;

            if (b == FHIST_LAST)
                return h->max;          /* 最后一个桶没有上界 */
#ifdef INJECT_FHIST_P99_LOWER
            hi = fhist_bucket_lo(h, b); /* 注错：报桶的下界，偏乐观 */
#else
            hi = fhist_bucket_hi(h, b);
#endif
            return (hi < h->max) ? hi : h->max;
        }
    }
    return h->max;                      /* 计数与 n 不一致时（只有注错会走到）按最大值报 */
}

uint32_t fhist_max(const fhist *h)
{
    return h->max;
}

uint32_t fhist_count(const fhist *h)
{
    return h->n;
}
