/******************************************************************************
* @file test_stg_view.c: 游戏的组帧、HUD 与共用文字绘制（SW7 2.6 节 C 批）的主机侧测试
*
* @brief  V 组，判据见 SW7 第 3 节「HUD 与游戏判据入口」一行（判据入口在 PC 上逐帧自查同样几件；
*         这里在 64 位 ASan/UBSan 与 -m32 下再跑一遍，不链 bsp）：
*           V1  stg_text_draw 与逐字调 gfx_blit（抽出之前 sw6_hud.c 的写法）生成的命令表逐字节相同，
*               含格数之外的字符（跳过、x 照样前进）
*           V2  按判据入口的按键脚本跑 3000 帧（受击关掉），每帧组帧：拒绝与表满为 0、以 END 结束；
*               第一条是游戏区 SET_CLIP，接着 29 条背景 FILL；普查从命令表另数的各方程条数等于组帧记下的，
*               ADD = 自机弹 + 粒子、ALPHA = 敌弹 + Boss + 标题图、CONST = 闪白；活动元素 = 五类之和
*           V3  标题帧：两张标题图（游戏区与 HUD），活动元素为 0；闪白帧：CONST 恰好一条
******************************************************************************/

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "core/format.h"
#include "gfx/census.h"
#include "gfx/gfx.h"
#include "game/stg_draw.h"
#include "game/stg_game.h"
#include "game/stg_kind.h"
#include "game/stg_script.h"
#include "test_stg_view.h"

/* 游戏的表面槽（SW7 2.7 节），与入口的 stg_slots.h 同值 */
#define SLOT_FB0        0
#define SLOT_BUL16      2
#define SLOT_BUL8       3
#define SLOT_RICE       4
#define SLOT_FONT       5
#define SLOT_PSHOT      6
#define SLOT_SPARK      7
#define SLOT_SHIPS      8
#define SLOT_BOSS       9
#define SLOT_STARS      10
#define SLOT_TITLE      11
#define SLOT_DIGITS     12
#define SLOT_ICONS      13
#define SLOT_FONT_CYAN  14

#define LIST_CAP        16384u

static uint32_t          g_fail;
static stg_game          s_g;
static gfx_cmd           s_cmds[LIST_CAP];
static gfx_cmd           s_ref[LIST_CAP];
static gfx_surface_table s_tab;

static void ck(const char *tag, int ok)
{
    char     line[200];
    core_out o;

    core_out_init(&o, line, (uint32_t)sizeof line);
    core_out_str(&o, tag);
    core_out_str(&o, ok ? " ok\n" : " FAIL\n");
    fputs(line, stdout);
    fflush(stdout);
    if (!ok)
        g_fail++;
}

