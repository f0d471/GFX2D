/******************************************************************************
* @file stg_field.c: 弹幕场
*
* @brief  口径见 stg_field.h 与 SW7 2.3 节。全部是整数运算，没有浮点、没有 malloc、
*         没有文件级的可变状态。
******************************************************************************/

#include "stg_field.h"

void stg_field_init(stg_field *f, uint32_t seed)
{
    stg_pools_init(&f->pools);
    stg_bounds_field(&f->bounds);
    f->n_em = 0u;
    xs32_seed(&f->rng, seed);
    f->px          = STG_PLAYER_X;
    f->py          = STG_PLAYER_Y;
    f->r_player    = STG_PLAYER_R_HIT;
    f->r_graze     = STG_PLAYER_R_GRAZE;
    stg_kind_radii(f->r_kind);
    f->hit_enabled = 1;
    f->hits        = 0u;
    f->grazes      = 0u;
    f->frame_hit.hits   = 0u;
    f->frame_hit.grazes = 0u;
    f->last.oob    = 0u;
    f->last.marked = 0u;
}

int stg_field_add_emitter(stg_field *f, const stg_emit_rec *rec, int32_t x, int32_t y,
                          const stg_host *host)
{
    stg_host *h;

    if (f->n_em >= STG_FIELD_EMIT_MAX)
        return -1;
    if (stg_emitter_init(&f->em[f->n_em], rec, x, y) != 0)
        return -1;

    h = &f->host[f->n_em];
    if (host != 0) {
        *h = *host;
    } else {
        h->vx   = 0;
        h->x_lo = 0;
        h->x_hi = 0;
    }
    f->n_em++;
    return 0;
}

/* 移动后 x 落在 [x_lo, x_hi) 之外就把速度取反（先移动再判）。 */
static void host_move(stg_emitter *em, stg_host *h)
{
    if (h->vx == 0)
        return;
    em->x += h->vx;
    if (em->x < h->x_lo || em->x >= h->x_hi)
        h->vx = -h->vx;
}

void stg_field_advance(stg_field *f)
{
    stg_emit_ctx ctx;
    uint32_t     i;

    /* ① 每个池一遍。B 批只有敌弹池有内容，另外三个是空的，一遍不做事。 */
    stg_pool_step(&f->pools.ebullet, &f->bounds, &f->last);
    stg_pool_step(&f->pools.pbullet, &f->bounds, 0);
    stg_pool_step(&f->pools.enemy, &f->bounds, 0);
    stg_pool_step(&f->pools.particle, &f->bounds, 0);

    /* ② 发射器按数组顺序；处理完一个，它的宿主再移动 */
    ctx.px  = f->px;
    ctx.py  = f->py;
    ctx.rng = &f->rng;
    for (i = 0u; i < f->n_em; i++) {
        (void)stg_emitter_tick(&f->em[i], &f->pools.ebullet, &ctx);
        host_move(&f->em[i], &f->host[i]);
    }
}

void stg_field_collide(stg_field *f)
{
    stg_probe pr;

    pr.px          = f->px;
    pr.py          = f->py;
    pr.r_hit       = f->r_player;
    pr.r_graze     = f->r_graze;
    pr.hit_enabled = f->hit_enabled;
    stg_pool_collide(&f->pools.ebullet, f->r_kind, &pr, &f->frame_hit);
    f->hits   += f->frame_hit.hits;
    f->grazes += f->frame_hit.grazes;
}

void stg_field_step(stg_field *f)
{
    stg_field_advance(f);
    stg_field_collide(f);
}

void stg_field_set_hits(stg_field *f, int on)
{
    f->hit_enabled = (on != 0);
}
