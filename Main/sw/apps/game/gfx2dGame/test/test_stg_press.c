/******************************************************************************
* @file test_stg_press.c: N_max 搜索跑在弹幕场上（SW8 2.3 节，B 批）的主机侧测试
*
* @brief  SW8 第 3 节「搜索跑在弹幕场上」一行。16x16 与 8x8 两种负载各一遍：
*           Y1  定寿命负载：旋钮 K 稳定之后池里恰好 K 颗，发出的活动元素 = 可见的负载弹 + 自机
*           Y2  搜索用 lib/core/nmax 与 stg_press（模拟时钟）跑到结束，状态 OK，逐档轨迹照打
*           Y3  另起一个场，在结果的旋钮上等涨潮（2L 帧）再跑 T_CONFIRM 场：零掉帧
*           Y4  另起一个场，在「结果 + STEP_MIN」上等涨潮再跑 T_PROBE 场：有掉帧
*         Y3、Y4 合起来说明结果离真实门槛不到 STEP_MIN，不需要解析解（场上的活动元素数随出界带里的
*         半可见弹起伏，解析解只能近似，见 SW8 4.9 节第 3 条）；「每颗都整个可见」的粗估打在旁边作参考。
*
*         状态机本身的正确性（解析解、单调、下界、尖峰、注错）在 lib/test/test_nmax.c 里用综合负载测。
******************************************************************************/

#include <stdint.h>
#include <stdio.h>

#include "core/format.h"
#include "core/nmax.h"
#include "gfx/gfx.h"
#include "game/stg_field.h"
#include "game/stg_kind.h"
#include "game/stg_press.h"
#include "test_stg_press.h"

#define SLOT_FB0    0
#define SLOT_BUL16  2
#define SLOT_BUL8   3
#define SLOT_RICE   4
#define SLOT_PSHOT  6
#define SLOT_SPARK  7
#define SLOT_SHIPS  8
#define LIST_CAP    32768u
#define FRAME_LIMIT 200000u

static uint32_t          g_fail;
static stg_field         s_field;
static gfx_cmd           s_cmds[LIST_CAP];
static gfx_surface_table s_tab;
static stg_compose_cfg   s_cfg;
static stg_press_clock   s_ck;
static nmax_state        s_st;

static void say(const char *s)
{
    fputs(s, stdout);
    fflush(stdout);
}

static void ck(const char *tag, int ok)
{
    char     line[220];
    core_out o;

    core_out_init(&o, line, (uint32_t)sizeof line);
    core_out_str(&o, tag);
    core_out_str(&o, ok ? " ok\n" : " FAIL\n");
    say(line);
    if (!ok)
        g_fail++;
}

static int surf(int slot, uint32_t base, uint16_t w, uint16_t h, uint8_t sl2, uint8_t fmt)
{
    gfx_surface s;

    s.base        = base;
    s.width       = w;
    s.height      = h;
    s.stride_log2 = sl2;
    s.space       = GFX_SPACE_DDR;
    s.format      = fmt;
    return gfx_surface_set(&s_tab, slot, &s);
}

static int setup(void)
{
    int rc = 0;

    gfx_surface_reset(&s_tab);
    rc |= surf(SLOT_FB0,    0x02000000u, 640u, 480u, 12u, GFX_FMT_XRGB8888);
    rc |= surf(SLOT_BUL16,  0x01000000u,  64u,  16u,  8u, GFX_FMT_ARGB8888_PM);
    rc |= surf(SLOT_BUL8,   0x01010000u,  32u,   8u,  7u, GFX_FMT_ARGB8888_PM);
    rc |= surf(SLOT_RICE,   0x01020000u, 320u,  80u, 11u, GFX_FMT_ARGB8888_PM);
    rc |= surf(SLOT_PSHOT,  0x01050000u,   6u,  24u,  5u, GFX_FMT_ARGB8888_PM);
    rc |= surf(SLOT_SPARK,  0x01060000u,  32u,  40u,  7u, GFX_FMT_ARGB8888_PM);
    rc |= surf(SLOT_SHIPS,  0x01100000u, 128u,  96u,  9u, GFX_FMT_XRGB8888);
    s_cfg.dst                     = SLOT_FB0;
    s_cfg.src_slot[STG_SRC_BUL16] = SLOT_BUL16;
    s_cfg.src_slot[STG_SRC_BUL8]  = SLOT_BUL8;
    s_cfg.src_slot[STG_SRC_RICE]  = SLOT_RICE;
    s_cfg.src_slot[STG_SRC_PSHOT] = SLOT_PSHOT;
    s_cfg.src_slot[STG_SRC_SPARK] = SLOT_SPARK;
    s_cfg.player_slot             = SLOT_SHIPS;
    s_cfg.player_w                = 32;
    s_cfg.player_h                = 32;
    s_cfg.player_key              = 0x00000000u;
    stg_press_clock_b(&s_ck);
    return rc;
}

