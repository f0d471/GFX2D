/******************************************************************************
* @file stg_kind.c: 弹种表
*
* @brief  B 批的三个弹种（SW7 2.3 节「弹种表」），用 SW5 的占位图：
*           ROUND16  TILE_ATLAS 第 0 帧，16x16，判定半径 4 像素
*           SMALL8   SOLID8，8x8，判定半径 2 像素
*           RICE16   TILE_ATLAS 第 16~31 帧（第 2、3 行），16 个方向，判定半径 3 像素
*         判定半径约为图像半径的一半（SW7 2.4 节，东方的经验）。
******************************************************************************/

#include "stg_kind.h"

#include "core/fixmath.h"

const stg_kind_info stg_kind_tab[STG_KIND_N_MAX] = {
    [STG_KIND_ROUND16] = { STG_SRC_ATLAS, 3u, 0u,        0u, 16u, 16u,  0u, 0u, 4 * FX_ONE },
    [STG_KIND_SMALL8]  = { STG_SRC_SMALL, 0u, 0u,        0u,  8u,  8u,  0u, 0u, 2 * FX_ONE },
    [STG_KIND_RICE16]  = { STG_SRC_ATLAS, 3u, STG_DIRS,  0u, 16u, 16u, 16u, 0u, 3 * FX_ONE },
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
