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
*
*         只在发射时调用，更新循环里没有除法。
******************************************************************************/

#include "core/fixmath.h"
#include <limits.h>

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

/* Q16.16 部分自研；乘积用 int64，rv32im 的 gcc 降成 mul/mulh。
   矩阵逐项舍入再饱和相加，避免四个极值乘积相加越出 int64。 */
int64_t fx16_floor_div(int64_t n, int64_t d)
{
    if (d == 16) return n >> 4;
    if (d == 65536) return n >> 16;
    int64_t q = n / d;
    return q - (n % d < 0);
}

int64_t fx16_round_div(int64_t n, int64_t d)
{
    /* Quotient plus a nonnegative remainder avoids n+d/2 overflow at s64 edges.
       GCC's arithmetic right shift is the same floor operation used above. */
    if (d == 65536) return (n >> 16) + (((uint64_t)n & 65535u) >= 32768u);
    if (d == 131072) return (n >> 17) + (((uint64_t)n & 131071u) >= 65536u);
    if (!n) return 0;
    if (n >= INT32_MIN && n <= INT32_MAX && d <= INT32_MAX) {
        int32_t q32 = (int32_t)n / (int32_t)d;
        int32_t r32 = (int32_t)n % (int32_t)d;
        if (r32 < 0) { --q32; r32 += (int32_t)d; }
        return (int64_t)q32 + (r32 >= d - d / 2);
    }
    int64_t q = n / d, rem = n % d;
    if (rem < 0) { --q; rem += d; }
    return q + (rem >= d - d / 2);
}

fx16_t fx16_sat(int64_t x)
{
    return x > INT32_MAX ? INT32_MAX : x < INT32_MIN ? INT32_MIN : (int32_t)x;
}

fx16_t fx16_mul(fx16_t a, fx16_t b)
{
    return fx16_sat(fx16_round_div((int64_t)a * b, FX16_ONE));
}

fx16_t fx16_div(fx16_t a, fx16_t b)
{
    uint64_t n, q = 0, rem = 0;
    uint32_t den;
    int neg = (a < 0) != (b < 0);
    if (!b) return a > 0 ? INT32_MAX : a < 0 ? INT32_MIN : 0;
    n = (uint64_t)(a < 0 ? -(int64_t)a : a) * FX16_ONE;
    den = (uint32_t)(b < 0 ? -(int64_t)b : b);
    if (!(den & (den - 1u))) {
        q = n >> (unsigned)__builtin_ctz(den);
        rem = n & (den - 1u);
    } else if (n <= UINT32_MAX) {
        q = (uint32_t)n / den;
        rem = (uint32_t)n % den;
    } else {
        uint32_t hi = (uint32_t)(n >> 32), lo = (uint32_t)n, quotient = 0;
        uint32_t remainder;
        int top;
        /* A quotient >=2^32 saturates for either sign before rounding.
           Otherwise only quotient bits are divided, with a u32 remainder:
           den<=2^31, so remainder*2+bit never overflows u32. */
        if (hi >= den) return neg ? INT32_MIN : INT32_MAX;
        top = 32 + __builtin_clz(den) - __builtin_clz(hi);
        if (top > 31) top = 31;
        remainder = (uint32_t)(n >> (top + 1));
        for (int i = top; i >= 0; --i) {
            remainder = remainder * 2u + ((lo >> i) & 1u);
            if (remainder >= den) { remainder -= den; quotient |= 1u << i; }
        }
        q = quotient;
        rem = remainder;
    }
    q += neg ? rem * 2 > den : rem * 2 >= den;
    return fx16_sat(neg ? -(int64_t)q : (int64_t)q);
}

static const uint32_t recip_seed[1024] = {
#include "recip_tab.inc"
};

fx16_t fx16_recip(fx16_t a)
{
    uint32_t mag, norm, y;
    int exp = 0;
    int64_t result;
    if (!a) return INT32_MAX;
    mag = (uint32_t)(a < 0 ? -(int64_t)a : a);
    for (uint32_t t = mag; t > 1; t >>= 1) ++exp;
    norm = exp <= 30 ? mag << (30 - exp) : mag >> (exp - 30);
    y = recip_seed[(norm - (1u << 30)) >> 20];
    y = (uint32_t)(((uint64_t)y * ((UINT64_C(2) << 30) -
         (((uint64_t)norm * y) >> 30))) >> 30);
    result = exp <= 2 ? (int64_t)y * (1u << (2 - exp)) :
             fx16_round_div(y, INT64_C(1) << (exp - 2));
    /* 一次 Newton 约 20 位有效精度；小除数的 raw 误差会被放大。
       用整数余数修回精确结果（最多 1024 步），不再调用除法器。 */
    int64_t rem = (INT64_C(1) << 32) - result * mag;
    while (rem < 0) { --result; rem += mag; }
    while (rem >= mag) { ++result; rem -= mag; }
    result += a < 0 ? rem * 2 > mag : rem * 2 >= mag;
    return fx16_sat(a < 0 ? -result : result);
}

void fx16_identity(fx16_mat4 *m)
{
    for (int i = 0; i < 16; ++i) m->v[i] = i % 5 == 0 ? FX16_ONE : 0;
}

void fx16_mat_vec(const fx16_mat4 *m, const fx16_vec4 *v, fx16_vec4 *out)
{
    fx16_vec4 t;
    for (int i = 0; i < 4; ++i) {
        int64_t sum = 0;
        for (int j = 0; j < 4; ++j) {
            fx16_t a = m->v[i * 4 + j], b = v->v[j];
            sum += !a || !b ? 0 : a == FX16_ONE ? b :
                   b == FX16_ONE ? a : fx16_mul(a, b);
        }
        t.v[i] = fx16_sat(sum);
    }
    *out = t;
}

void fx16_mat_mul(const fx16_mat4 *a, const fx16_mat4 *b, fx16_mat4 *out)
{
    fx16_mat4 t;
    for (int j = 0; j < 4; ++j) {
        fx16_vec4 col, result;
        for (int i = 0; i < 4; ++i) col.v[i] = b->v[i * 4 + j];
        fx16_mat_vec(a, &col, &result);
        for (int i = 0; i < 4; ++i) t.v[i * 4 + j] = result.v[i];
    }
    *out = t;
}

int fx16_perspective(fx16_mat4 *m, uint16_t fov_y, fx16_t aspect,
                     fx16_t near_z, fx16_t far_z)
{
    fx16_t cot, span;
    if (fov_y < 128 || fov_y >= 32768 || aspect <= 0 ||
        near_z <= 0 || far_z <= near_z) return -1;
    span = far_z - near_z;
    cot = fx16_div(fx_cos(fov_y / 2), fx_sin(fov_y / 2));
    for (int i = 0; i < 16; ++i) m->v[i] = 0;
    m->v[0] = fx16_div(cot, aspect);
    m->v[5] = cot;
    m->v[10] = fx16_sat(fx16_round_div(-((int64_t)far_z + near_z) * FX16_ONE, span));
    m->v[11] = fx16_sat(fx16_round_div(-(int64_t)2 * far_z * near_z, span));
    m->v[14] = -FX16_ONE;
    return 0;
}

int32_t fx16_viewport(fx16_t ndc, int32_t extent, int flip)
{
    int64_t n = flip ? (int64_t)FX16_ONE - ndc : (int64_t)FX16_ONE + ndc;
    return fx16_sat(fx16_round_div(n * extent * 8, FX16_ONE));
}
