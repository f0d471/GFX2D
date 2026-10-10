/******************************************************************************
* @file stg_draw.c: 游戏的组帧与 HUD
*
* @brief  口径见 stg_draw.h 与 sw 第 06 章 3.13 节。全部是整数运算，没有文件级的可变状态。
*         除法与取模只出现在 HUD 的数字格式化与精灵帧号上（每帧几十次），不在弹的逐颗循环里；
*         弹走 stg_draw_pool（stg_field.c）；逐颗的扫描在 stg_pool_draw 里，G11 的反汇编判据查的是它。
******************************************************************************/

#include "stg_draw.h"

#include "stg_enemy.h"
#include "stg_kind.h"
#include "stg_stage.h"
#ifdef GFX_FRONTEND_PROFILE
extern void frontend_stage(uint32_t stage,const gfx_list *list);
#define FRONTEND_STAGE(n,l) frontend_stage(n,l)
#else
#define FRONTEND_STAGE(n,l) ((void)0)
#endif

/* 组帧的上下文：命令表、配置与计数。 */
typedef struct {
    gfx_list            *l;
    const stg_draw_cfg  *cfg;
    stg_frame_stats     *st;
    int                  dst;
    gfx_rect             clip;          /* 当前裁剪框（闭区间） */
    int                  static_base;
} dctx;

static void count(dctx *d, int rc, int eq)
{
    if (rc == 0)
        d->st->by_eq[eq]++;
    else
        d->st->rejected++;
}

static void fill(dctx *d, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t argb, int eq, uint8_t c)
{
    gfx_rect r;

    r.x0 = x0;
    r.y0 = y0;
    r.x1 = x1;
    r.y1 = y1;
    count(d, gfx_fill(d->l, d->dst, r, argb, eq, c), eq);
}

/* 目的矩形与当前裁剪框相交才发出；发出成功返回 1。 */
static int sprite(dctx *d, int src, int sx, int sy, int32_t dx, int32_t dy, int32_t w, int32_t h,
                  int eq, int key_en)
{
    int rc;

    if (dx > d->clip.x1 || dx + w - 1 < d->clip.x0 || dy > d->clip.y1 || dy + h - 1 < d->clip.y0)
        return 0;
    rc = gfx_blit(d->l, src, sx, sy, d->dst, (int)dx, (int)dy, (int)w, (int)h, eq, 0u, key_en, 0u);
    count(d, rc, eq);
    return rc == 0;
}

static void text(dctx *d, const stg_font *f, int x, int y, const char *s)
{
    uint32_t drawn = 0u;

    d->st->rejected += stg_text_draw(d->l, f, d->dst, x, y, s, &drawn);
    d->st->by_eq[GFX_EQ_COPY] += drawn;
}

/* --- 游戏区 ----------------------------------------------------------------- */

static void draw_background(dctx *d, const stg_bg *bg)
{
    uint32_t k;

    if(!d->static_base)
        fill(d, 0, 0, STG_FIELD_W - 1, STG_FIELD_H - 1, STG_BG_ARGB, GFX_EQ_COPY, 0u);
    for (k = 0u; k < STG_BG_HLINES; k++) {
        const int32_t y = stg_bg_hline_y(bg, k);

        fill(d, 0, y, STG_FIELD_W - 1, y, stg_bg_hline_bright(bg, k) ? STG_BG_BRIGHT : STG_BG_DIM,
             GFX_EQ_COPY, 0u);
    }
    for (k = 0u; k < STG_BG_VLINES; k++) {
        const int32_t x = stg_bg_vline_x(k);

        fill(d, x, 0, x, STG_FIELD_H - 1, stg_bg_vline_bright(k) ? STG_BG_BRIGHT : STG_BG_DIM,
             GFX_EQ_COPY, 0u);
    }
    for (k = 0u; k < STG_BG_STARS; k++) {
        const stg_star *s = &bg->star[k];

        (void)sprite(d, d->cfg->stars_slot, (int)(s->kind * (uint32_t)STG_STAR_PX), 0,
                     fx_px(s->x), fx_px(s->y), STG_STAR_PX, STG_STAR_PX, GFX_EQ_COPY, 1);
    }
}

/* ships.png 的第 f 格（4 列 32x32）画在中心 (x, y) 上。 */
static int draw_ship(dctx *d, uint32_t f, int32_t x, int32_t y)
{
    return sprite(d, d->cfg->ships_slot,
                  (int)((f % STG_SHIP_COLS) * (uint32_t)STG_SHIP_PX), (int)((f / STG_SHIP_COLS) * (uint32_t)STG_SHIP_PX),
                  stg_draw_origin(x, STG_SHIP_PX), stg_draw_origin(y, STG_SHIP_PX), STG_SHIP_PX, STG_SHIP_PX,
                  GFX_EQ_COPY, 1);
}

