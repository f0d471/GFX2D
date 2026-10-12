/******************************************************************************
* @file sw6_scene_a.h: 场景 A —— 大量方块随机移动
*
* @brief  固定容量 5000 个方块，启动时按录像头（或演示种子）一次初始化完，
*         N 只决定本帧更新与绘制前多少个。位置与速度用 Q12.4。
*
*         方块 #0~#3 是四个**固定出屏样例**，位置与速度由常量给定，分别跨上、下、
*         左、右四条边且只沿所跨的那条边方向移动，所以每一帧都不会完全落在屏内；
*         #0 与 #3 的往返范围避开 HUD 矩形，裁剪在屏幕上看得到。
*         它们只证明本段每帧都提交了部分出屏的命令（日志 `off` 列）；裁剪本身的
*         正确性由命令表构造层的整帧用例与一致性向量保证。
*
*         绘制方式由种子选：COPY／ALPHA／ADD 各走 FILL，另一部分方块用素材包的纹理走
*         BLIT（图集 16×16 或棋盘 32×32，BLIT 不支持缩放，尺寸必须等于源格子）。
*         ALPHA 的 FILL 常量按预乘给，否则构造层会报 GFX_E_BAD_PREMULT。
******************************************************************************/
#ifndef SW6_SCENE_A_H
#define SW6_SCENE_A_H

#include <stdint.h>

#include "gfx/gfx.h"

#include "sw6_cfg.h"

/* 组帧时的构造侧统计（日志 `eq`／`off` 两列的来源）。只计场景命令：
   打底的全屏 FILL 与 HUD 都不进这里。 */
typedef struct {
    uint32_t eq[4];     /* 0 COPY、1 ALPHA、2 ADD、3 ALPHA_STRAIGHT */
    uint32_t off;       /* 本帧目的矩形不完全在屏内的场景命令数 */
    uint32_t bad;       /* 追加被构造层拒绝的次数 */
    uint32_t cmds;      /* 本帧追加的场景命令数（含被拒的调用） */
} sw6_build_stat;

void sw6_build_stat_clear(sw6_build_stat *fs);

/* 绘制方式。前三个走 FILL，后三个走 BLIT。 */
enum {
    SW6_DRAW_FILL_COPY = 0,
    SW6_DRAW_FILL_ALPHA,
    SW6_DRAW_FILL_ADD,
    SW6_DRAW_BLIT_ALPHA,        /* 图集 16×16，预乘源，方程 ALPHA */
    SW6_DRAW_BLIT_ADD,          /* 图集 16×16，方程 ADD */
    SW6_DRAW_BLIT_COPY,         /* 棋盘 32×32，方程 COPY */
    SW6_DRAW_N
};

typedef struct {
    int32_t  x, y;              /* Q12.4 的位置 */
    int32_t  vx, vy;            /* Q12.4 的每帧位移，非零 */
    int32_t  xmin, xmax;        /* Q12.4 的往返范围，闭区间 */
    int32_t  ymin, ymax;
    uint8_t  w, h;              /* 像素 */
    uint8_t  mode;              /* SW6_DRAW_* */
    uint8_t  cell;              /* BLIT 的源格子／子矩形选择 */
    uint32_t color;             /* FILL 用；ALPHA 时是预乘常量 */
} sw6_block;

typedef struct {
    sw6_block blk[SW6_POOL_MAX];
    uint32_t  n;                /* 本帧的活动数 */
} sw6_scene_a;

/* 整个池按 seed 一次初始化，活动数置 n0（夹在 [SW6_N_MIN, SW6_N_MAX]）。 */
void sw6_a_begin(sw6_scene_a *a, uint32_t seed, uint32_t n0);

/* 位置推进一步（只推前 n 个），碰边反弹。 */
void sw6_a_update(sw6_scene_a *a);

/* 上下键的按下沿：各增减 SW6_N_STEP，夹在上下限内。 */
void sw6_a_adjust(sw6_scene_a *a, int up, int down);

/* 把前 n 个方块追加进命令表（dst 是本帧的目的槽），统计写进 fs。 */
void sw6_a_build(const sw6_scene_a *a, gfx_list *l, int dst, sw6_build_stat *fs);

#endif /* SW6_SCENE_A_H */
