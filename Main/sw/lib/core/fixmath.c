/******************************************************************************
* @file fixmath.c: atan2 的整数实现
*
* @brief  照 Coranac《Off on a tangent》的 atan2Tonc：八分象限归约、一次除法、
*         五项多项式。原文的角度单位是 π = 0x4000（一周 2^15），多项式
*         系数 0x0470、0x1029、0x1F0B、0x364C、0xA2FC 算出来的单位是一周 2^18，
*         最后 (dphi + 4) >> 3 取整回一周 2^15。本文件要的是 16 位 BAM（一周 2^16），
*         所以只改两处：八分象限的一格从 0x1000 改成 0x2000，最后一步改成
*         (dphi + 2) >> 2。多项式本身与原文逐项相同。
*
*         原文的被除数是 NDS 除法器上的 64 位，这里的输入又是 Q.8 的坐标差
*         （约 ±11 万），照抄会在两处越出 int32：`y << 15` 与归约第三步的 `x + y`。
*         所以先把两个分量同时算术右移，直到都落在 [-32767, 32767] 之内：
*           - 比较写成 v > 32767 || v < -32767，不取绝对值，INT32_MIN 也走得通；
*           - 缩放之后 x + y <= 65534，y << 15 < 2^30，下面每一步的乘积都小于 2^31；
*           - 只在某一分量不小于 2^15 时才缩放，这时大的那个分量缩完仍不小于 2^14，
*             丢掉的相对精度不到 2^-14，角度误差不到 1 BAM。
*         缩放与归约的规则见 Engineering/TechReports/sw/06-游戏本体.md 3.4 节。
*
*         只在发射时调用，更新循环里没有除法（sw 第 06 章第五节的 G11）。
******************************************************************************/

#include "core/fixmath.h"

/* 多项式在 t = y/x 上的定点位数，原文的 fixShift。t 落在 [0, 1) 里，即 [0, 2^15)。 */
#define ATAN_FP         15

/* 16 位 BAM 下八分之一周。原文（一周 2^15）是 0x1000。 */
#define BAM_OCTANT      0x2000u

uint16_t fx_atan2(int32_t y, int32_t x)
{
    int32_t  t, t2, dphi, tmp;
    uint32_t oct = 0u;

#ifndef INJECT_FX_ATAN_NOSCALE
    while (x > 32767 || x < -32767 || y > 32767 || y < -32767) {
        x >>= 1;
        y >>= 1;
    }
#endif

    /* 原文的第一句：y 为 0 时直接给 0 或 π。(0, 0) 也落在这里，返回 0。 */
    if (y == 0)
        return (x >= 0) ? 0u : 0x8000u;

    /* OCTANTIFY：三步各把向量转过 π、π/2、π/4，oct 记下转过多少个八分之一周。
       走完之后 0 <= y < x，t = y / x 落在 [0, 1)。 */
    if (y < 0) {
        x = -x;
        y = -y;
        oct += 4u;
    }
    if (x <= 0) {
        tmp = x;
        x   = y;
        y   = -tmp;
        oct += 2u;
    }
    if (x <= y) {
        tmp = y - x;
        x   = x + y;
        y   = tmp;
        oct += 1u;
    }

    t  = (y << ATAN_FP) / x;
    t2 = -t * t >> ATAN_FP;                 /* 原文写法：(-t) * t，再算术右移 */

    dphi = 0x0470;
    dphi = 0x1029 + (t2 * dphi >> ATAN_FP);
    dphi = 0x1F0B + (t2 * dphi >> ATAN_FP);
    dphi = 0x364C + (t2 * dphi >> ATAN_FP);
    dphi = 0xA2FC + (t2 * dphi >> ATAN_FP);
    dphi = dphi * t >> ATAN_FP;             /* 一周 2^18 */

#ifdef INJECT_FX_ATAN_UNIT
    /* 注错：最后一步留在原文的一周 2^15，而八分象限已经是 16 位的一格 */
    return (uint16_t)(oct * BAM_OCTANT + (uint32_t)((dphi + 4) >> 3));
#else
    /* 加起来可能恰好是 65536（紧贴 2π），截成 uint16 就是 0，正是同一个方向。 */
    return (uint16_t)(oct * BAM_OCTANT + (uint32_t)((dphi + 2) >> 2));
#endif
}
