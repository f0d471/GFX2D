/******************************************************************************
* @file census.c: 按方程的普查
*
* @brief  口径见 census.h 与 sw 第 07 章 3.2 节。裁剪照 exec_sw.c 的 dst_window() 与
*         apply_set_clip() **另写一份**，不为了共用去改 exec_sw.c：E7 的板上吞吐数据与
*         注错复验都绑定在执行器现在的代码上。两份写法不同、结论相同，
*         主机测试拿执行器的 pixel_total 与 cmd 对账，这本身就是一次独立实现的比对。
******************************************************************************/

#include "gfx/census.h"

static int32_t imax(int32_t a, int32_t b)
{
    return (a > b) ? a : b;
}

static int32_t imin(int32_t a, int32_t b)
{
    return (a < b) ? a : b;
}

void gfx_census_reset(gfx_census_state *cs)
{
    cs->clip.x0 = 0;
    cs->clip.y0 = 0;
    cs->clip.x1 = GFX_SCREEN_W - 1;
    cs->clip.y1 = GFX_SCREEN_H - 1;
}

void gfx_census_clear(gfx_census *c)
{
    uint32_t e;

    for (e = 0u; e < GFX_CENSUS_EQ_N; e++) {
        c->cmd_by_eq[e] = 0u;
        c->pix_by_eq[e] = 0u;
    }
    c->cmd_other = 0u;
}

/* 目的矩形（左上角加宽高）与三个框求交后的面积：游戏区、裁剪框、目的表面。
   区间都写成闭区间 [lo, hi]，任何一维 hi < lo 就是空。 */
static uint32_t clipped_area(const gfx_surf_raw *ds, const gfx_rect *clip,
                             int32_t dx, int32_t dy, uint32_t w, uint32_t h)
{
    int32_t x0, y0, x1, y1;

    if (w == 0u || h == 0u)
        return 0u;

    x0 = imax(imax(dx, 0), clip->x0);
    y0 = imax(imax(dy, 0), clip->y0);
    x1 = imin(imin(dx + (int32_t)w - 1, GFX_SCREEN_W - 1), clip->x1);
    y1 = imin(imin(dy + (int32_t)h - 1, GFX_SCREEN_H - 1), clip->y1);
#ifndef INJECT_CENSUS_NO_SURF
    x1 = imin(x1, (int32_t)gfx_surf_width(ds) - 1);
    y1 = imin(y1, (int32_t)gfx_surf_height(ds) - 1);
#else
    (void)ds;
#endif

    if (x1 < x0 || y1 < y0)
        return 0u;
    return (uint32_t)(x1 - x0 + 1) * (uint32_t)(y1 - y0 + 1);
}

int gfx_census_run(gfx_census_state *cs, const gfx_surface_table *t,
                   const gfx_cmd *list, uint32_t list_count, gfx_census *c)
{
    uint32_t idx;

    for (idx = 0u; idx < list_count; idx++) {
        const gfx_cmd *r  = &list[idx];
        uint32_t       op = gfx_op(r);
        uint32_t       eq;

        if (op == GFX_OP_END)
            return 0;
        if (op == GFX_OP_NOP) {
            c->cmd_other++;
            continue;
        }
        if (op == GFX_OP_SET_CLIP) {
#ifndef INJECT_CENSUS_NO_SETCLIP
            int32_t x0 = gfx_s16(r->w[1]);
            int32_t y0 = gfx_s16(r->w[1] >> 16);

            cs->clip.x0 = imax(x0, 0);
            cs->clip.y0 = imax(y0, 0);
            cs->clip.x1 = imin(x0 + (int32_t)gfx_u16(r->w[3]) - 1, GFX_SCREEN_W - 1);
            cs->clip.y1 = imin(y0 + (int32_t)gfx_u16(r->w[3] >> 16) - 1, GFX_SCREEN_H - 1);
#endif
            c->cmd_other++;
            continue;
        }
        if (op != GFX_OP_FILL && op != GFX_OP_BLIT)
            return -1;                  /* 不认识的操作码：命令表不合法，停下 */

        eq = gfx_eq(r);
        if (eq >= GFX_CENSUS_EQ_N)
            return -1;                  /* 方程号越界：不拿它去索引计数数组 */

        c->cmd_by_eq[eq]++;
        c->pix_by_eq[eq] += clipped_area(&t->s[gfx_dst(r)], &cs->clip,
                                         gfx_s16(r->w[2]), gfx_s16(r->w[2] >> 16),
                                         gfx_u16(r->w[3]), gfx_u16(r->w[3] >> 16));
    }
    return 0;                           /* COUNT 耗尽而没有 END 也是正常结束 */
}

uint32_t gfx_census_cmd_total(const gfx_census *c)
{
    uint32_t e, n = c->cmd_other;

    for (e = 0u; e < GFX_CENSUS_EQ_N; e++)
        n += c->cmd_by_eq[e];
    return n;
}

uint32_t gfx_census_pix_total(const gfx_census *c)
{
    uint32_t e, n = 0u;

    for (e = 0u; e < GFX_CENSUS_EQ_N; e++)
        n += c->pix_by_eq[e];
    return n;
}

uint64_t gfx_cost_cycles(const gfx_cost_model *m, const gfx_census *c)
{
    uint64_t cost = m->a;
    uint32_t e;

#ifndef INJECT_COST_NO_B
    cost += (uint64_t)m->b * gfx_census_cmd_total(c);
#endif
    for (e = 0u; e < GFX_CENSUS_EQ_N; e++)
        cost += (uint64_t)m->c[e] * c->pix_by_eq[e];
    return cost;
}

int gfx_cost_drops(uint64_t cost, uint32_t field_cyc)
{
    return cost > (uint64_t)field_cyc;
}