static void frame(stg_press_out *o)
{
    gfx_list l;

    gfx_list_begin(&l, s_cmds, LIST_CAP, &s_tab);
    stg_press_frame(&s_field, &l, &s_cfg, &s_ck, o);
}

/* --- Y1 负载 ------------------------------------------------------------- */

static void y1_load(uint32_t kind, const char *tag)
{
    stg_press_out o;
    uint32_t      k, bad = 0u, vis_lo = 0xFFFFFFFFu, vis_hi = 0u;

    stg_press_begin(&s_field, kind);
    stg_field_set_pressure(&s_field, 1000u);
    for (k = 0u; k < 2u * STG_PRESS_LIFE + 50u; k++) {
        frame(&o);
        if (o.bad != 0u)
            bad++;
        if (k >= 2u * STG_PRESS_LIFE) {
            if (stg_pool_count(&s_field.pools.ebullet) != 1000u)
                bad++;
            if (o.act.player != 1u || o.act.total != o.act.ebullet + 1u)
                bad++;
            if (o.act.ebullet < vis_lo)
                vis_lo = o.act.ebullet;
            if (o.act.ebullet > vis_hi)
                vis_hi = o.act.ebullet;
        }
    }
    {
        char     line[160];
        core_out w;

        core_out_init(&w, line, (uint32_t)sizeof line);
        core_out_str(&w, tag);
        core_out_str(&w, " knob 1000 steady: pool 1000, visible enemy bullets ");
        core_out_u32(&w, vis_lo);
        core_out_str(&w, "..");
        core_out_u32(&w, vis_hi);
        core_out_str(&w, "\n");
        say(line);
    }
    /* 可见的帧数（SW8 2.3 节）：第 k 帧中心在 y = -10 + 4k 像素。16x16 在 k = 1..124 可见（124 帧），
       8x8 在 k = 2..123 可见（122 帧）；Bresenham 让任意 m 帧恰好发 m*K/L 颗（这里是整数），所以是定值 */
    ck(kind == STG_KIND_SMALL8
       ? "Y1 8x8 knob 1000: pool exactly 1000 for 50 frames, visible 976 (122/125), player 1"
       : "Y1 16x16 knob 1000: pool exactly 1000 for 50 frames, visible 992 (124/125), player 1",
       bad == 0u && vis_lo == vis_hi && vis_lo == ((kind == STG_KIND_SMALL8) ? 976u : 992u));
}

/* --- Y2 搜索 ------------------------------------------------------------- */

static uint32_t search(uint32_t kind, uint32_t *frames_out)
{
    nmax_cfg      cfg;
    stg_press_out o;
    uint32_t      frames = 0u, bad = 0u;
    char          line[240];

    nmax_cfg_default(&cfg);
    (void)nmax_init(&s_st, &cfg, s_ck.field_cyc);
    (void)nmax_format_cfg(&s_st, line, (uint32_t)sizeof line);
    say(line);
    say("\n");

    stg_press_begin(&s_field, kind);
    stg_field_set_pressure(&s_field, s_st.knob);
    while (s_st.status == NMAX_RUNNING && frames < FRAME_LIMIT) {
        uint32_t ev;

        frame(&o);
        bad += o.bad;
        ev = nmax_feed(&s_st, o.e, o.act.total, o.frame_cyc);
        frames++;
        if (ev & NMAX_EV_PROBE) {
            (void)nmax_format_probe(&s_st.last, line, (uint32_t)sizeof line);
            say(line);
            say("\n");
        }
        if (ev & NMAX_EV_KNOB)
            stg_field_set_pressure(&s_field, s_st.knob);
    }
    (void)nmax_format_result(&s_st, line, (uint32_t)sizeof line);
    say(line);
    say("\n");
    *frames_out = frames;
    return bad;
}

