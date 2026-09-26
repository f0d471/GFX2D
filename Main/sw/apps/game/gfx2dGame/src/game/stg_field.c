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

void stg_rect_field(gfx_rect *r)
{
    r->x0 = 0;
    r->y0 = 0;
    r->x1 = STG_FIELD_W - 1;
    r->y1 = STG_FIELD_H - 1;
}

void stg_rect_screen(gfx_rect *r)
{
    r->x0 = 0;
    r->y0 = 0;
    r->x1 = 640 - 1;
    r->y1 = 480 - 1;
}

/* 一个池按弹种编号的顺序发出：每个用到的弹种一个模板，分段扫、分段交给 gfx_blit_run()。
   返回成功追加的条数。 */
static uint32_t draw_pool(const stg_pool *p, gfx_list *l, const stg_compose_cfg *cfg,
                          const gfx_rect *clip)
{
    gfx_blit_pos pos[STG_DRAW_CHUNK];
    uint32_t     kind, done = 0u;

    for (kind = 0u; kind < STG_KIND_N_MAX; kind++) {
        const stg_kind_info *ki = &stg_kind_tab[kind];
        stg_draw_geom        g;
        gfx_blit_tmpl        t;
        uint32_t             cursor = 0u;

        if (ki->w == 0u)
            continue;
        g.w         = (int32_t)ki->w;
        g.h         = (int32_t)ki->h;
        g.cols_log2 = ki->cols_log2;
        t.src    = cfg->src_slot[ki->src < STG_SRC_N ? ki->src : 0u];
        t.dst    = cfg->dst;
        t.w      = (int)ki->w;
        t.h      = (int)ki->h;
        t.eq     = GFX_EQ_ALPHA;
        t.c      = 0u;
        t.key_en = 0;
        t.key    = 0u;

        while (cursor < stg_pool_count(p)) {
            uint32_t k = stg_pool_draw(p, kind, &g, clip, &cursor, pos, STG_DRAW_CHUNK);

            if (k != 0u)
                done += gfx_blit_run(l, &t, pos, k);
        }
    }
    return done;
}

int stg_field_compose(const stg_field *f, gfx_list *l, const stg_compose_cfg *cfg,
                      stg_active *act)
{
    gfx_rect field, screen;
    int32_t  pdx, pdy;

    act->ebullet  = 0u;
    act->pbullet  = 0u;
    act->enemy    = 0u;
    act->particle = 0u;
    act->player   = 0u;
    act->total    = 0u;
    if (l == 0 || l->t == 0 || l->ended)
        return -1;

    stg_rect_field(&field);
    stg_rect_screen(&screen);

    (void)gfx_set_clip(l, field);
    (void)gfx_fill(l, cfg->dst, field, STG_BG_ARGB, GFX_EQ_COPY, 0u);

    /* 自机：目的矩形与裁剪框相交才发出 */
    pdx = stg_draw_origin(f->px, cfg->player_w);
    pdy = stg_draw_origin(f->py, cfg->player_h);
    if (pdx <= field.x1 && pdx + cfg->player_w - 1 >= field.x0
        && pdy <= field.y1 && pdy + cfg->player_h - 1 >= field.y0
        && gfx_blit(l, cfg->player_slot, 0, 0, cfg->dst, (int)pdx, (int)pdy,
                    (int)cfg->player_w, (int)cfg->player_h, GFX_EQ_COPY, 0u, 1,
                    cfg->player_key) == 0)
        act->player = 1u;

    act->ebullet = draw_pool(&f->pools.ebullet, l, cfg, &field);

    (void)gfx_set_clip(l, screen);
    (void)gfx_list_end(l);

    act->total = act->ebullet + act->pbullet + act->enemy + act->particle + act->player;
    return 0;
}
