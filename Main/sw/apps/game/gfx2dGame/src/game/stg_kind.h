/******************************************************************************
* @file stg_kind.h: 弹种表
*
* @brief  sw 第 06 章 3.7 节。一颗弹的弹种是 flags[3:0]，查这张 16 项的常量表得到图像
*         （源图、尺寸、起始帧、方向帧）、方程与判定半径。下标由 4 位掩码得来，不用判越界；
*         未用的项全是 0：尺寸为 0 不画，半径为 0 不碰。
*
*         表里记的是源图的抽象编号（STG_SRC_*），真正的表面槽由入口传进来，
*         src/game/ 下的代码不认识槽号。C 批换成正式素材（2.7 节）：三种敌弹各四色、自机弹、火花，共 14 项。
*
*         方向帧（sw 第 06 章 3.7 节）：有方向帧的弹种按发射角取帧，
*           sprite = first + ((((uint32_t)angle + 2048) >> 12) & 15)
*         & 15 让接近一周的角度回到第 0 帧；没有方向帧的 sprite = first。
*         源左上角由帧号算：sx = (sprite & (2^cols_log2 - 1)) * w、sy = (sprite >> cols_log2) * h。
*         火花的帧号是剩余寿命（1~16）：列 = 寿命 & 3（闪烁的 4 帧），行 = 寿命 >> 2（5 档亮度）。
******************************************************************************/
#ifndef STG_KIND_H
#define STG_KIND_H

#include <stdint.h>

#define STG_KIND_N_MAX      16u         /* flags[3:0] 的取值个数 */

/* 颜色号（2.3 节 C 批）：0~3 依次是青、品红、琥珀、绿。 */
#define STG_COLOR_CYAN      0u
#define STG_COLOR_MAGENTA   1u
#define STG_COLOR_AMBER     2u
#define STG_COLOR_GREEN     3u

/* 弹种号就是 flags[3:0]。三种敌弹各占四个号，按颜色号排。 */
#define STG_KIND_ROUND16    0u          /* 0~3：16x16 圆弹，圆弹图集，判定半径 4 像素 */
#define STG_KIND_SMALL8     4u          /* 4~7：8x8 小圆弹，小圆弹图集，判定半径 2 像素 */
#define STG_KIND_RICE20     8u          /* 8~11：20x20 米弹，16 个方向，判定半径 4 像素 */
#define STG_KIND_PSHOT      12u         /* 自机弹 6x24，ADD，判定半径 3 像素 */
#define STG_KIND_SPARK      13u         /* 火花 8x8，ADD，不碰 */
#define STG_KIND_USED       14u         /* 用到的弹种：0 .. STG_KIND_USED-1 */

/* 源图的抽象编号。入口把它们映射成表面槽（2.7 节的表面槽表）。 */
#define STG_SRC_BUL16       0u          /* bul16.png：64x16，4 列 16x16 */
#define STG_SRC_BUL8        1u          /* bul8.png：32x8，4 列 8x8 */
#define STG_SRC_RICE        2u          /* rice20.png：320x80，16 列 20x20 */
#define STG_SRC_PSHOT       3u          /* pshot.png：6x24 单图 */
#define STG_SRC_SPARK       4u          /* spark.png：32x40，4 列 8x8 */
#define STG_SRC_N           5u

#define STG_DIRS            16u         /* 有方向帧的弹种的方向数，每格 22.5 度 */

typedef struct {
    uint8_t  src;           /* STG_SRC_* */
    uint8_t  cols_log2;     /* 源图每行 2^cols_log2 格 */
    uint8_t  dirs;          /* 0：无方向帧；STG_DIRS：16 个方向 */
    uint8_t  eq;            /* 画图的方程：GFX_EQ_ALPHA（敌弹）或 GFX_EQ_ADD（自机弹、火花） */
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
