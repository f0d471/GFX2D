/******************************************************************************
* @file stg_modes.c: 模式框架里的模式 2（游戏）与模式 3（压力测试）
*
* @brief  口径见 stg_modes.h 与 sw 第 07 章 3.7、3.8 节。游戏状态、搜索状态机与本文件的计数都是静态的，
*         不用 malloc；游戏状态每次离开、进入都整体重置。
******************************************************************************/

#include "stg_modes.h"

#include <stddef.h>

#include "board.h"

#include "core/crc32.h"
#include "core/format.h"
#include "core/nmax.h"
#include "gfx/census.h"

#include "assets.h"

#include "game/stg_auto.h"
#include "game/stg_game.h"
#include "game/stg_kind.h"
#include "game/stg_press.h"

#include "stg_slots.h"
#include "renderer.h"

/* --- 状态 ---------------------------------------------------------------- */

static stg_game   s_game;           /* 模式 2、3 共用的游戏状态，约 390 KB */
static uint32_t   s_fail;
static uint32_t   s_idle;           /* 模式 2：连续无按键的帧数（自动驾驶接管用） */

/* 模式 3 的搜索 */
#define PRESS_16    0u              /* 先搜 16x16 圆弹 */
#define PRESS_8     1u              /* 再搜 8x8 小圆弹 */
#define PRESS_DONE  2u              /* 两遍都搜完，停在 8x8 的结果那一档 */
#define PRESS_FIXED 3u              /* -DGFX2D_PRESS_KNOB=<K>：不搜索，16x16、旋钮固定为 K */
#ifndef GFX2D_PRESS_PREWARM_STEPS
#define GFX2D_PRESS_PREWARM_STEPS 0
#endif

static nmax_state      s_nmax;
static stg_press_clock s_clock;
static uint32_t        s_press_stage;
static uint32_t        s_press_nmax[2];     /* 两遍的结果（N_max），HUD 用 */
#if SW9_PERF
static int             s_press_prime;       /* 流水切段后的首帧只预热，不喂搜索器 */
#endif
static char            s_press_text[32];    /* HUD 段名那一行 */
static logq           *s_log;               /* 日志队列；NULL 时直接 board_puts */

/* --- 公共 ---------------------------------------------------------------- */

void stg_modes_draw_cfg(stg_draw_cfg *c, int dst)
{
    c->pool.dst                     = dst;
    c->pool.src_slot[STG_SRC_BUL16] = STG_SLOT_BUL16;
    c->pool.src_slot[STG_SRC_BUL8]  = STG_SLOT_BUL8;
    c->pool.src_slot[STG_SRC_RICE]  = STG_SLOT_RICE;
    c->pool.src_slot[STG_SRC_PSHOT] = STG_SLOT_PSHOT;
    c->pool.src_slot[STG_SRC_SPARK] = STG_SLOT_SPARK;
    c->pool.player_slot             = STG_SLOT_SHIPS;
    c->pool.player_w                = (int32_t)ASSET_SHIPS_CELL_W;
    c->pool.player_h                = (int32_t)ASSET_SHIPS_CELL_H;
    c->pool.player_key              = ASSET_SHIPS_KEY;
    c->ships_slot                   = STG_SLOT_SHIPS;
    c->boss_slot                    = STG_SLOT_BOSS;
    c->stars_slot                   = STG_SLOT_STARS;
    c->title_slot                   = STG_SLOT_TITLE;
    c->digits_slot                  = STG_SLOT_DIGITS;
    c->icons_slot                   = STG_SLOT_ICONS;
    c->font.slot                    = STG_SLOT_FONT;
    c->font.cell_w                  = ASSET_FONT8X16_CELL_W;
    c->font.cell_h                  = ASSET_FONT8X16_CELL_H;
    c->font.cols                    = ASSET_FONT8X16_COLS;
    c->font.first                   = 0x20u;
    c->font.cells                   = ASSET_FONT8X16_CELLS;
    c->font.key                     = 0x00000000u;
    c->font_cyan                    = c->font;
    c->font_cyan.slot               = STG_SLOT_FONT_CYAN;
    c->font_cyan.cols               = ASSET_FONT_CYAN_COLS;
    c->font_cyan.cells              = ASSET_FONT_CYAN_CELLS;
    c->font_cyan.key                = ASSET_FONT_CYAN_KEY;
}

