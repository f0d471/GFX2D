/******************************************************************************
* @file stg_emit.c: 发射记录与发射器
*
* @brief  口径见 stg_emit.h 与 sw 第 06 章 3.7 节。除法与取模只出现在发射时：
*         fx_atan2 里一次除法（AIM），随机雨两次无符号取模；更新循环在 stg_pool.c，没有这些。
******************************************************************************/

#include "stg_emit.h"

#include "core/fixmath.h"

#include "stg_kind.h"

int stg_emitter_init(stg_emitter *em, const stg_emit_rec *rec, int32_t x, int32_t y)
{
    if (rec == 0 || rec->interval == 0u || rec->dir_mode > STG_DIR_SEQ)
        return -1;
    if (rec->rand_x < 0 || rec->rand_x > STG_RAND_MAX
        || rec->rand_speed < 0 || rec->rand_speed > STG_RAND_MAX)
        return -1;

    em->rec     = rec;
    em->x       = x;
    em->y       = y;
    em->wait    = 0u;
    em->volleys = 0u;
    em->seq     = rec->angle0;
    em->rsvd    = 0u;
    return 0;
}

int stg_emitter_done(const stg_emitter *em)
{
    return em->rec->repeat != 0u && em->volleys >= em->rec->repeat;
}

/* 随机雨的一次取数：[0, span] 里的一个数。span 不超过 2^25，结果放得进 int32。 */
static int32_t rand_upto(xs32 *g, uint32_t span)
{
    return (int32_t)(xs32_next(g) % (span + 1u));
}

/* 一次齐射：基准角、逐发的角度与速度、随机雨的取数；flags 是弹种号（已取低 4 位）。已齐射次数加一。 */
static uint32_t volley(stg_emitter *em, stg_pool *pool, const stg_emit_ctx *ctx, uint16_t flags)
{
    const stg_emit_rec *r = em->rec;
    uint16_t            a;
    int32_t             spd;
    uint32_t            j;

    /* 基准角 */
    if (r->dir_mode == STG_DIR_AIM) {
#ifdef INJECT_EMIT_AIM_SWAP
        /* 注错：两个参数对调 */
        a = (uint16_t)(fx_atan2(ctx->px - em->x, ctx->py - em->y) + r->angle0);
#else
        a = (uint16_t)(fx_atan2(ctx->py - em->y, ctx->px - em->x) + r->angle0);
#endif
    } else if (r->dir_mode == STG_DIR_SEQ) {
        a = em->seq;
#ifndef INJECT_EMIT_SEQ_NOACC
        em->seq = (uint16_t)(em->seq + r->spin);
#endif
    } else {
        a = r->angle0;
    }

    spd = r->speed;
    for (j = 0u; j < r->count; j++) {
        int32_t bx = em->x;
        int32_t s = spd;
        int32_t vx, vy;

#ifdef INJECT_EMIT_RAIN_ORDER
        /* 注错：先取速度、再取 x */
        if (r->rand_speed != 0)
            s += rand_upto(ctx->rng, (uint32_t)r->rand_speed);
        if (r->rand_x != 0)
            bx += rand_upto(ctx->rng, 2u * (uint32_t)r->rand_x) - r->rand_x;
#else
        if (r->rand_x != 0)
            bx += rand_upto(ctx->rng, 2u * (uint32_t)r->rand_x) - r->rand_x;
        if (r->rand_speed != 0)
            s += rand_upto(ctx->rng, (uint32_t)r->rand_speed);
#endif

        fx_vel(s, a, &vx, &vy);
        (void)stg_pool_add(pool, bx, em->y, vx, vy, stg_kind_sprite(flags, a), flags);

        a = (uint16_t)(a + r->angle_step);
        spd += r->speed_step;
    }

    em->volleys++;
    return r->count;
}

uint32_t stg_emitter_tick(stg_emitter *em, stg_pool *pool, const stg_emit_ctx *ctx)
{
    const stg_emit_rec *r = em->rec;
    uint32_t            n;

    if (stg_emitter_done(em))
        return 0u;
    if (em->wait != 0u) {
        em->wait--;
        return 0u;
    }
    n = volley(em, pool, ctx, (uint16_t)(r->kind & (STG_KIND_N_MAX - 1u)));
    em->wait = (uint32_t)r->interval - 1u;
    return n;
}

uint32_t stg_emitter_fire(stg_emitter *em, stg_pool *pool, const stg_emit_ctx *ctx, uint32_t kind)
{
    return volley(em, pool, ctx, (uint16_t)(kind & (STG_KIND_N_MAX - 1u)));
}