static void draw_enemies(dctx *d, const stg_game *g)
{
    uint32_t i;

    for (i = 0u; i < STG_ENEMY_SLOTS; i++) {
        const stg_enemy *e = &g->en.e[i];

        if (e->alive == 0u)
            continue;
        if (draw_ship(d, stg_etype_tab[e->type].frame0 + ((e->age >> 3) & 3u), e->x, e->y))
            d->st->act.enemy++;
    }
    if (g->boss.on != STG_BOSS_OFF) {
        const uint32_t f = (g->frame >> 3) % STG_BOSS_FRAMES;

        if (sprite(d, d->cfg->boss_slot,
                   (int)((f % STG_BOSS_COLS) * (uint32_t)STG_BOSS_PX), (int)((f / STG_BOSS_COLS) * (uint32_t)STG_BOSS_PX),
                   stg_draw_origin(g->boss.x, STG_BOSS_PX), stg_draw_origin(g->boss.y, STG_BOSS_PX),
                   STG_BOSS_PX, STG_BOSS_PX, GFX_EQ_ALPHA, 0)) {
            d->st->act.enemy++;
            d->st->boss++;
        }
    }
}

static void draw_player(dctx *d, const stg_game *g)
{
    if (!stg_game_player_visible(g))
        return;
    if (draw_ship(d, 0u, g->f.px, g->f.py))
        d->st->act.player++;
    if (g->slow
        && sprite(d, d->cfg->icons_slot, (int)(STG_ICON_HITBOX * (uint32_t)STG_ICON_PX), 0,
                  stg_draw_origin(g->f.px, STG_ICON_PX), stg_draw_origin(g->f.py, STG_ICON_PX),
                  STG_ICON_PX, STG_ICON_PX, GFX_EQ_COPY, 1))
        d->st->act.player++;
}

static void draw_banner(dctx *d, const stg_game *g)
{
    if (g->phase == STG_PH_TITLE) {
        if (sprite(d, d->cfg->title_slot, 0, 0, (STG_FIELD_W - STG_TITLE_W) / 2, (STG_FIELD_H - STG_TITLE_H) / 2,
                   STG_TITLE_W, STG_TITLE_H, GFX_EQ_ALPHA, 0))
            d->st->title++;
    } else if (g->phase == STG_PH_CLEAR) {
        const char    *s = "CLEAR";
        const uint32_t w = stg_text_width(&d->cfg->font, s);

        text(d, &d->cfg->font, (int)(((uint32_t)STG_FIELD_W - w) / 2u),
             (int)(((uint32_t)STG_FIELD_H - d->cfg->font.cell_h) / 2u), s);
    }
}

/* --- HUD -------------------------------------------------------------------- */

#define HUD_X0      400
#define HUD_TX      424         /* 文字的左边 */
#define HUD_VX      514         /* 同一行的数值 */

/* 十进制，width 不为 0 时左补零到 width 位。buf 至少 12 字节。 */
static void fmt_u32(char *buf, uint32_t v, uint32_t width)
{
    char     tmp[11];
    uint32_t n = 0u, i = 0u;

    do {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v != 0u && n < 10u);
    while (n < width && n < 10u)
        tmp[n++] = '0';
    while (n != 0u)
        buf[i++] = tmp[--n];
    buf[i] = '\0';
}

static void value(dctx *d, int x, int y, uint32_t v, int valid, uint32_t width)
{
    char buf[12];

    if (valid)
        fmt_u32(buf, v, width);
    else {
        buf[0] = '-';
        buf[1] = '-';
        buf[2] = '\0';
    }
    text(d, &d->cfg->font, x, y, buf);
}

/* 大号数字：digits.png 第 0~9 格是 0~9，第 10 格是 '-'。 */
static void big_number(dctx *d, int x, int y, uint32_t v)
{
    char     buf[12];
    uint32_t i;

    fmt_u32(buf, v, 0u);
    for (i = 0u; buf[i] != '\0'; i++)
        (void)sprite(d, d->cfg->digits_slot, (int)((uint32_t)(buf[i] - '0') * (uint32_t)STG_DIGIT_W), 0,
                     x + (int32_t)i * STG_DIGIT_W, y, STG_DIGIT_W, STG_DIGIT_H, GFX_EQ_COPY, 1);
}

static void icons(dctx *d, uint32_t icon, uint32_t n, int y)
{
    uint32_t i;

    for (i = 0u; i < n && i < 3u; i++)
        (void)sprite(d, d->cfg->icons_slot, (int)(icon * (uint32_t)STG_ICON_PX), 0,
                     HUD_VX + (int32_t)i * 22, y, STG_ICON_PX, STG_ICON_PX, GFX_EQ_COPY, 1);
}

