/******************************************************************************
* @file stg_modes.c: 模式框架里的模式 2（游戏）与模式 3（压力测试）
*
* @brief  口径见 stg_modes.h。游戏状态、搜索状态机与本文件的计数都是静态的，
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
#include "frontend_game.h"
#include "game/myriad_hud.h"

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
    c->bglayers_slot                = STG_SLOT_BGLAYERS;
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
    if(frontend_game_reset()) {s_fail++;return;}
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
static int (*s_presenter)(int,uint32_t *);
static int (*s_frame_begin)(gfx_surface_table *,int,stg_draw_view *);
void stg_modes_set_presenter(int (*present)(int,uint32_t *)) {s_presenter=present;}
void stg_modes_set_frame_begin(int (*begin)(gfx_surface_table *,int,stg_draw_view *)) {s_frame_begin=begin;}

static void draw_frame(const stg_main_ctx *ctx, int dst, const stg_hud_live *live, stg_mode_out *o,
                       gfx_census *census)
{
    const uint32_t   idx = (dst == STG_SLOT_FB1) ? 1u : 0u;
    gfx_list         l;
    stg_draw_cfg     cfg;
    gfx_census       cc;
    exec_sw_stats    xs;
#if GFX_FRAME_DIAGNOSTIC || !GFX2D_PRESS_CLOCK
    uint32_t         e;
#endif
    int              rc;
    uint32_t         t0 = board_cycles();
    stg_draw_view view;
#ifndef __riscv
    gfx_surface_table view_tab;
    stg_main_ctx view_ctx=*ctx;
    if(s_frame_begin)view_tab=*ctx->tab;
    const int adaptive=s_frame_begin?s_frame_begin(&view_tab,dst,&view):1;
    if(adaptive==0) {view_ctx.tab=&view_tab;ctx=&view_ctx;}
#else
    /* 板上固定 640 画布与 PC 宽画布共用 SW18 视口排布:场景真实铺满
       [0,480),战场 400 居中,HUD 480..640。旧无视口路径场景止于 400,
       400..480 无人绘制,在板上表现为战场与 HUD 之间的黑带。 */
    view.width=(unsigned)(MYRIAD_HUD_X+MYRIAD_HUD_WIDTH);
    view.hud_x=MYRIAD_HUD_X;
    view.field_x=(MYRIAD_HUD_X-(int)STG_FIELD_W)/2;
    const int adaptive=0;
#endif

    if(adaptive<0) {say_err("frame layout",0);return;}

    o->presented_dst=-1;o->cyc_build=0;o->cyc_exec=0;o->crc=0;o->used=0;
    o->instances=NULL;o->instance_generation=0;
    exec_sw_stats_clear(&o->xs);
#if GFX_FRONTEND_BACKGROUND
    o->frame_plan=NULL;
#endif
    if (renderer_list_begin(ctx->hw, &l, ctx->cmds[idx], ctx->cap[idx], ctx->tab, dst) != GFX_OK) {
        say_err("list begin", 0u);
        return;
    }
    stg_modes_draw_cfg(&cfg, dst);
    if ((adaptive==0?stg_game_compose_view(&s_game,&l,&cfg,live,&o->st,&view):
         frontend_game_compose(&s_game,&l,&cfg,live,&o->st)) != 0) {
        say_err("compose", 0u);
        return;
    }
    o->instances=l.instances;o->instance_generation=l.instance_generation;
    if (l.rejected != 0u || l.overflow != 0u || o->st.rejected != 0u) {
        say_err("list rejected/overflow", l.rejected + l.overflow + o->st.rejected);
        return;
    }
#if GFX_FRONTEND_BACKGROUND
    o->frame_plan=l.frame;
#endif

    gfx_census_clear(&cc);
#if GFX_FRAME_DIAGNOSTIC || !GFX2D_PRESS_CLOCK
    if (renderer_list_census(ctx->tab, &l, &cc) != 0)
        say_err("census", 0u);
    for (e = 0u; e <= GFX_EQ_MAX; e++)
        if (l.strip_table == NULL && cc.cmd_by_eq[e] != o->st.by_eq[e])
            say_err("census eq", e);
#endif
    if (census != NULL)
        *census = cc;
    o->cyc_build = board_cycles() - t0;             /* 组帧含普查自查 */

