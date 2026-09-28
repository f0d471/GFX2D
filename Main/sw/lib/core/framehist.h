/******************************************************************************
* @file framehist.h: 帧时间直方图
*
* @brief  sw 第 07 章 3.4 节。固定 256 个线性桶覆盖 0~2 场，由它给出最大值与 99 分位，
*         不存逐帧样本（HdrHistogram 的做法，但 0~2 场的范围里线性分桶就够）。
*
*         口径（记一场的周期数为 F）：
*           - F 是参数，初始化时给，不写死成 1/60 秒：场周期以板上实测为准，
*             PC 上用模拟时钟时就是模型里的那个数。
*           - 周期数 t 落进第 b = floor(t * 128 / F) 个桶，b >= 255 的都进第 255 个桶。
*             第 b 个桶覆盖 [ceil(b*F/128), ceil((b+1)*F/128))；最后一个桶另收所有不小于 2 场的样本。
*           - 最大值另存精确值。
*           - 分位数报桶的上界（偏保守）：秩 r = ceil(num * n / den)，累加到不小于 r 的那个桶 b，
*             报 min(ceil((b+1)*F/128) - 1, 最大值)；落在最后一个桶时它没有上界，报最大值。
*             报出的数不小于真实的分位数，也不超过最大值。
*
*         每加一个样本做一次 64 位除法（板上是 libgcc 的库函数），只在帧外做，不进帧时间。
*
*         注错开关（主机测试 lib/test/test_stats.c 用，见 check_sw8_stats.sh）：
*           INJECT_FHIST_P99_LOWER  分位数报桶的下界
*           INJECT_FHIST_DROP_OVER  不小于 2 场的样本不进最后一个桶
******************************************************************************/
#ifndef GFX2D_CORE_FRAMEHIST_H
#define GFX2D_CORE_FRAMEHIST_H

#include <stdint.h>

#define FHIST_BUCKETS   256u

typedef struct {
    uint32_t field_cyc;                 /* 一场的周期数 F，>= 1 */
    uint32_t n;                         /* 样本数 */
    uint32_t max;                       /* 精确的最大值 */
    uint32_t count[FHIST_BUCKETS];
} fhist;

/* 初始化并清空。field_cyc 为 0 时按 1 处理，免得除以零。 */
void     fhist_init(fhist *h, uint32_t field_cyc);

/* 清空样本，保留 F。切模式时统计清零用它（sw 第 07 章 3.7 节）。 */
void     fhist_clear(fhist *h);

void     fhist_add(fhist *h, uint32_t cyc);

/* 周期数 t 落进哪个桶。 */
uint32_t fhist_bucket(const fhist *h, uint32_t cyc);

/* 第 b 个桶里最小与最大的周期数（闭区间）。最后一个桶的上界没有意义，返回 0xFFFFFFFF。 */
uint32_t fhist_bucket_lo(const fhist *h, uint32_t b);
uint32_t fhist_bucket_hi(const fhist *h, uint32_t b);

/* num/den 分位数的保守上界，例如 99 分位是 (99, 100)。没有样本时返回 0。 */
uint32_t fhist_quantile(const fhist *h, uint32_t num, uint32_t den);

uint32_t fhist_max(const fhist *h);
uint32_t fhist_count(const fhist *h);

#endif /* GFX2D_CORE_FRAMEHIST_H */
