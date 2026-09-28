/******************************************************************************
* @file stg_press.h: 压力测试在弹幕场上的一帧（sw 第 07 章 3.6 节，B 批）
*
* @brief  N_max 搜索的状态机（lib/core/nmax）只吃「这一帧经过几场、活动元素数、帧时间」；
*         这里把弹幕场接上去：场推进一帧（发射、定寿命下落负载、碰撞），组帧，普查，
*         再按模拟时钟的代价模型算出代价与 e = max(1, ceil(代价 / F))。不执行命令表、不碰板子，
*         所以 app 测试与 gfx2dGame 的 -DSTG_FIELD=3 驱动用的是同一段代码。
*
*         B 批的模拟时钟模型（sw 第 07 章 3.6 节，只验证流程，不是性能数字）：
*           F = 1,666,667（100 MHz / 60），a = 20,000，b = 100，c[COPY] = 1，c[ALPHA] = 2，其余方程 0。
******************************************************************************/
#ifndef STG_PRESS_H
#define STG_PRESS_H

#include <stdint.h>

#include "gfx/census.h"
#include "gfx/gfx.h"

#include "stg_field.h"

#define STG_PRESS_SEED      0x5E7E0001u

typedef struct {
    gfx_cost_model model;
    uint32_t       field_cyc;       /* 一场的周期数 F */
} stg_press_clock;

typedef struct {
    stg_active act;                 /* 这一帧发出的活动元素 */
    uint64_t   cost;                /* 模型给出的帧代价（周期） */
    uint32_t   frame_cyc;           /* cost 截到 uint32，给帧时间直方图 */
    uint32_t   e;                   /* 这一帧经过的场数，不小于 1 */
    uint32_t   bad;                 /* 组帧被拒、表满或普查出错的次数（应为 0） */
} stg_press_out;

/* B 批的模型。 */
void stg_press_clock_b(stg_press_clock *c);

/* 一帧的普查按模型算代价，返回 e = max(1, ceil(代价 / F))；代价写进 *cost。
   SW8 模式 3 在终符上搜索时也用它（整帧的普查，含 HUD）。 */
uint32_t stg_press_cost(const stg_press_clock *ck, const gfx_census *c, uint64_t *cost);

/* 压力测试的场：空场（种子 STG_PRESS_SEED）、关掉受击、负载弹种 kind、旋钮 0。
   调用方随后把 nmax 的第一档设进 stg_field_set_pressure()。 */
void stg_press_begin(stg_field *f, uint32_t kind);

/* 一帧：stg_field_step、stg_field_compose 进 l（l 由调用方每帧 begin）、普查、代价与 e。 */
void stg_press_frame(stg_field *f, gfx_list *l, const stg_compose_cfg *cfg,
                     const stg_press_clock *ck, stg_press_out *o);

#endif /* STG_PRESS_H */