#if defined(GFX2D_STRIP_RESEARCH) && GFX2D_STRIP_RESEARCH
    /* 条带研究探针：同一份游戏状态第二遍组 v2 条带表，只统计不执行。
       16 行 30 桶与 32 行 15 桶各一遍，桶峰值读表头计数数组。
       输出列：rcsv,u1,u2,pk16,cyc2,u3,pk32,cyc3,err2,err3,eqok；
       err=两遍各自的 rejected+overflow+first_error 异常，eqok=1 表示
       条带组帧的 by_eq 与直通一致。两遍都要 err 全 0、eqok=1。 */
    {
        static gfx_cmd  stbl[32768];                /* 1 MiB，一张定长 v2 表（D61） */
        gfx_list        s2;
        stg_frame_stats st2;
        gfx_strip_config rcfg;
        core_out        ro;
        char            rbuf[128];
        uint32_t        t1, t2, u1, u2, u3, pk16, pk32, i;
        uint32_t        e2, e3;
        int             eqok;

        eqok = 1;
        u1 = l.used;
        rcfg.bg_id = (uint32_t)dst;
        rcfg.dst_id = (uint32_t)dst;

        t1 = board_cycles();
        rcfg.lstrips = 30u;
        rcfg.strip_log2 = 4u;
        e2 = 1u;
        u2 = 0u;
        pk16 = 0u;
        if (gfx_list_begin_strips(&s2, stbl, (uint32_t)sizeof stbl, ctx->tab,
                                  &rcfg, 0) == 0 &&
            stg_game_compose(&s_game, &s2, &cfg, live, &st2) == 0) {
            for (i = 0u; i <= GFX_EQ_MAX; i++)
                if (st2.by_eq[i] != o->st.by_eq[i]) eqok = 0;
            u2 = s2.used;
            e2 = s2.rejected + s2.overflow + (s2.first_error != GFX_OK);
            for (i = 0u; i < 30u; i++) {
                uint32_t c = ((const uint32_t *)(const void *)stbl)[i];
                if (c > pk16)
                    pk16 = c;
            }
        }
        t1 = board_cycles() - t1;

        {
            uint32_t u3b = 0u;

            t2 = board_cycles();
            rcfg.lstrips = 15u;
            rcfg.strip_log2 = 5u;
            e3 = 1u;
            pk32 = 0u;
            if (gfx_list_begin_strips(&s2, stbl, (uint32_t)sizeof stbl, ctx->tab,
                                      &rcfg, 0) == 0 &&
                stg_game_compose(&s_game, &s2, &cfg, live, &st2) == 0) {
                u3b = s2.used;
                e3 = s2.rejected + s2.overflow + (s2.first_error != GFX_OK);
                for (i = 0u; i < 15u; i++) {
                    uint32_t c = ((const uint32_t *)(const void *)stbl)[i];
                    if (c > pk32)
                        pk32 = c;
                }
            }
            t2 = board_cycles() - t2;
            u3 = u3b;
        }

        for (i = 0u; i <= GFX_EQ_MAX; i++)
            if (st2.by_eq[i] != o->st.by_eq[i])
                eqok = 0;
        core_out_init(&ro, rbuf, (uint32_t)sizeof rbuf);
        core_out_str(&ro, "rcsv,");
        core_out_u32(&ro, u1);
        core_out_str(&ro, ",");
        core_out_u32(&ro, u2);
        core_out_str(&ro, ",");
        core_out_u32(&ro, pk16);
        core_out_str(&ro, ",");
        core_out_u32(&ro, t1);
        core_out_str(&ro, ",");
        core_out_u32(&ro, u3);
        core_out_str(&ro, ",");
        core_out_u32(&ro, pk32);
        core_out_str(&ro, ",");
        core_out_u32(&ro, t2);
        core_out_str(&ro, ",");
        core_out_u32(&ro, e2);
        core_out_str(&ro, ",");
        core_out_u32(&ro, e3);
        core_out_str(&ro, ",");
        core_out_u32(&ro, (uint32_t)eqok);
        core_out_str(&ro, "\r\n");
        board_puts(rbuf);
    }
#endif

    o->presented_dst = -1;
    if (renderer_pipeline_enabled(ctx->hw)) {
        rc = renderer_pipeline_step(&l, dst, &xs, &o->cyc_exec,
                                    &o->t_present, &o->presented_dst);
    } else {
        rc = adaptive==0?renderer_run_view(ctx->tab,ctx->mem,ctx->es,ctx->err,&l,&xs,&o->cyc_exec,view.width):
            renderer_run(ctx->hw,ctx->tab,ctx->mem,ctx->es,ctx->err,&l,&xs,&o->cyc_exec);
    }
    if (rc != GFX_OK) {
        say_err("exec", (uint32_t)rc);
#if GFX_FRONTEND_BACKGROUND
        o->frame_plan=NULL;
#endif
        o->list_idx=idx;o->used=l.used;o->xs=xs;o->crc=0u;
        return;
    }
    o->list_idx = idx;
    o->used     = l.used;
    o->xs       = xs;
#if !GFX_FRAME_DIAGNOSTIC
    o->crc = 0u;
#else
    if (ctx->hw)
        board_dcache_invalidate_all();
    o->crc = stg_modes_frame_crc(board_fb(dst));
#endif
    if (!renderer_pipeline_enabled(ctx->hw)) {
        uint32_t ticket;
        o->t_present = board_cycles();
        if ((s_presenter?s_presenter(dst,&ticket):board_present_request(dst,&ticket)) != 0 || board_present_wait(ticket) != 0)
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

    /* 自动驾驶：连续 600 帧无按键就接管，任何按键立即交还；开销单独计 */
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

    /* 模拟时钟：本帧整帧的普查按弹幕场的模型算代价；真实时钟：场数增量与周期数 */
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