static void draw_hud(dctx *d, const stg_game *g, const stg_hud_live *live)
{
    const stg_font *lab = &d->cfg->font_cyan;

    if(!d->static_base)
        fill(d, HUD_X0, 0, 639, 479, STG_HUD_BG, GFX_EQ_COPY, 0u);
    fill(d, HUD_X0, 0, HUD_X0 + 1, 479, STG_HUD_LINE, GFX_EQ_COPY, 0u);
    if (sprite(d, d->cfg->title_slot, 0, 0, HUD_X0, 4, STG_TITLE_W, STG_TITLE_H, GFX_EQ_ALPHA, 0))
        d->st->title++;

    text(d, lab, HUD_TX, 100, "SCORE");
    value(d, HUD_TX, 118, g->score, 1, 9u);
    text(d, lab, HUD_TX, 142, "HI-SCORE");
    value(d, HUD_TX, 160, g->hiscore, 1, 9u);
    text(d, lab, HUD_TX, 188, "PLAYER");
    icons(d, STG_ICON_PLAYER, g->lives, 188);
    text(d, lab, HUD_TX, 214, "BOMB");
    icons(d, STG_ICON_BOMB, g->bombs, 214);
    text(d, lab, HUD_TX, 240, "GRAZE");
    value(d, HUD_VX, 240, g->graze, 1, 0u);
    fill(d, HUD_TX, 266, 613, 266, STG_HUD_RULE, GFX_EQ_COPY, 0u);
    text(d, &d->cfg->font, HUD_TX, 280, (live->seg_text != 0) ? live->seg_text : stg_game_seg_name(g->seg));
    text(d, lab, HUD_TX, 312, "N (ACTIVE SPRITES)");
    big_number(d, HUD_TX, 332, d->st->act.total);
    text(d, lab, HUD_TX, 378, "BULLET");
    value(d, HUD_VX, 378, stg_pool_count(&g->f.pools.ebullet), 1, 0u);
    text(d, lab, HUD_TX, 408, "SW FPS");
    value(d, HUD_VX, 408, live->sw_fps, live->sw_fps_valid, 0u);
    text(d, lab, HUD_TX, 434, "HW FPS");
    value(d, HUD_VX, 434, live->hw_fps, live->hw_fps_valid, 0u);
}

/* --- 一帧 --------------------------------------------------------------------- */

int stg_game_compose_layers(const stg_game *g,gfx_list *l,gfx_list *background,
                            int bg_dst,int static_base,const stg_draw_cfg *cfg,
                            const stg_hud_live *live,stg_frame_stats *st)
{
    stg_frame_stats zero = { { 0u, 0u, 0u, 0u, 0u, 0u }, 0u, 0u, 0u, { 0u }, 0u };
    stg_active     *a = &st->act;
    dctx            d;
    gfx_rect        screen;

    *st = zero;
    if (l == 0 || l->t == 0 || l->ended)
        return -1;

    d.l   = l;
    d.cfg = cfg;
    d.st  = st;
    d.dst = cfg->pool.dst;
    d.static_base = static_base;
    stg_rect_field(&d.clip);
    stg_rect_screen(&screen);

    if (gfx_set_clip(l, d.clip) != 0)
        st->rejected++;
    FRONTEND_STAGE(0,l);
    if(background) {
        d.l=background;d.dst=bg_dst;
        if(gfx_set_clip(background,d.clip))st->rejected++;
    }
    draw_background(&d, &g->bg);
    if(static_base)st->by_eq[GFX_EQ_COPY]++;
    d.l=l;d.dst=cfg->pool.dst;
    FRONTEND_STAGE(1,l);
    draw_enemies(&d, g);
    draw_player(&d, g);
    FRONTEND_STAGE(2,l);
    a->pbullet  = stg_draw_pool(&g->f.pools.pbullet, l, &cfg->pool, &d.clip, st->by_eq);
    FRONTEND_STAGE(3,l);
    a->ebullet  = stg_draw_pool(&g->f.pools.ebullet, l, &cfg->pool, &d.clip, st->by_eq);
    FRONTEND_STAGE(4,l);
    a->particle = stg_draw_pool(&g->f.pools.particle, l, &cfg->pool, &d.clip, st->by_eq);
    FRONTEND_STAGE(5,l);
    a->total    = a->ebullet + a->pbullet + a->enemy + a->particle + a->player;
    draw_banner(&d, g);
    if (g->flash) {
        fill(&d, 0, 0, STG_FIELD_W - 1, STG_FIELD_H - 1, STG_FLASH_ARGB, GFX_EQ_CONST, (uint8_t)STG_FLASH_C);
        st->flash = (st->by_eq[GFX_EQ_CONST] != 0u) ? 1u : 0u;
    }

    FRONTEND_STAGE(6,l);
    if (gfx_set_clip(l, screen) != 0)
        st->rejected++;
    d.clip = screen;
    if(background){d.l=background;d.dst=bg_dst;if(gfx_set_clip(background,screen))st->rejected++;}
    draw_hud(&d, g, live);
    if(static_base)st->by_eq[GFX_EQ_COPY]++;
    if(background&&gfx_list_end(background))st->rejected++;
    FRONTEND_STAGE(7,l);
    if (gfx_list_end(l) != 0)
        st->rejected++;
    FRONTEND_STAGE(8,l);
    return 0;
}

int stg_game_compose(const stg_game *g,gfx_list *l,const stg_draw_cfg *cfg,
                     const stg_hud_live *live,stg_frame_stats *st)
{return stg_game_compose_layers(g,l,0,0,0,cfg,live,st);}