static void info(const char *tag, const uint32_t *v, uint32_t n)
{
    char     line[200];
    core_out o;
    uint32_t i;

    core_out_init(&o, line, (uint32_t)sizeof line);
    core_out_str(&o, tag);
    for (i = 0u; i < n; i++) {
        core_out_ch(&o, ' ');
        core_out_u32(&o, v[i]);
    }
    core_out_str(&o, "\n");
    fputs(line, stdout);
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

/* 表面表照素材包的几何（行跨按 2 的幂对齐） */
static int setup(stg_draw_cfg *c)
{
    int rc = 0;

    gfx_surface_reset(&s_tab);
    rc |= surf(SLOT_FB0,       0x02000000u, 640u, 480u, 12u, GFX_FMT_XRGB8888);
    rc |= surf(SLOT_BUL16,     0x01000000u,  64u,  16u,  8u, GFX_FMT_ARGB8888_PM);
    rc |= surf(SLOT_BUL8,      0x01010000u,  32u,   8u,  7u, GFX_FMT_ARGB8888_PM);
    rc |= surf(SLOT_RICE,      0x01020000u, 320u,  80u, 11u, GFX_FMT_ARGB8888_PM);
    rc |= surf(SLOT_FONT,      0x01050000u, 128u,  96u,  9u, GFX_FMT_XRGB8888);
    rc |= surf(SLOT_PSHOT,     0x01060000u,   6u,  24u,  5u, GFX_FMT_ARGB8888_PM);
    rc |= surf(SLOT_SPARK,     0x01070000u,  32u,  40u,  7u, GFX_FMT_ARGB8888_PM);
    rc |= surf(SLOT_SHIPS,     0x01080000u, 128u,  96u,  9u, GFX_FMT_XRGB8888);
    rc |= surf(SLOT_BOSS,      0x01100000u, 192u, 288u, 10u, GFX_FMT_ARGB8888_PM);
    rc |= surf(SLOT_STARS,     0x01200000u,   8u,   2u,  5u, GFX_FMT_XRGB8888);
    rc |= surf(SLOT_TITLE,     0x01210000u, 240u,  90u, 10u, GFX_FMT_ARGB8888_PM);
    rc |= surf(SLOT_DIGITS,    0x01230000u, 256u,  32u, 10u, GFX_FMT_XRGB8888);
    rc |= surf(SLOT_ICONS,     0x01240000u,  64u,  16u,  8u, GFX_FMT_XRGB8888);
    rc |= surf(SLOT_FONT_CYAN, 0x01250000u, 128u,  96u,  9u, GFX_FMT_XRGB8888);

    c->pool.dst                     = SLOT_FB0;
    c->pool.src_slot[STG_SRC_BUL16] = SLOT_BUL16;
    c->pool.src_slot[STG_SRC_BUL8]  = SLOT_BUL8;
    c->pool.src_slot[STG_SRC_RICE]  = SLOT_RICE;
    c->pool.src_slot[STG_SRC_PSHOT] = SLOT_PSHOT;
    c->pool.src_slot[STG_SRC_SPARK] = SLOT_SPARK;
    c->pool.player_slot             = SLOT_SHIPS;
    c->pool.player_w                = 32;
    c->pool.player_h                = 32;
    c->pool.player_key              = 0u;
    c->ships_slot                   = SLOT_SHIPS;
    c->boss_slot                    = SLOT_BOSS;
    c->stars_slot                   = SLOT_STARS;
    c->title_slot                   = SLOT_TITLE;
    c->digits_slot                  = SLOT_DIGITS;
    c->icons_slot                   = SLOT_ICONS;
    c->font.slot                    = SLOT_FONT;
    c->font.cell_w                  = 8u;
    c->font.cell_h                  = 16u;
    c->font.cols                    = 16u;
    c->font.first                   = 0x20u;
    c->font.cells                   = 96u;
    c->font.key                     = 0u;
    c->font_cyan                    = c->font;
    c->font_cyan.slot               = SLOT_FONT_CYAN;
    return rc;
}

/* --- V1 共用文字绘制与原写法逐字节相同 ---------------------------------------- */

static void v1_text(const stg_draw_cfg *c)
{
    char     s[128];
    gfx_list a, b;
    uint32_t i, n = 0u, bad;
    int      x = 400;

    for (i = 0u; i < 95u; i++)
        s[n++] = (char)(0x20u + i);
    s[n++] = (char)0x1F;
    s[n++] = (char)0x7F;
    s[n++] = (char)0xFF;
    s[n] = '\0';

    gfx_list_begin(&a, s_cmds, LIST_CAP, &s_tab);
    bad = stg_text_draw(&a, &c->font, SLOT_FB0, 400, 32, s, 0);

    /* 抽出之前 sw6_hud.c 的写法：逐字算格号，格数之内才画，x 每字加 8 */
    gfx_list_begin(&b, s_ref, LIST_CAP, &s_tab);
    for (i = 0u; s[i] != '\0'; i++) {
        const uint32_t gi = (uint32_t)(uint8_t)s[i] - 0x20u;

        if (gi < 96u)
            (void)gfx_blit(&b, SLOT_FONT, (int)((gi % 16u) * 8u), (int)((gi / 16u) * 16u),
                           SLOT_FB0, x, 32, 8, 16, GFX_EQ_COPY, 0u, 1, 0u);
        x += 8;
    }
    ck("V1 stg_text_draw == per-glyph gfx_blit (95 glyphs + 3 out of range): same records and counters",
       bad == 0u && a.used == b.used && a.used == 96u && a.rejected == b.rejected
       && memcmp(s_cmds, s_ref, (size_t)a.used * sizeof(gfx_cmd)) == 0);
}

/* --- V2 3000 帧组帧 ------------------------------------------------------------ */

static int compose_check(const stg_draw_cfg *c, stg_frame_stats *st, uint32_t *used)
{
    static const stg_hud_live live = { 0u, 0, 0u, 0 };
    gfx_list         l;
    gfx_census_state cs;
    gfx_census       cc;
    const stg_active *a = &st->act;
    uint32_t         e, k;
    int              ok;

    gfx_list_begin(&l, s_cmds, LIST_CAP, &s_tab);
    ok = (stg_game_compose(&s_g, &l, c, &live, st) == 0);
    ok &= (l.rejected == 0u && l.overflow == 0u && st->rejected == 0u && l.ended);
    ok &= ((s_cmds[0].w[0] & 0xFFu) == GFX_OP_SET_CLIP);
    for (k = 1u; k <= 29u; k++)
        ok &= ((s_cmds[k].w[0] & 0xFFu) == GFX_OP_FILL);
    gfx_census_reset(&cs);
    gfx_census_clear(&cc);
    ok &= (gfx_census_run(&cs, &s_tab, s_cmds, l.used, &cc) == 0);
    for (e = 0u; e <= GFX_EQ_MAX; e++)
        ok &= (cc.cmd_by_eq[e] == st->by_eq[e]);
    ok &= (st->by_eq[GFX_EQ_ADD] == a->pbullet + a->particle);
    ok &= (st->by_eq[GFX_EQ_ALPHA] == a->ebullet + st->boss + st->title);
    ok &= (st->by_eq[GFX_EQ_CONST] == st->flash);
    ok &= (a->total == a->ebullet + a->pbullet + a->enemy + a->particle + a->player);
    *used = l.used;
    return ok;
}

static void v2_frames(const stg_draw_cfg *c)
{
    stg_script      sc;
    stg_frame_stats st;
    uint32_t        f, used, bad = 0u, max_used = 0u, max_act = 0u, title_ok = 1u, v[3];

    stg_game_init(&s_g, STG_GAME_SEED);
    stg_game_set_hits(&s_g, 0);
    stg_script_init(&sc);
    for (f = 0u; f < 3000u; f++) {
        stg_game_step(&s_g, stg_script_keys(&sc, f));
        if (!compose_check(c, &st, &used))
            bad++;
        if (s_g.phase == STG_PH_TITLE && (st.title != 2u || st.act.total != 0u))
            title_ok = 0u;
        if (used > max_used)
            max_used = used;
        if (st.act.total > max_act)
            max_act = st.act.total;
    }
    ck("V2 3000 frames: no rejects, SET_CLIP + 29 FILL first, census by eq == compose, class identities", bad == 0u);
    ck("V3 title frames: 2 title images (field + HUD), 0 active sprites", title_ok != 0u);
    v[0] = f;
    v[1] = max_used;
    v[2] = max_act;
    info("V2 frames max_commands max_active", v, 3u);

    s_g.flash = 1u;
    ck("V3 flash frame: exactly one CONST FILL", compose_check(c, &st, &used) && st.flash == 1u
       && st.by_eq[GFX_EQ_CONST] == 1u);
}

uint32_t test_stg_view_run(void)
{
    stg_draw_cfg c;

    g_fail = 0u;
    ck("V0 surface table registers", setup(&c) == 0);
    v1_text(&c);
    v2_frames(&c);
    return g_fail;
}
