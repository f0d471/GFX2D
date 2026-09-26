/******************************************************************************
* @file stg_kind.h: 弹种表
*
* @brief  SW7 2.3 节。一颗弹的弹种是 flags[3:0]，查这张 16 项的常量表得到图像
*         （源图、尺寸、起始帧、方向帧）与判定半径。下标由 4 位掩码得来，不用判越界；
*         未用的项全是 0：尺寸为 0 不画，半径为 0 不碰。
*
*         表里记的是源图的抽象编号（STG_SRC_*），真正的表面槽由入口传进来，
*         src/game/ 下的代码不认识槽号。B 批用 SW5 的占位图；C 批换正式素材时只改 stg_kind.c。
*
*         方向帧（SW7 2.3 节「方向帧」）：有方向帧的弹种按发射角取帧，
*           sprite = first + ((((uint32_t)angle + 2048) >> 12) & 15)
*         & 15 让接近一周的角度回到第 0 帧；没有方向帧的 sprite = first。
******************************************************************************/
#ifndef STG_KIND_H
#define STG_KIND_H

#include <stdint.h>

#define STG_KIND_N_MAX      16u         /* flags[3:0] 的取值个数 */

/* B 批用到的三个弹种。编号就是 flags[3:0]。 */
#define STG_KIND_ROUND16    0u          /* 16x16 圆弹，TILE_ATLAS 第 0 帧 */
#define STG_KIND_SMALL8     1u          /* 8x8 小圆弹，SOLID8 */
#define STG_KIND_RICE16     2u          /* 16x16 米弹，TILE_ATLAS 第 16~31 帧，16 个方向 */
#define STG_KIND_USED       3u          /* 用到的弹种：0 .. STG_KIND_USED-1 */

/* 源图的抽象编号。入口把它们映射成表面槽。 */
#define STG_SRC_ATLAS       0u          /* TILE_ATLAS：128x96，8 列 16x16 */
#define STG_SRC_SMALL       1u          /* SOLID8：8x8 单图 */
#define STG_SRC_N           2u

#define STG_DIRS            16u         /* 有方向帧的弹种的方向数，每格 22.5 度 */

typedef struct {
    uint8_t  src;           /* STG_SRC_* */
    uint8_t  cols_log2;     /* 源图每行 2^cols_log2 格 */
    uint8_t  dirs;          /* 0：无方向帧；STG_DIRS：16 个方向 */
    uint8_t  rsvd;
    uint16_t w, h;          /* 图像尺寸（像素）；0 为未用 */
    uint16_t first;         /* 起始帧号 */
    uint16_t rsvd2;
    int32_t  r_hit;         /* 判定半径，Q.8 */
} stg_kind_info;

_Static_assert(sizeof(stg_kind_info) == 16u, "stg_kind_info 必须是 16 字节");

extern const stg_kind_info stg_kind_tab[STG_KIND_N_MAX];

/* 按发射角取帧号。kind 只取低 4 位。 */
uint16_t stg_kind_sprite(uint32_t kind, uint16_t angle);

/* 把 16 个弹种的判定半径抄进 r[16]（给 stg_pool_collide 用）。 */
void     stg_kind_radii(int32_t r[STG_KIND_N_MAX]);

#endif /* STG_KIND_H */
