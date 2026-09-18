/******************************************************************************
* @file xorshift32.c: 自带随机数
*
* @brief  经典 32 位 xorshift：x ^= x << 13; x ^= x >> 17; x ^= x << 5。
*
*         三个移位常数在这里集中定义，SW1 的跨平台判据用它们做注错：
*         `-DINJECT_XS32_SHIFT` 把第一个常数从 13 改成 12，
*         同一个种子的序列从第 0 帧起就与正常版不同，
*         用来证明「60 个 CRC 相同」这条判据不是空转。
******************************************************************************/

#include "core/xorshift32.h"

#ifdef INJECT_XS32_SHIFT
#define XS32_SHIFT_A    12u     /* 注错：正常值 13 */
#else
#define XS32_SHIFT_A    13u
#endif

#define XS32_SHIFT_B    17u
#define XS32_SHIFT_C    5u

void xs32_seed(xs32 *g, uint32_t seed)
{
    g->s = (seed == 0u) ? 1u : seed;
}

uint32_t xs32_next(xs32 *g)
{
    uint32_t x = g->s;

    x ^= x << XS32_SHIFT_A;
    x ^= x >> XS32_SHIFT_B;
    x ^= x << XS32_SHIFT_C;
    g->s = x;
    return x;
}
