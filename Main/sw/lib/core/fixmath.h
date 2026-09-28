/******************************************************************************
* @file fixmath.h: 定点数、二进制角、正弦表与 atan2
*
* @brief  SW7 的数值基础，口径见 Engineering/TechReports/sw/06-游戏本体.md 3.4 节。
*         纯整数（SW1-SW9 总览约束 2），两个平台编同一份，同一输入得到同一结果。
*
*         四样东西：
*           Q.8      位置与速度都是 int32，低 8 位小数，1 像素 = 256。
*                    Q12.4 画不圆慢速圆环，数值实验见 sw 第 06 章 3.2 节。
*           BAM      角度是 uint16，65536 为一周，加减自然回绕，不取模。
*                    方向约定：角 a 对应 (cos a, sin a)，0 指向 +x；屏幕 y 向下，
*                    所以角度增大在屏幕上是顺时针。
*           正弦表   1024 项加 256 项尾巴，Q1.14；余弦取 fx_sin_tab[i + 256]。
*                    表由 tools/check/gen_sin_tab.py 生成进 sin_tab.c。
*           atan2    只在发射时调用（自机狙），更新循环里没有除法。
*
*         下面每一条取整规则都是契约：B 批弹型判据的期望文件由 Python 独立实现生成，
*         它必须照这里的规则写，所以规则一律写成两边都能逐位复现的整数运算。
*         有符号右移按 gcc 的实现是算术右移（向负无穷取整），两个目标都是 gcc。
*
*         注错开关（主机测试用，见 lib/test/test_fixmath.c）：
*           INJECT_FX_IDX_TRUNC    下标直接 a >> 6，不四舍五入
*           INJECT_FX_VEL_Q4       速度分量截成 Q12.4 的精度
*           INJECT_FX_ATAN_NOSCALE atan2 不先缩放（大输入溢出）
*           INJECT_FX_ATAN_UNIT    atan2 最后一步留在原文的一周 2^15 单位
******************************************************************************/
#ifndef GFX2D_CORE_FIXMATH_H
#define GFX2D_CORE_FIXMATH_H

#include <stdint.h>

/* --- Q.8 ------------------------------------------------------------------ */

#define FX_SHIFT        8
#define FX_ONE          256             /* 1 像素 */

/* Q.8 -> 整像素，向负无穷取整。提交命令时屏幕坐标就取这个。 */
static inline int32_t fx_px(int32_t v)
{
    return v >> FX_SHIFT;
}

/* --- 二进制角与正弦表 ------------------------------------------------------ */

#define FX_SIN_ONE      16384           /* Q1.14 的 1.0 */
#define FX_SIN_SHIFT    14
#define FX_SIN_N        1024u           /* 一周的项数 */
#define FX_SIN_TAIL     256u            /* 尾巴，余弦偏移四分之一周 */
#define FX_SIN_TAB_LEN  1280u
#define FX_BAM_STEP     64u             /* 表的一格是多少 BAM */

extern const int16_t fx_sin_tab[FX_SIN_TAB_LEN];

/* 角度取表下标：取最近的一格，恰在两格正中（a 除以 64 余 32）时取后一格。
   直接 a >> 6 会让所有角度都往同一个方向偏，最多 63 BAM；这样最多 32 BAM。 */
static inline uint32_t fx_bam_index(uint16_t a)
{
#ifdef INJECT_FX_IDX_TRUNC
    return ((uint32_t)a >> 6) & (FX_SIN_N - 1u);
#else
    return (((uint32_t)a + (FX_BAM_STEP / 2u)) >> 6) & (FX_SIN_N - 1u);
#endif
}

/* Q1.14 的 sin a 与 cos a。 */
static inline int32_t fx_sin(uint16_t a)
{
    return fx_sin_tab[fx_bam_index(a)];
}

static inline int32_t fx_cos(uint16_t a)
{
    return fx_sin_tab[fx_bam_index(a) + FX_SIN_TAIL];
}

/* 速度分量：speed 是 Q.8（像素/帧），方向按上面的约定。
     vx = (speed * cos a) >> 14,  vy = (speed * sin a) >> 14
   右移是算术右移。int32 乘积不溢出的条件是 |speed| < 2^17，即 512 像素/帧。 */
static inline void fx_vel(int32_t speed, uint16_t a, int32_t *vx, int32_t *vy)
{
    uint32_t i = fx_bam_index(a);

    *vx = (speed * (int32_t)fx_sin_tab[i + FX_SIN_TAIL]) >> FX_SIN_SHIFT;
    *vy = (speed * (int32_t)fx_sin_tab[i]) >> FX_SIN_SHIFT;
#ifdef INJECT_FX_VEL_Q4
    *vx &= ~(int32_t)15;        /* 注错：只留 Q12.4 能表达的精度 */
    *vy &= ~(int32_t)15;
#endif
}

/* --- atan2 --------------------------------------------------------------- */

/* 返回 BAM，满足方向约定：(cos a, sin a) 与 (x, y) 同向。参数顺序同 C 的 atan2(y, x)。
   任意 int32 输入都有定义（含 INT32_MIN）；fx_atan2(0, 0) 返回 0。
   对双精度参考的误差见主机测试的登记值，判据是小于正弦表的一格（64 BAM）。 */
uint16_t fx_atan2(int32_t y, int32_t x);

#endif /* GFX2D_CORE_FIXMATH_H */
