/******************************************************************************
* @file xorshift32.h: 自带随机数
*
* @brief  确定性：逻辑不调 rand()、不读墙钟，
*         同一种子在同一平台的每一步都相同，PC 与板子上也必须相同。
*
*         xorshift32 是纯 32 位整数运算，不依赖任何平台特性，
*         两个平台编同一份源码就必然得到同一串数。
******************************************************************************/
#ifndef GFX2D_CORE_XORSHIFT32_H
#define GFX2D_CORE_XORSHIFT32_H

#include <stdint.h>

typedef struct {
    uint32_t s;
} xs32;

/* 种子为 0 时内部改成 1：xorshift 的全零状态是不动点，会把随机数卡死。 */
void     xs32_seed(xs32 *g, uint32_t seed);

/* 推进一步并返回新状态。 */
uint32_t xs32_next(xs32 *g);

#endif /* GFX2D_CORE_XORSHIFT32_H */
