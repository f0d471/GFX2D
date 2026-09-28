/******************************************************************************
* @file stg_enemy.h: 敌机的槽位表、机种表与移动事件表
*
* @brief  sw 第 06 章 3.5、3.11 节（C 批）的定法。敌机不进池：一架敌机要带血量、移动事件表的游标与
*         1~4 个发射器，20 字节的实体放不下；稳定压缩又会移动实体，发射器按下标找宿主的话，
*         压缩一次就指错了。所以敌机放在 64 个槽的固定槽位表里，槽位表是游戏状态的一部分。
*
*         移动事件表（Butano Fighter 的做法）：{dvx, dvy, 帧数} 一串，速度增量是 Q.8。
*         出生时进入第 0 步；进入一步时把增量加到速度上，在这一步的帧数里每帧把速度加进位置；
*         走完最后一步保持速度，直到出界。全是加法。
*
*         每帧（游戏一帧的第 5 步，stg_enemies_step）：按槽位顺序，每一架先让它的发射器按槽内顺序
*         到点齐射（在它当前的位置），再按移动事件表走一步，再判出界：中心落在
*         [-32, 432) x [-32, 512) 像素之外就移除，不得分。出生点都在这之内。
*         第 7 步（stg_enemies_settle）按第 6 步的命中数扣血，血量耗尽的移除、得分、原位生成 8 个火花。
******************************************************************************/
#ifndef STG_ENEMY_H
#define STG_ENEMY_H

#include <stdint.h>

#include "core/fixmath.h"

#include "stg_emit.h"
#include "stg_pool.h"

#define STG_ENEMY_SLOTS     64u
#define STG_ENEMY_EM_MAX    4u
#define STG_ENEMY_MARGIN_PX 32

/* 机种 */
#define STG_ETYPE_DART      0u
#define STG_ETYPE_CROSS     1u
#define STG_ETYPE_N         2u

/* 敌机被击毁时的火花：8 个，环形 2 像素/帧，寿命 16 帧 */
#define STG_KILL_SPARKS     8u
#define STG_KILL_SPARK_V    (2 * FX_ONE)
#define STG_KILL_SPARK_LIFE 16u

typedef struct {
    int32_t  dvx, dvy;      /* 速度增量，Q.8 */
    uint32_t frames;        /* 这一步走几帧，不小于 1 */
} stg_move;

typedef struct {
    const stg_move *steps;
    uint32_t        n;
} stg_move_tab;

/* 机种表（常量）：判定半径、血量、得分、画图的第一帧（ships.png 里的格号）。 */
typedef struct {
    int32_t  r_hit;         /* Q.8 */
    int32_t  hp;
    uint32_t score;
    uint32_t frame0;
} stg_etype;

extern const stg_etype stg_etype_tab[STG_ETYPE_N];

typedef struct {
    uint32_t        alive;
    uint32_t        type;
    int32_t         x, y;           /* 中心，Q.8 */
    int32_t         vx, vy;         /* Q.8/帧 */
    int32_t         hp;
    const stg_move *mv;
    uint32_t        mv_n, mv_i;     /* 移动事件表的长度与当前步 */
    uint32_t        mv_left;        /* 当前步还剩几帧；0 表示走完了最后一步 */
    uint32_t        n_em;
    stg_emitter     em[STG_ENEMY_EM_MAX];
    uint32_t        age;            /* 出生以来的帧数（动画帧） */
} stg_enemy;

typedef struct {
    stg_enemy e[STG_ENEMY_SLOTS];
    uint32_t  spawn_overflow;       /* 槽满被拒的出敌次数 */
} stg_enemies;

void     stg_enemies_init(stg_enemies *s);

/* 出敌：取第一个空槽，进入移动事件表的第 0 步，装入 n_em 条发射记录（发射器的位置就是出生点），
   第一次齐射推迟 delay 帧。槽满、n_em 超过 4、mv_n 为 0 或发射记录不合法返回 -1（槽满时计 spawn_overflow）；
   成功返回槽号。 */
int      stg_enemy_spawn(stg_enemies *s, uint32_t type, int32_t x, int32_t y,
                         const stg_move *mv, uint32_t mv_n,
                         const stg_emit_rec *const *recs, uint32_t n_em, uint32_t delay);

/* 第 5 步：发射、移动、出界。返回出界移除的架数。 */
uint32_t stg_enemies_step(stg_enemies *s, stg_pool *ebullet, const stg_emit_ctx *ctx);

/* 第 6 步的目标圆：tg[0..64) 与槽位一一对应，空槽 alive = 0。 */
void     stg_enemies_targets(const stg_enemies *s, stg_target *tg);

/* 第 7 步：按 hits[0..64) 扣血；血量耗尽的移除，得分加进 *score，击毁数按机种加进 kills[]，
   原位生成 8 个火花（粒子池满了计粒子池的溢出）。返回击毁的架数。 */
uint32_t stg_enemies_settle(stg_enemies *s, const uint32_t *hits, stg_pool *particle,
                            uint32_t *score, uint32_t kills[STG_ETYPE_N]);

/* 存活架数（组帧与活动元素计数按槽位表来）。 */
uint32_t stg_enemies_alive(const stg_enemies *s);

#endif /* STG_ENEMY_H */
