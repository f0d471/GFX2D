/******************************************************************************
* @file stg_kind.c: 弹种表
*
* @brief  C 批的 14 个弹种（sw 第 06 章 3.7 节的弹种表），素材见 3.12 节：
*           0~3   ROUND16  圆弹图集第 0~3 格（颜色号），16x16，判定半径 4 像素，ALPHA
*           4~7   SMALL8   小圆弹图集第 0~3 格，8x8，判定半径 2 像素，ALPHA
*           8~11  RICE20   米弹图集第 (颜色号 x 16 + 方向) 格，20x20，16 个方向，判定半径 4 像素，ALPHA
*           12    PSHOT    自机弹 6x24，判定半径 3 像素，ADD
*           13    SPARK    火花 8x8，帧号是剩余寿命，不碰，ADD
*         判定半径约为图像半径的一半（sw 第 06 章 3.1 节，东方的经验）。16x16 圆弹与 8x8 小圆弹的尺寸与 B 批相同。
******************************************************************************/

#include "stg_kind.h"

#include "core/fixmath.h"
#include "gfx/gfx.h"

#define ROUND(c)  { STG_SRC_BUL16, 2u, 0u,       GFX_EQ_ALPHA, 16u, 16u, (c),        0u, 4 * FX_ONE }
#define SMALL(c)  { STG_SRC_BUL8,  2u, 0u,       GFX_EQ_ALPHA,  8u,  8u, (c),        0u, 2 * FX_ONE }
#define RICE(c)   { STG_SRC_RICE,  4u, STG_DIRS, GFX_EQ_ALPHA, 20u, 20u, (c) * 16u,  0u, 4 * FX_ONE }

const stg_kind_info stg_kind_tab[STG_KIND_N_MAX] = {
    [STG_KIND_ROUND16 + 0u] = ROUND(0u), [STG_KIND_ROUND16 + 1u] = ROUND(1u),
    [STG_KIND_ROUND16 + 2u] = ROUND(2u), [STG_KIND_ROUND16 + 3u] = ROUND(3u),
    [STG_KIND_SMALL8 + 0u]  = SMALL(0u), [STG_KIND_SMALL8 + 1u]  = SMALL(1u),
    [STG_KIND_SMALL8 + 2u]  = SMALL(2u), [STG_KIND_SMALL8 + 3u]  = SMALL(3u),
    [STG_KIND_RICE20 + 0u]  = RICE(0u),  [STG_KIND_RICE20 + 1u]  = RICE(1u),
    [STG_KIND_RICE20 + 2u]  = RICE(2u),  [STG_KIND_RICE20 + 3u]  = RICE(3u),
    [STG_KIND_PSHOT]        = { STG_SRC_PSHOT, 0u, 0u, GFX_EQ_ADD, 6u, 24u, 0u, 0u, 3 * FX_ONE },
    [STG_KIND_SPARK]        = { STG_SRC_SPARK, 2u, 0u, GFX_EQ_ADD, 8u,  8u, 0u, 0u, 0 },
};

uint16_t stg_kind_sprite(uint32_t kind, uint16_t angle)
{
    const stg_kind_info *k = &stg_kind_tab[kind & (STG_KIND_N_MAX - 1u)];

    if (k->dirs == 0u)
        return k->first;
    return (uint16_t)(k->first + ((((uint32_t)angle + 2048u) >> 12) & (STG_DIRS - 1u)));
}

void stg_kind_radii(int32_t r[STG_KIND_N_MAX])
{
    uint32_t i;

    for (i = 0u; i < STG_KIND_N_MAX; i++)
        r[i] = stg_kind_tab[i].r_hit;
}
