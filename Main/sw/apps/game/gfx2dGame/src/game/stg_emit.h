/******************************************************************************
* @file stg_emit.h: 发射记录与发射器
*
* @brief  sw 第 06 章 3.7 节。一种弹型不是一个函数，而是一条常量发射记录；五种弹型是同一个
*         发射函数的五组参数（BulletML 的 fire 与 repeat 的子集）。发射器按宿主的时间调用，
*         不在弹幕上调用；发射时算好 vx、vy，此后池里只做加法。
*
*         契约（2.3 节 B 批定法，Python 独立实现 tools/check/gen_stg_patterns.py 照同样的规则写）：
*           计时    从 t = 0 起，t 是 interval 的倍数时齐射；实现为倒数计数（到 0 齐射、
*                   再装入 interval - 1），不取模。repeat 是总齐射次数，0 为一直发。
*           基准角  ABS 取 angle0；AIM 取 fx_atan2(py - ey, px - ex) + angle0，每次齐射重算；
*                   SEQ 第 m 次齐射取 angle0 + m * spin（累计角装入时等于 angle0，齐射后加 spin）。
*           逐发    第 j 发角度 base + j * angle_step（uint16 回绕），速度 speed + j * speed_step，
*                   vx、vy 由 fx_vel 算出；弹的位置就是发射器的位置，出生那一帧不移动。
*           随机雨  rand_x、rand_speed 非零时从场的 xorshift32 取数，每一发先 x 后速度：
*                     x     = ex + (int32)(next % (2 * rand_x + 1)) - rand_x
*                     speed += (int32)(next % (rand_speed + 1))
*                   为 0 的那一项不取数。取模只在这里做，更新循环里没有。
*           帧号    stg_kind_sprite(kind, 角度)；flags 只带弹种号。
*
*         注错开关（app 测试与 check_sw7_game.sh 用）：
*           INJECT_EMIT_SEQ_NOACC   SEQ 不累加 spin
*           INJECT_EMIT_AIM_SWAP    AIM 的 fx_atan2 两个参数对调
*           INJECT_EMIT_RAIN_ORDER  随机雨先取速度、再取 x
******************************************************************************/
#ifndef STG_EMIT_H
#define STG_EMIT_H

#include <stdint.h>

#include "core/xorshift32.h"

#include "stg_pool.h"

#define STG_DIR_ABS         0u          /* 绝对角 */
#define STG_DIR_AIM         1u          /* 以自机方向为 0 */
#define STG_DIR_SEQ         2u          /* 在上一次齐射的角度上累加 spin */

/* 随机范围的上限：2 * rand_x + 1 要放得进 uint32，且 x 偏移不越出 int32。 */
#define STG_RAND_MAX        (1 << 24)

typedef struct {
    uint16_t count;         /* 一次齐射几发 */
    uint8_t  dir_mode;      /* STG_DIR_* */
    uint8_t  kind;          /* 弹种，0..15 */
    uint16_t angle0;        /* BAM */
    uint16_t angle_step;    /* BAM */
    uint16_t spin;          /* SEQ 每次齐射转过的角度，BAM */
    uint16_t interval;      /* 齐射间隔（帧），不小于 1 */
    uint16_t repeat;        /* 总齐射次数，0 为一直发 */
    uint16_t rsvd;
    int32_t  speed;         /* 第一发的速度，Q.8 */
    int32_t  speed_step;    /* 相邻两发的速度差，Q.8 */
    int32_t  rand_x;        /* 出生点 x 的随机半宽，Q.8；0 不取数 */
    int32_t  rand_speed;    /* 速度的随机增量上限，Q.8；0 不取数 */
} stg_emit_rec;

_Static_assert(sizeof(stg_emit_rec) == 32u, "stg_emit_rec 必须是 32 字节");

typedef struct {
    const stg_emit_rec *rec;
    int32_t  x, y;          /* 位置，Q.8，由宿主写 */
    uint32_t wait;          /* 倒数计数：为 0 的那一帧齐射 */
    uint32_t volleys;       /* 已齐射次数 */
    uint16_t seq;           /* SEQ 的累计角 */
    uint16_t rsvd;
} stg_emitter;

/* 发射时要的外部量：自机中心（AIM）与场的随机数（随机雨）。 */
typedef struct {
    int32_t  px, py;        /* 自机中心，Q.8 */
    xs32    *rng;
} stg_emit_ctx;

/* 装入一条发射记录。interval 为 0、方向模式或随机范围不合法时返回 -1，em 不动；
   成功返回 0，计时从 t = 0 开始（这一帧就齐射）。 */
int      stg_emitter_init(stg_emitter *em, const stg_emit_rec *rec, int32_t x, int32_t y);

/* 推进一帧：到点就齐射，弹追加到 pool 的末尾（池满的那几发被池拒绝、计入池的溢出）。
   返回这一帧试图发射的弹数（没到点或已发完为 0）。 */
uint32_t stg_emitter_tick(stg_emitter *em, stg_pool *pool, const stg_emit_ctx *ctx);

/* 不看计时与 repeat，马上齐射一次（终符的门控用，sw 第 06 章 3.11 节）：弹种换成 kind（只取低 4 位），
   其余照 stg_emitter_tick 的规则；已齐射次数加一，倒数计数不动。返回这次的发数。 */
uint32_t stg_emitter_fire(stg_emitter *em, stg_pool *pool, const stg_emit_ctx *ctx, uint32_t kind);

/* repeat 次已经发完返回 1。repeat 为 0 的永远返回 0。 */
int      stg_emitter_done(const stg_emitter *em);

#endif /* STG_EMIT_H */