uint32_t stg_modes_frame_crc(const uint32_t *fb)
{
    const uint8_t *p0 = (const uint8_t *)fb;
    crc32_ctx      c;
    uint32_t       y;

    crc32_begin(&c);
    for (y = 0u; y < (uint32_t)FB_HEIGHT; y++)
        crc32_update(&c, p0 + (size_t)y * FB_STRIDE_BYTES, (uint32_t)FB_WIDTH * 4u);
    return crc32_end(&c);
}

void stg_modes_reset(void)
{
#ifdef INJECT_MODE_KEEP_POOL
    const uint32_t keep = s_game.f.pools.ebullet.n;     /* 注错：漏清敌弹池 */
#endif
    stg_game_init(&s_game, STG_GAME_SEED);
#ifdef INJECT_MODE_KEEP_POOL
    s_game.f.pools.ebullet.n = keep;
#endif
}

void stg_modes_pools(uint32_t pool[4])
{
    pool[0] = stg_pool_count(&s_game.f.pools.ebullet);
    pool[1] = stg_pool_count(&s_game.f.pools.pbullet);
    pool[2] = stg_enemies_alive(&s_game.en);
    pool[3] = stg_pool_count(&s_game.f.pools.particle);
}

uint32_t stg_modes_fail(void)
{
    return s_fail;
}

int stg_modes_press_done(void)
{
    return s_press_stage == PRESS_DONE;
}

void stg_modes_set_log(logq *q)
{
    s_log = q;
}

uint32_t stg_modes_overflow(void)
{
    return stg_pool_overflow(&s_game.f.pools.ebullet);
}

static void say_line(const char *s)
{
    char     buf[256];
    core_out o;

    core_out_init(&o, buf, (uint32_t)sizeof buf);
    core_out_str(&o, s);
    core_out_str(&o, "\r\n");
    if (s_log != NULL)
        (void)logq_put(s_log, buf);
    else
        board_puts(buf);
}

static void say_err(const char *what, uint32_t v)
{
    char     buf[80];
    core_out o;

    core_out_init(&o, buf, (uint32_t)sizeof buf);
    core_out_str(&o, "err ");
    core_out_str(&o, what);
    core_out_str(&o, " v ");
    core_out_u32(&o, v);
    say_line(buf);
    s_fail++;
}

/* 组帧、普查自查、执行、整帧 CRC、呈现。census 不为 NULL 时把普查结果交回去（模式 3 算代价）。 */
static void draw_frame(const stg_main_ctx *ctx, int dst, const stg_hud_live *live, stg_mode_out *o,
                       gfx_census *census)
{
    const uint32_t   idx = (dst == STG_SLOT_FB1) ? 1u : 0u;
    gfx_list         l;
    stg_draw_cfg     cfg;
#if !SW9_PERF || !GFX2D_PRESS_CLOCK
    gfx_census_state cs;
#endif
    gfx_census       cc;
    exec_sw_stats    xs;
#if !SW9_PERF || !GFX2D_PRESS_CLOCK
    uint32_t         e;
#endif
    int              rc;
    uint32_t         t0 = board_cycles();

    gfx_list_begin(&l, ctx->cmds[idx], ctx->cap[idx], ctx->tab);
    stg_modes_draw_cfg(&cfg, dst);
    if (stg_game_compose(&s_game, &l, &cfg, live, &o->st) != 0)
        say_err("compose", 0u);
    if (l.rejected != 0u || l.overflow != 0u || o->st.rejected != 0u)
        say_err("list rejected/overflow", l.rejected + l.overflow + o->st.rejected);

    gfx_census_clear(&cc);
#if !SW9_PERF || !GFX2D_PRESS_CLOCK
    gfx_census_reset(&cs);
    if (gfx_census_run(&cs, ctx->tab, l.buf, l.used, &cc) != 0)
        say_err("census", 0u);
    for (e = 0u; e <= GFX_EQ_MAX; e++)
        if (cc.cmd_by_eq[e] != o->st.by_eq[e])
            say_err("census eq", e);
#endif
    if (census != NULL)
        *census = cc;
    o->cyc_build = board_cycles() - t0;             /* 组帧含普查自查 */

    o->presented_dst = -1;
    if (renderer_pipeline_enabled(ctx->hw)) {
        exec_sw_stats_clear(&xs);
        rc = renderer_pipeline_step(&l, dst, &o->cyc_exec,
                                    &o->t_present, &o->presented_dst);
    } else {
        rc = renderer_run(ctx->hw, ctx->tab, ctx->mem, ctx->es, ctx->err,
                          &l, &xs, &o->cyc_exec);
    }
    if (rc != GFX_OK)
        say_err("exec", (uint32_t)rc);
    o->list_idx = idx;
    o->used     = l.used;
    o->xs       = xs;
#if SW9_PERF
    o->crc = 0u;
#else
    if (ctx->hw)
        board_dcache_invalidate_all();
    o->crc = stg_modes_frame_crc(board_fb(dst));
#endif
    if (!renderer_pipeline_enabled(ctx->hw)) {
        uint32_t ticket;
        o->t_present = board_cycles();
        if (board_present_request(dst, &ticket) != 0 || board_present_wait(ticket) != 0)
            say_err("present", (uint32_t)dst);
        else
            o->presented_dst = dst;
    }
}

