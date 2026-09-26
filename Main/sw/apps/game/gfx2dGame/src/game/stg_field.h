/******************************************************************************
* @file stg_field.h: 弹幕场 —— 状态结构体与一帧的顺序
*
* @brief  SW7 2.3、2.4、2.6、2.8 节的 B 批部分。场的全部状态在一个结构体里
*         （四个池、发射器与替身宿主、随机数、自机探针、计数），没有模块级的可变全局；
*         换段、切模式时整体重新初始化。
*
*         一帧的顺序（2.3 节「一帧的顺序」，是契约）：
*           ① 每个池一遍更新（上一帧打的移除标记在这一步移除）；
*           ② 发射器按数组顺序：到点就在宿主当前的位置齐射，新弹追加在池尾、出生那一帧不移动；
*              处理完一个发射器，它的宿主再移动；
*           ③ 碰撞：自机判定点对敌弹（2.4 节）；
*           ④ 组帧与活动元素计数（2.6 节，stg_field_compose）。
*         第 k 帧的状态指第 k 帧第 ③ 步之后的池。stg_field_advance 做 ①②，
*         stg_field_collide 做 ③，stg_field_step 是两者相加；弹型判据只跑 ①②。
*
*         给 SW8 的受击钩子（2.8 节）：stg_field_set_hits(f, 0) 关掉受击，碰撞照做、擦弹照算。
*
*         组帧（2.6 节 B 批，占位图）：SET_CLIP 游戏区 400x480 -> 深色 FILL 铺满游戏区 ->
*         静止自机（COPY 加色键）-> 敌弹（按弹种编号的顺序，每个弹种分段调 gfx_blit_run，ALPHA）->
*         SET_CLIP 全屏 -> END。表面槽由调用方在 stg_compose_cfg 里给，本文件不认识槽号。
*         活动元素就是发出去的 Sprite 命令条数（至少一个像素落在裁剪框内），按池分开计，背景 FILL 不算。
*
*         替身宿主（B 批没有敌机）：位置每帧加 vx，移动后 x 落在 [x_lo, x_hi) 之外就把 vx 取反。
******************************************************************************/
#ifndef GFX2D_STG_FIELD_H
#define GFX2D_STG_FIELD_H

#include <stdint.h>

#include "core/fixmath.h"
#include "core/xorshift32.h"
#include "gfx/gfx.h"

#include "stg_emit.h"
#include "stg_kind.h"
#include "stg_pool.h"

#define STG_FIELD_EMIT_MAX  8u

/* 自机（B 批静止）。尺寸常量见 SW7 2.4 节。 */
#define STG_PLAYER_X        (200 * FX_ONE)
#define STG_PLAYER_Y        (400 * FX_ONE)
#define STG_PLAYER_R_HIT    (2 * FX_ONE)
#define STG_PLAYER_R_GRAZE  (16 * FX_ONE)

/* 组帧时的背景色（G2 的底色 #02040A）与分段缓冲的条数（16 字节一条，2 KB）。 */
#define STG_BG_ARGB         0x0002040Au
#define STG_DRAW_CHUNK      128u

/* 替身宿主的运动。vx 为 0 时不动。 */
typedef struct {
    int32_t vx;             /* Q.8/帧 */
    int32_t x_lo, x_hi;     /* 往返范围，Q.8，半开区间 */
} stg_host;

typedef struct {
    stg_pools       pools;
    stg_bounds      bounds;
    stg_emitter     em[STG_FIELD_EMIT_MAX];
    stg_host        host[STG_FIELD_EMIT_MAX];
    uint32_t        n_em;
    xs32            rng;                    /* 随机雨：一个场一个 */
    int32_t         px, py;                 /* 自机中心，Q.8 */
    int32_t         r_player, r_graze;      /* 自机判定半径与擦弹半径，Q.8 */
    int32_t         r_kind[STG_KIND_N_MAX]; /* 按弹种的判定半径，Q.8 */
    int             hit_enabled;            /* 受击开关（SW8 的压力钩子关掉它） */
    uint32_t        hits, grazes;           /* 累计 */
    stg_hit_stats   frame_hit;              /* 最近一帧的命中与擦弹 */
    stg_step_stats  last;                   /* 最近一遍敌弹池的移除计数 */
} stg_field;

/* 组帧的表面槽与自机图。 */
typedef struct {
    int      dst;                       /* 目的：本帧要画的那张帧缓冲 */
    int      src_slot[STG_SRC_N];       /* 弹种表里的源图编号 -> 表面槽 */
    int      player_slot;
    int32_t  player_w, player_h;
    uint32_t player_key;                /* 色键 */
} stg_compose_cfg;

/* 一帧发出的活动元素（2.6 节），按池分开。B 批只有敌弹与自机。 */
typedef struct {
    uint32_t ebullet;
    uint32_t pbullet;
    uint32_t enemy;
    uint32_t particle;
    uint32_t player;
    uint32_t total;
} stg_active;

/* 空场：池清空、默认出界边界、自机在 (200, 400)、受击打开、随机数按 seed。 */
void stg_field_init(stg_field *f, uint32_t seed);

/* 挂一个发射器。发射器满了或记录不合法（stg_emitter_init 拒绝）返回 -1。
   host 为 NULL 时宿主不动。 */
int  stg_field_add_emitter(stg_field *f, const stg_emit_rec *rec, int32_t x, int32_t y,
                           const stg_host *host);

/* ①②：池更新、发射与宿主移动。 */
void stg_field_advance(stg_field *f);

/* ③：自机判定点对敌弹（2.4 节），计数累加进 hits、grazes，本帧的写进 frame_hit。 */
void stg_field_collide(stg_field *f);

/* ①②③。 */
void stg_field_step(stg_field *f);

/* 受击钩子：0 关掉受击（不计命中、不打标记，擦弹照算），非 0 打开。 */
void stg_field_set_hits(stg_field *f, int on);

/* ④：把这一帧追加进 l（含两条 SET_CLIP 与 END），活动元素写进 act。
   追加被拒的条数照构造层的口径记在 l->rejected、l->overflow 里，这里不另计；
   被拒的不算活动元素。返回 0；l 已经 END 或没绑表面表时返回 -1、act 清零。 */
int  stg_field_compose(const stg_field *f, gfx_list *l, const stg_compose_cfg *cfg,
                       stg_active *act);

/* 游戏区（400x480）与全屏（640x480）两个裁剪框，闭区间。 */
void stg_rect_field(gfx_rect *r);
void stg_rect_screen(gfx_rect *r);

#endif /* GFX2D_STG_FIELD_H */
