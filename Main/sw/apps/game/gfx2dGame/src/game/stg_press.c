/******************************************************************************
* @file stg_press.c: 压力测试在弹幕场上的一帧
*
* @brief  口径见 stg_press.h 与 SW8 2.2、2.3 节。e 用逐场累加求，不做 64 位除法
*         （板上是 RV32，64 位除法是库调用；e 最多几百，循环几百次也比它省事）。
******************************************************************************/

#include "stg_press.h"

void stg_press_clock_b(stg_press_clock *c)
{
    uint32_t i;

    c->field_cyc = 1666667u;
    c->model.a   = 20000u;
    c->model.b   = 100u;
    for (i = 0u; i < GFX_CENSUS_EQ_N; i++)
        c->model.c[i] = 0u;
    c->model.c[GFX_EQ_COPY]  = 1u;
    c->model.c[GFX_EQ_ALPHA] = 2u;
}

void stg_press_begin(stg_field *f, uint32_t kind)
{
    stg_field_init(f, STG_PRESS_SEED);
    stg_field_set_hits(f, 0);
    stg_field_press_kind(f, kind);
    stg_field_set_pressure(f, 0u);
}

void stg_press_frame(stg_field *f, gfx_list *l, const stg_compose_cfg *cfg,
                     const stg_press_clock *ck, stg_press_out *o)
{
    gfx_census_state cs;
    gfx_census       c;
    uint64_t         left;
    uint32_t         e = 1u;

    o->bad = 0u;
    stg_field_step(f);
    if (stg_field_compose(f, l, cfg, &o->act) != 0 || l->rejected != 0u || l->overflow != 0u)
        o->bad++;

    gfx_census_reset(&cs);
    gfx_census_clear(&c);
    if (gfx_census_run(&cs, l->t, l->buf, l->used, &c) != 0)
        o->bad++;

    o->cost = gfx_cost_cycles(&ck->model, &c);
    o->frame_cyc = (o->cost > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)o->cost;

    /* e = max(1, ceil(cost / F))：代价严格大于 e 场就再加一场 */
    left = o->cost;
    while (left > ck->field_cyc) {
        left -= ck->field_cyc;
        e++;
    }
    o->e = e;
}