/* --- 模式 2：游戏 -------------------------------------------------------- */

void stg_mode_game_enter(void)
{
    stg_modes_reset();
    s_idle = 0u;
}

void stg_mode_game_frame(const stg_main_ctx *ctx, int dst, uint32_t keys,
                         const stg_hud_live *live, stg_mode_out *o)
{
    uint32_t t0 = board_cycles();

    /* 自动驾驶：连续 600 帧无按键就接管，任何按键立即交还（sw 第 07 章 3.10 节）；开销单独计 */
    o->auto_on = (uint32_t)stg_auto_take(&s_idle, keys);
    if (o->auto_on)
        keys = stg_auto_keys(&s_game, NULL);
    o->cyc_auto = board_cycles() - t0;

    t0 = board_cycles();
    stg_game_step(&s_game, keys);
    o->cyc_logic = board_cycles() - t0;
    draw_frame(ctx, dst, live, o, NULL);
}

/* --- 模式 3：压力测试 ---------------------------------------------------- */

static void press_text(void)
{
    core_out o;

    core_out_init(&o, s_press_text, (uint32_t)sizeof s_press_text);
    core_out_str(&o, GFX2D_PRESS_CLOCK ? "REAL " : "SIM ");
    if (s_press_stage == PRESS_FIXED) {
        core_out_str(&o, "K ");
        core_out_u32(&o, (uint32_t)GFX2D_PRESS_KNOB);
    } else if (s_press_stage == PRESS_DONE) {
        core_out_str(&o, "NMAX ");
        core_out_u32(&o, s_press_nmax[0]);
        core_out_ch(&o, '/');
        core_out_u32(&o, s_press_nmax[1]);
    } else {
        core_out_str(&o, (s_press_stage == PRESS_16) ? "16X16 K " : "8X8 K ");
        core_out_u32(&o, s_nmax.knob);
    }
}

/* 一遍搜索的开始：游戏进终符的压力形态、负载弹种、nmax 的配置与第一档。 */
static void press_begin(uint32_t stage)
{
    nmax_cfg cfg;
    char     buf[240];

    s_press_stage = stage;
#if SW9_PERF
    s_press_prime = 1;
#endif
    say_line((stage == PRESS_16) ? "nmax load 16x16" : "nmax load 8x8");
    stg_game_press_begin(&s_game, (stage == PRESS_16) ? STG_KIND_ROUND16 : STG_KIND_SMALL8);
    nmax_cfg_default(&cfg);
    cfg.settle_min = STG_MODES_SETTLE_MIN;
    if (nmax_init(&s_nmax, &cfg, s_clock.field_cyc) != 0)
        say_err("nmax cfg", stage);
    (void)nmax_format_cfg(&s_nmax, buf, (uint32_t)sizeof buf);
    say_line(buf);
    stg_game_set_knob(&s_game, s_nmax.knob);
}