/* 在旋钮 knob 上等涨潮 2L 帧，再跑 fields 场，返回掉帧数。 */
static uint32_t drops_at(uint32_t kind, uint32_t knob, uint32_t fields)
{
    stg_press_out o;
    uint32_t      k, got = 0u, drops = 0u;

    stg_press_begin(&s_field, kind);
    stg_field_set_pressure(&s_field, knob);
    for (k = 0u; k < 2u * STG_PRESS_LIFE; k++)
        frame(&o);
    while (got < fields) {
        frame(&o);
        got += o.e;
        drops += o.e - 1u;
    }
    return drops;
}

static void y_search(uint32_t kind, const char *tag, uint32_t approx)
{
    uint32_t frames = 0u, bad, knob, d_ok, d_up;
    char     line[200];
    core_out w;

    bad = search(kind, &frames);
    knob = s_st.res_knob;
    core_out_init(&w, line, (uint32_t)sizeof line);
    core_out_str(&w, tag);
    core_out_str(&w, " search: ");
    core_out_str(&w, nmax_status_name(s_st.status));
    core_out_str(&w, " n_max ");
    core_out_u32(&w, s_st.n_max);
    core_out_str(&w, " knob ");
    core_out_u32(&w, knob);
    core_out_str(&w, " frames ");
    core_out_u32(&w, frames);
    core_out_str(&w, " (estimate if every bullet were full-size ");
    core_out_u32(&w, approx);
    core_out_str(&w, ")\n");
    say(line);
    ck(kind == STG_KIND_SMALL8 ? "Y2 8x8 search ends OK, no rejected commands"
                               : "Y2 16x16 search ends OK, no rejected commands",
       s_st.status == NMAX_OK && bad == 0u && knob != 0u);
    if (s_st.status != NMAX_OK)
        return;

    d_ok = drops_at(kind, knob, s_st.cfg.t_confirm);
    d_up = drops_at(kind, knob + s_st.cfg.step_min, s_st.cfg.t_probe);
    core_out_init(&w, line, (uint32_t)sizeof line);
    core_out_str(&w, tag);
    core_out_str(&w, " verify: knob ");
    core_out_u32(&w, knob);
    core_out_str(&w, " drops over T_CONFIRM ");
    core_out_u32(&w, d_ok);
    core_out_str(&w, "; knob ");
    core_out_u32(&w, knob + s_st.cfg.step_min);
    core_out_str(&w, " drops over T_PROBE ");
    core_out_u32(&w, d_up);
    core_out_str(&w, "\n");
    say(line);
    ck(kind == STG_KIND_SMALL8 ? "Y3 8x8 result knob: zero drops over T_CONFIRM on a fresh field"
                               : "Y3 16x16 result knob: zero drops over T_CONFIRM on a fresh field",
       d_ok == 0u);
    ck(kind == STG_KIND_SMALL8 ? "Y4 8x8 result + STEP_MIN: drops within T_PROBE"
                               : "Y4 16x16 result + STEP_MIN: drops within T_PROBE",
       d_up > 0u);
}

uint32_t test_stg_press_run(void)
{
    g_fail = 0u;
    ck("Y0 surface table registers", setup() == 0);
    y1_load(STG_KIND_ROUND16, "Y1 16x16");
    y1_load(STG_KIND_SMALL8, "Y1 8x8");
    y_search(STG_KIND_ROUND16, "Y 16x16", 2374u);
    y_search(STG_KIND_SMALL8, "Y 8x8", 6373u);
    return g_fail;
}