void stg_mode_press_enter(void)
{
    stg_modes_reset();
    stg_press_clock_b(&s_clock);
    s_press_nmax[0] = 0u;
    s_press_nmax[1] = 0u;
#if GFX2D_PRESS_KNOB != 0
    {
        char     buf[64];
        core_out o;

        s_press_stage = PRESS_FIXED;
        stg_game_press_begin(&s_game, STG_KIND_ROUND16);
        stg_game_set_knob(&s_game, (uint32_t)GFX2D_PRESS_KNOB);
#if GFX2D_PRESS_PREWARM_STEPS > 0
        for (uint32_t i = 0u; i < (uint32_t)GFX2D_PRESS_PREWARM_STEPS; i++)
            stg_game_step(&s_game, 0u);
        core_out_init(&o, buf, (uint32_t)sizeof buf);
        core_out_str(&o, "press prewarm steps ");
        core_out_u32(&o, (uint32_t)GFX2D_PRESS_PREWARM_STEPS);
        say_line(buf);
#endif
        core_out_init(&o, buf, (uint32_t)sizeof buf);
        core_out_str(&o, "press fixed knob ");
        core_out_u32(&o, (uint32_t)GFX2D_PRESS_KNOB);
        core_out_str(&o, " kind 16x16");
        say_line(buf);
    }
#else
    press_begin(PRESS_16);
#endif
}

void stg_mode_press_frame(const stg_main_ctx *ctx, int dst, const stg_hud_live *live,
                          stg_mode_out *o)
{
    stg_hud_live h = *live;
    gfx_census   cc;
    uint64_t     cost;
    uint32_t     e, cyc, ev;
    const uint32_t f0 = board_frame_count();
    const uint32_t t0 = board_cycles();
    char         buf[240];

    o->cyc_auto = 0u;
    o->auto_on  = 0u;
    stg_game_step(&s_game, 0u);
    o->cyc_logic = board_cycles() - t0;
    press_text();
    h.seg_text = s_press_text;
#if !GFX2D_PRESS_CLOCK
    /* 模拟时钟的代价按本帧整帧的普查算，HUD 也在里面。SW FPS 一栏若显示按墙钟量的实测值，
       它的位数会改变命令数与像素数，进而改变模拟的掉帧判定：同一份代码在不同机器上搜出不同的 N_max。
       所以模拟时钟下这一栏一律 --（SW6 判据模式的做法，sw 第 07 章 4.8 节）。 */
    h.sw_fps_valid = 0;
#endif
    draw_frame(ctx, dst, &h, o, &cc);

    /* 模拟时钟：本帧整帧的普查按 B 批的模型算代价；真实时钟：场数增量与周期数 */
    e   = stg_press_cost(&s_clock, &cc, &cost);
    cyc = (cost > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)cost;
#if GFX2D_PRESS_CLOCK
    e   = board_frame_count() - f0;
    cyc = o->t_present - t0;        /* 帧时间到发出呈现请求为止，不含等场（2.2 节） */
#else
    (void)f0;
    (void)t0;
#endif
    if (s_press_stage == PRESS_DONE || s_press_stage == PRESS_FIXED)
        return;
#if SW9_PERF
    if (renderer_pipeline_enabled(ctx->hw) && s_press_prime) {
        s_press_prime = 0;
        return;
    }
#endif

    ev = nmax_feed(&s_nmax, e, o->st.act.total, cyc);
    if (ev & NMAX_EV_PROBE) {
        (void)nmax_format_probe(&s_nmax.last, buf, (uint32_t)sizeof buf);
        say_line(buf);
    }
    if (ev & NMAX_EV_KNOB)
        stg_game_set_knob(&s_game, s_nmax.knob);
    if (ev & NMAX_EV_DONE) {
        (void)nmax_format_result(&s_nmax, buf, (uint32_t)sizeof buf);
        say_line(buf);
        if (s_nmax.status != NMAX_OK
#if GFX2D_PRESS_CLOCK
            && s_nmax.status != NMAX_BELOW_N0
#endif
        )
            say_err("nmax status", s_nmax.status);
        s_press_nmax[s_press_stage] = s_nmax.n_max;
        if (s_press_stage == PRESS_16) {
            press_begin(PRESS_8);
        } else {
            /* 两遍都搜完：停在 8x8 结果那一档继续跑 */
            s_press_stage = PRESS_DONE;
            stg_game_set_knob(&s_game, s_nmax.res_knob);
            say_line("nmax done");
        }
    }
}
