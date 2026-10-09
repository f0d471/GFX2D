/******************************************************************************
* @file stg_main.c: 弹幕场的画面入口（sw 第 06 章 3.9、3.13 节）
*
* @brief  只在 -DSTG_FIELD=<n> 时有内容；默认构建与 SW6 的两种构建里这个文件是空的。
*
*         一帧：场推进一帧（发射、碰撞，sw 第 06 章 3.7 节一帧顺序的 ①②③）-> 组帧 -> 普查自查 ->
*         软件执行器 -> 游戏区 CRC -> 呈现。双缓冲按全局帧序号 s_seq 的奇偶选面，
*         不按段内帧号（sw 第 05 章 4.9 节的教训）。
*
*         判据模式（STG_FIELD=2）每帧自查三件，不符就记一次 fail 并打一行 err：
*           - 追加的拒绝数与表满数为 0（gfx_blit_run 与其余追加）；
*           - 普查的 ALPHA 条数等于发出的敌弹条数；
*           - 执行器返回 GFX_OK，且不往正在显示的那一面上画。
*         逐帧一行：
*           f <帧号> <段号> pool <敌弹> <自机弹> <敌机> <粒子> act <活动元素> hit <本段累计命中> graze <本段累计擦弹> crc <游戏区 CRC32>
*         帧号从 0 起全程连续；每段开头重新装场，命中与擦弹从 0 数起。
*         游戏区 CRC 只取左边 400x480：右边 HUD 的区域 B 批不画，板上那片 DDR 的初值不确定。
*
*         游戏的判据入口（STG_FIELD=4，sw 第 06 章 3.13 节）：无窗口、不读实时按键，输入是固定的按键脚本
*         （stg_script），受击默认关掉（-DSTG_JUDGE_HITS=1 打开），跑完一局（回到标题）或到 -DSTG_FRAMES=N 为止。
*         -DSTG_JUDGE_AUTO=1 时输入换成自动驾驶（stg_auto，从第 0 帧起）、受击打开（sw 第 07 章 3.10 节），
*         末尾多一行 auto frames <帧数> cyc_avg <每帧平均周期数> cyc_max <最大周期数>（PC 上只作登记）。
*         -DSTG_JUDGE_REPLAY=1 时输入换成入库的通关录像 replay/stg_clear.g2dr（工具辅助，
*         Main/tools/check/gen_stg_clear.c 搜出来的，sw 第 06 章 3.14 节）、受击打开，第 k 帧用录像的第 k 个按键位图；
*         进入 CLEAR 那一帧之后多一行
*           clear <帧号> score <得分> graze <擦弹> lives <残机> bombs <炸弹>
*         末尾多一行
*           end frames <总帧数> hits <中弹次数> over <0|1>
*         与 gen_stg_clear --verify 的摘要同格式。录像打不开、种子不是游戏的、没走到 CLEAR、残机用完、
*         回到标题时录像的帧没用完或不够用，都记 fail。注错 INJECT_REPLAY_KEY 把第 200~203 帧的按键换成别的方向，
*         逐帧日志必须从第 200 帧起与正常回放不同。
*         组帧是游戏的整帧（stg_game_compose，含 HUD），执行后取整帧 640x480 的 CRC。每段开始打一行
*           seg <帧号> <段名>
*         逐帧一行：
*           g <帧号> <阶段> <段号> pool <敌弹> <自机弹> <粒子> act <敌弹> <自机弹> <敌机> <粒子> <自机>
*             life <残机> bomb <炸弹> score <得分> graze <擦弹> crc <整帧 CRC32>
*         每帧自查（不符记 fail、打 err）：追加的拒绝数与表满数为 0；普查从命令表另数的各方程条数等于组帧记下的，
*         且 ADD = 自机弹 + 粒子、ALPHA = 敌弹 + Boss + 标题图、CONST = 闪白。HUD 的实时量一律 --；
*         注错 INJECT_STG_LIVE_HUD 让它显示实测的 SW FPS，两遍的日志必须因此不同。
*         PC 上跑它时设 GFX2D_UNPACED=1（bsp/host），呈现不等场。
*
*         N_max 搜索（STG_FIELD=3，sw 第 07 章 3.6 节）：无窗口、不执行命令表、不呈现，每帧只做场推进、组帧、
*         普查与模拟时钟（stg_press），把 e、活动元素数与帧代价喂给 lib/core/nmax；16x16 与 8x8 两种负载
*         各搜一遍，打 nmax cfg、逐档 nmax probe 与 nmax result。结果不是 OK 记一次 fail。
*         板上跑它也只是模拟时钟下验证流程；板上真实周期的驱动在 SW9。
******************************************************************************/

#include "stg_main.h"
#include "renderer.h"

#if STG_FIELD_MODE != 0

#include <stddef.h>

#include "board.h"

#include "core/crc32.h"
#include "core/format.h"
#include "gfx/census.h"

#include "assets.h"

#include "core/nmax.h"

#include "game/stg_draw.h"
#include "game/stg_field.h"
#include "game/stg_game.h"
#include "game/stg_pattern.h"
#include "game/stg_press.h"
#include "game/stg_auto.h"
#include "game/stg_script.h"

#include "stg_modes.h"
#include "stg_slots.h"

#if STG_FIELD_MODE == 4 && defined(STG_JUDGE_REPLAY) && STG_JUDGE_REPLAY
#include "core/replay.h"
#include "stg_clear.h"      /* 生成物：$(OBJDIR)/replay/stg_clear.h（replay/stg_clear.g2dr） */
#endif

#if STG_FIELD_MODE < 1 || STG_FIELD_MODE > 4
#error "STG_FIELD 只认 1（演示）、2（判据）、3（N_max 搜索）与 4（游戏的判据入口）"
#endif

/* 游戏的按键位与 board.h 同值（sw 第 06 章 3.10 节）：src/game/ 不含 board.h，在入口这里卡住 */
_Static_assert(STG_KEY_UP == BOARD_KEY_UP && STG_KEY_DOWN == BOARD_KEY_DOWN
               && STG_KEY_LEFT == BOARD_KEY_LEFT && STG_KEY_RIGHT == BOARD_KEY_RIGHT
               && STG_KEY_BOMB == BOARD_KEY_BOMB && STG_KEY_SLOW == BOARD_KEY_SLOW,
               "游戏的按键位 STG_KEY_* 必须与 board.h 的 BOARD_KEY_* 同值");

#define STG_DEMO_SEG_FRAMES     300u
#define STG_DEMO_MIX_FRAMES     600u
#define STG_JUDGE_SEG_FRAMES    120u
#define STG_JUDGE_MIX_FRAMES    240u

/* 与 SW6_FRAMES_MAX 同一个理由不带 (uint32_t)：#if 里要求值 */
#ifdef STG_FRAMES
#define STG_FRAMES_MAX STG_FRAMES
#else
#define STG_FRAMES_MAX 0
#endif

/* --- 状态（都是静态的，不用 malloc）--------------------------------------- */

static uint32_t  s_fail;

/* 弹幕场（1、2、3）才用的：场的状态。游戏的判据入口（4）用游戏的状态结构体。 */
#define STG_USES_FIELD  (STG_FIELD_MODE != 4)
/* 画面模式（1、2、4）才用的：执行与呈现。N_max 搜索（3）不执行也不呈现。 */
#define STG_PRESENTS    (STG_FIELD_MODE != 3)
/* 弹幕场的画面模式（1、2）才用的：一段一段装场、游戏区 CRC。 */
#define STG_DRAWS       (STG_FIELD_MODE == 1 || STG_FIELD_MODE == 2)

#if STG_USES_FIELD
static stg_field s_field;           /* 场的全部状态，约 380 KB */
#endif

#if STG_PRESENTS
static uint32_t  s_seq;             /* 全局帧序号：选命令表与帧缓冲 */
static int       s_shown = -1;      /* 正在显示的那一面 */
#if defined(G1_TEX_COMPARE) && G1_TEX_COMPARE
static uint32_t s_compare_tex_commands;
#endif
#endif

#if STG_DRAWS
typedef struct {
    stg_active act;
    uint32_t   crc;
    uint32_t   cyc_step, cyc_bld, cyc_exe;
} frame_out;
#endif

static void say_err(const char *what, uint32_t frame, uint32_t v)
{
    char     buf[96];
    core_out o;

    core_out_init(&o, buf, (uint32_t)sizeof buf);
    core_out_str(&o, "err ");
    core_out_str(&o, what);
    core_out_str(&o, " frame ");
    core_out_u32(&o, frame);
    core_out_str(&o, " v ");
    core_out_u32(&o, v);
    core_out_str(&o, "\r\n");
    board_puts(buf);
    s_fail++;
}

#if STG_DRAWS

static void say_seg(uint32_t seg)
{
    char     buf[48];
    core_out o;

    core_out_init(&o, buf, (uint32_t)sizeof buf);
    core_out_str(&o, "seg ");
    core_out_u32(&o, seg);
    core_out_ch(&o, ' ');
    core_out_str(&o, stg_seg_name(seg));
    core_out_str(&o, "\r\n");
    board_puts(buf);
}

/* 游戏区 400x480 的 CRC32，逐行取左边 1600 字节。 */
static uint32_t field_crc(const uint32_t *fb)
{
    const uint8_t *p0 = (const uint8_t *)fb;
    crc32_ctx      c;
    uint32_t       y;

    crc32_begin(&c);
    for (y = 0u; y < (uint32_t)STG_FIELD_H; y++)
        crc32_update(&c, p0 + (size_t)y * FB_STRIDE_BYTES, (uint32_t)STG_FIELD_W * 4u);
    return crc32_end(&c);
}

#endif /* STG_DRAWS */

#if STG_USES_FIELD
/* 游戏的表面槽（stg_slots.h，与模式框架共用 stg_modes_draw_cfg）。自机取 ships.png 的第 0 格。 */
static void cfg_init(stg_compose_cfg *c, int dst)
{
    stg_draw_cfg d;

    stg_modes_draw_cfg(&d, dst);
    *c = d.pool;
}
#endif

#if STG_DRAWS

static void frame_run(const stg_main_ctx *ctx, uint32_t frame, frame_out *fo)
{
    const uint32_t   idx = s_seq & 1u;
    const int        dst = idx ? STG_SLOT_FB1 : STG_SLOT_FB0;
    gfx_list         l;
    stg_compose_cfg  cfg;
    gfx_census_state cs;
    gfx_census       cc;
    exec_sw_stats    st;
    uint32_t         t0;
    int              rc;

    if (dst == s_shown)
        say_err("fb", frame, (uint32_t)dst);

    t0 = board_cycles();
    stg_field_step(&s_field);
    fo->cyc_step = board_cycles() - t0;

    t0 = board_cycles();
    gfx_list_begin(&l, ctx->cmds[idx], ctx->cap[idx], ctx->tab);
    cfg_init(&cfg, dst);
    if (stg_field_compose(&s_field, &l, &cfg, &fo->act) != 0)
        say_err("compose", frame, 0u);
    fo->cyc_bld = board_cycles() - t0;
    if (l.rejected != 0u || l.overflow != 0u)
        say_err("list rejected/overflow", frame, l.rejected + l.overflow);

    gfx_census_reset(&cs);
    gfx_census_clear(&cc);
    if (gfx_census_run(&cs, ctx->tab, l.buf, l.used, &cc) != 0
        || cc.cmd_by_eq[GFX_EQ_ALPHA] != fo->act.ebullet)
        say_err("census alpha", frame, cc.cmd_by_eq[GFX_EQ_ALPHA]);

    exec_sw_stats_clear(&st);
    t0 = board_cycles();
    rc = exec_sw_run(ctx->es, ctx->tab, ctx->mem, l.buf, l.used, l.used, &st, ctx->err);
    fo->cyc_exe = board_cycles() - t0;
    if (rc != GFX_OK)
        say_err("exec", frame, (uint32_t)rc);

    fo->crc = field_crc(board_fb(dst));

    {
        uint32_t ticket;
        if (board_present_request(dst, &ticket) != 0 || board_present_wait(ticket) != 0)
            say_err("present", frame, (uint32_t)dst);
        else
            s_shown = dst;
    }
    s_seq++;
}

static uint32_t seg_frames(uint32_t seg, int judge)
{
    if (judge)
        return (seg == STG_SEG_MIX) ? STG_JUDGE_MIX_FRAMES : STG_JUDGE_SEG_FRAMES;
    return (seg == STG_SEG_MIX) ? STG_DEMO_MIX_FRAMES : STG_DEMO_SEG_FRAMES;
}

static void seg_begin(uint32_t seg)
{
    if (stg_pattern_load(&s_field, seg) != 0)
        say_err("segment load", 0u, seg);
    say_seg(seg);
}

#endif /* STG_DRAWS */

static void say_end(void)
{
#if defined(G1_TEX_COMPARE) && G1_TEX_COMPARE
    {
        char text[64]; core_out out;
        core_out_init(&out,text,sizeof text);
        core_out_str(&out,"compare tex commands ");
        core_out_u32(&out,s_compare_tex_commands);
        core_out_str(&out,"\r\n"); board_puts(text);
        if (s_compare_tex_commands==0u) s_fail++;
    }
#endif
    char     buf[48];
    core_out o;

    core_out_init(&o, buf, (uint32_t)sizeof buf);
    core_out_str(&o, "fail ");
    core_out_u32(&o, s_fail);
    core_out_str(&o, "\r\ndone\r\n");
    board_puts(buf);
}

#if STG_FIELD_MODE == 4

/* --- 游戏的判据入口（sw 第 06 章 3.13 节）--------------------------- */

#ifndef STG_JUDGE_AUTO
#define STG_JUDGE_AUTO  0           /* 1：输入换成自动驾驶（sw 第 07 章 3.10 节） */
#endif

#ifndef STG_JUDGE_REPLAY
#define STG_JUDGE_REPLAY 0          /* 1：输入换成入库的通关录像（sw 第 06 章 3.14 节） */
#endif

#if STG_JUDGE_AUTO && STG_JUDGE_REPLAY
#error "STG_JUDGE_AUTO 与 STG_JUDGE_REPLAY 只能选一个"
#endif

#ifndef STG_JUDGE_HITS
/* 受击默认关掉（脚本不躲弹，打开时一局很快结束）；自动驾驶与录像回放时打开 */
#define STG_JUDGE_HITS  (STG_JUDGE_AUTO || STG_JUDGE_REPLAY)
#endif

#define STG_GAME_FRAME_LIMIT 20000u /* 保险：一局最多 10184 帧（2.5 节） */

static stg_game   s_game;           /* 游戏的全部状态，约 390 KB */
#if STG_JUDGE_AUTO
static uint32_t   s_auto_sum, s_auto_max;   /* 自动驾驶的开销：累计与最大（周期） */
#elif STG_JUDGE_REPLAY
static replay_view s_rv;            /* 通关录像 */
static uint32_t    s_rv_short;      /* 录像的帧不够用了 */
#else
static stg_script s_script;
#endif

typedef struct {
    stg_frame_stats st;
    uint32_t        crc;
} game_out;

/* HUD 的实时量。判据入口一律显示 --；注错 STG_LIVE_HUD 显示实测的 SW FPS（最近一秒的呈现次数）。 */
static void live_hud(stg_hud_live *h)
{
#ifdef INJECT_STG_LIVE_HUD
    static uint32_t t0, n, fps;
    static int      valid, started;
    const uint32_t  now = board_cycles();

    if (!started) {
        t0      = now;
        started = 1;
    }
    n++;
    if (now - t0 >= 100000000u) {
        fps   = n;
        n     = 0u;
        t0    = now;
        valid = 1;
    }
    h->sw_fps       = fps;
    h->sw_fps_valid = valid;
#else
    h->sw_fps       = 0u;
    h->sw_fps_valid = 0;
#endif
    h->hw_fps       = 0u;
    h->hw_fps_valid = 0;
    h->seg_text     = 0;
}

static void game_frame(const stg_main_ctx *ctx, uint32_t frame, uint32_t keys, game_out *go)
{
    const uint32_t   idx = s_seq & 1u;
    const int        dst = idx ? STG_SLOT_FB1 : STG_SLOT_FB0;
    gfx_list         l;
    stg_draw_cfg     cfg;
    stg_hud_live     live;
    gfx_census       cc;
    exec_sw_stats    xs;
    const stg_active *a = &go->st.act;
    uint32_t         e;
    int              rc;

    if (dst == s_shown)
        say_err("fb", frame, (uint32_t)dst);

    stg_game_step(&s_game, keys);

    rc = renderer_list_begin(
#if (defined(G4_STRIP_COMPARE) && G4_STRIP_COMPARE) || (defined(G4_STRIP_RENDER) && G4_STRIP_RENDER)
                             3,
#else
                             ctx->hw,
#endif
                             &l, ctx->cmds[idx], ctx->cap[idx], ctx->tab, dst);
    if (rc != GFX_OK) {
        say_err("list begin", frame, (uint32_t)rc);
        return;
    }
    stg_modes_draw_cfg(&cfg, dst);
    live_hud(&live);
    if (stg_game_compose(&s_game, &l, &cfg, &live, &go->st) != 0)
        say_err("compose", frame, 0u);
    if (l.rejected != 0u || l.overflow != 0u || go->st.rejected != 0u)
        say_err("list rejected/overflow", frame, l.rejected + l.overflow + go->st.rejected);
#if defined(G1_TEX_COMPARE) && G1_TEX_COMPARE
    for (e=0u;e<l.used;e++)
        if (gfx_op(&l.buf[e])==GFX_OP_BLIT &&
            gfx_surf_space(&ctx->tab->s[gfx_src(&l.buf[e])])==GFX_SPACE_TEX)
            s_compare_tex_commands++;
#endif

    /* 普查从命令表另数一遍：每个方程的条数等于组帧记下的，且按类对得上 */
    if (renderer_list_census(ctx->tab, &l, &cc) != 0)
        say_err("census", frame, 0u);
    for (e = 0u; e <= GFX_EQ_MAX; e++)
        if (l.strip_table == NULL && cc.cmd_by_eq[e] != go->st.by_eq[e])
            say_err("census eq", frame, e);
    if (go->st.by_eq[GFX_EQ_ADD] != a->pbullet + a->particle)
        say_err("class add", frame, go->st.by_eq[GFX_EQ_ADD]);
    if (go->st.by_eq[GFX_EQ_ALPHA] != a->ebullet + go->st.boss + go->st.title)
        say_err("class alpha", frame, go->st.by_eq[GFX_EQ_ALPHA]);
    if (go->st.by_eq[GFX_EQ_CONST] != go->st.flash)
        say_err("class const", frame, go->st.by_eq[GFX_EQ_CONST]);

    {
        uint32_t cyc;
        rc = renderer_run(ctx->hw, ctx->tab, ctx->mem, ctx->es, ctx->err,
                          &l, &xs, &cyc);
    }
    if (rc != GFX_OK)
        say_err("exec", frame, (uint32_t)rc);
    if (ctx->hw)
        board_dcache_invalidate_all();
    go->crc = stg_modes_frame_crc(board_fb(dst));

#if defined(SW9_COMPARE) && SW9_COMPARE
    {
        const int shadow = dst == STG_SLOT_FB0 ? STG_SLOT_FB1 : STG_SLOT_FB0;
        uint32_t hw_crc;
        char buf[96];
        core_out out;
        if (renderer_compare_shadow(dst, shadow, ctx->tab, ctx->mem, ctx->es,
                                    ctx->err, &l, &xs) != 0)
            say_err("compare engine", frame, 0u);
        hw_crc = stg_modes_frame_crc(board_fb(shadow));
        core_out_init(&out, buf, (uint32_t)sizeof buf);
        core_out_str(&out, "compare ");
        core_out_u32(&out, frame);
        core_out_str(&out, " sw ");
        core_out_hex32(&out, go->crc);
        core_out_str(&out, " hw ");
        core_out_hex32(&out, hw_crc);
        core_out_str(&out, "\r\n");
        board_puts(buf);
#ifdef INJECT_SW9_SKIP_INVALIDATE
        if (hw_crc == go->crc)
            say_err("compare not red", frame, hw_crc);
#else
        if (hw_crc != go->crc)
            say_err("compare mismatch", frame, hw_crc);
#endif
    }
#else
    {
        uint32_t ticket;
        if (board_present_request(dst, &ticket) != 0 || board_present_wait(ticket) != 0)
            say_err("present", frame, (uint32_t)dst);
        else
            s_shown = dst;
    }
#endif
    s_seq++;
}

static void say_game_seg(uint32_t frame, uint32_t seg)
{
    char     buf[64];
    core_out o;

    core_out_init(&o, buf, (uint32_t)sizeof buf);
    core_out_str(&o, "seg ");
    core_out_u32(&o, frame);
    core_out_ch(&o, ' ');
    core_out_str(&o, stg_game_seg_name(seg));
    core_out_str(&o, "\r\n");
    board_puts(buf);
}

static void say_game(uint32_t frame, const game_out *go)
{
    const stg_game   *g = &s_game;
    const stg_active *a = &go->st.act;
    char              buf[240];
    core_out          o;

    core_out_init(&o, buf, (uint32_t)sizeof buf);
    core_out_str(&o, "g ");
    core_out_u32(&o, frame);
    core_out_ch(&o, ' ');
    core_out_u32(&o, g->phase);
    core_out_ch(&o, ' ');
    core_out_u32(&o, g->seg);
    core_out_str(&o, " pool ");
    core_out_u32(&o, stg_pool_count(&g->f.pools.ebullet));
    core_out_ch(&o, ' ');
    core_out_u32(&o, stg_pool_count(&g->f.pools.pbullet));
    core_out_ch(&o, ' ');
    core_out_u32(&o, stg_pool_count(&g->f.pools.particle));
    core_out_str(&o, " act ");
    core_out_u32(&o, a->ebullet);
    core_out_ch(&o, ' ');
    core_out_u32(&o, a->pbullet);
    core_out_ch(&o, ' ');
    core_out_u32(&o, a->enemy);
    core_out_ch(&o, ' ');
    core_out_u32(&o, a->particle);
    core_out_ch(&o, ' ');
    core_out_u32(&o, a->player);
    core_out_str(&o, " life ");
    core_out_u32(&o, g->lives);
    core_out_str(&o, " bomb ");
    core_out_u32(&o, g->bombs);
    core_out_str(&o, " score ");
    core_out_u32(&o, g->score);
    core_out_str(&o, " graze ");
    core_out_u32(&o, g->graze);
    core_out_str(&o, " crc ");
    core_out_hex32(&o, go->crc);
    core_out_str(&o, "\r\n");
    board_puts(buf);
}

/* 这一帧的输入：按键脚本，或自动驾驶（开销另计），或通关录像。 */
static uint32_t judge_keys(uint32_t frame)
{
#if STG_JUDGE_AUTO
    const uint32_t t0 = board_cycles();
    const uint32_t k  = stg_auto_keys(&s_game, NULL);
    const uint32_t c  = board_cycles() - t0;

    (void)frame;
    s_auto_sum = (s_auto_sum + c < s_auto_sum) ? 0xFFFFFFFFu : s_auto_sum + c;
    if (c > s_auto_max)
        s_auto_max = c;
    return k;
#elif STG_JUDGE_REPLAY
    uint32_t k = 0u;

    if (replay_key_at(&s_rv, frame, &k) != 0)
        s_rv_short = 1u;            /* 录像用完了还没回到标题：按键给 0，结束时记 fail */
#ifdef INJECT_REPLAY_KEY
    if (frame >= 200u && frame < 204u)      /* 注错：杂兵段里一块（4 帧）的按键换成别的方向 */
        k = (k == STG_KEY_LEFT) ? STG_KEY_RIGHT : STG_KEY_LEFT;
#endif
    return k;
#else
    return stg_script_keys(&s_script, frame);
#endif
}

#if STG_JUDGE_AUTO
static void say_auto(uint32_t frames)
{
    char     buf[96];
    core_out o;

    core_out_init(&o, buf, (uint32_t)sizeof buf);
    core_out_str(&o, "auto frames ");
    core_out_u32(&o, frames);
    core_out_str(&o, " cyc_avg ");
    core_out_u32(&o, (frames != 0u) ? s_auto_sum / frames : 0u);
    core_out_str(&o, " cyc_max ");
    core_out_u32(&o, s_auto_max);
    core_out_str(&o, "\r\n");
    board_puts(buf);
}
#endif

#if STG_JUDGE_REPLAY
/* 进入 CLEAR 那一帧之后的状态（与 gen_stg_clear 的摘要同格式）。 */
static void say_clear(uint32_t frame)
{
    char     buf[128];
    core_out o;

    core_out_init(&o, buf, (uint32_t)sizeof buf);
    core_out_str(&o, "clear ");
    core_out_u32(&o, frame);
    core_out_str(&o, " score ");
    core_out_u32(&o, s_game.score);
    core_out_str(&o, " graze ");
    core_out_u32(&o, s_game.graze);
    core_out_str(&o, " lives ");
    core_out_u32(&o, s_game.lives);
    core_out_str(&o, " bombs ");
    core_out_u32(&o, s_game.bombs);
    core_out_str(&o, "\r\n");
    board_puts(buf);
}

static void say_replay_end(uint32_t frames, uint32_t hits, uint32_t over)
{
    char     buf[80];
    core_out o;

    core_out_init(&o, buf, (uint32_t)sizeof buf);
    core_out_str(&o, "end frames ");
    core_out_u32(&o, frames);
    core_out_str(&o, " hits ");
    core_out_u32(&o, hits);
    core_out_str(&o, " over ");
    core_out_u32(&o, over);
    core_out_str(&o, "\r\n");
    board_puts(buf);
}
#endif

static int run(const stg_main_ctx *ctx)
{
    uint32_t frame;
    game_out go;
#if STG_JUDGE_REPLAY
    uint32_t hits = 0u, over = 0u, clear = 0u;

    if (replay_open(stg_clear_bytes, STG_CLEAR_LEN, &s_rv) != 0 || s_rv.seed != STG_GAME_SEED) {
        say_err("replay open", 0u, s_rv.seed);
        say_end();
        return 1;
    }
    s_rv_short = 0u;
#endif

    stg_game_init(&s_game, STG_GAME_SEED);
    stg_game_set_hits(&s_game, STG_JUDGE_HITS);
#if !STG_JUDGE_AUTO && !STG_JUDGE_REPLAY
    stg_script_init(&s_script);
#endif
    say_game_seg(0u, s_game.seg);
    for (frame = 0u; frame < STG_GAME_FRAME_LIMIT; frame++) {
#if defined(G4_SAMPLE_STEP) && G4_SAMPLE_STEP > 0
        const int sample = frame % G4_SAMPLE_STEP == 0u || frame + 1u == s_rv.frames;
        if (sample)
            game_frame(ctx, frame, judge_keys(frame), &go);
        else
            stg_game_step(&s_game, judge_keys(frame));
#else
        game_frame(ctx, frame, judge_keys(frame), &go);
#endif
        if ((s_game.ev & STG_EV_SEG) != 0u)
            say_game_seg(frame, s_game.seg);
#if defined(G4_SAMPLE_STEP) && G4_SAMPLE_STEP > 0
        if (sample)
#endif
            say_game(frame, &go);
#if STG_JUDGE_REPLAY
        if ((s_game.ev & STG_EV_HIT) != 0u)
            hits++;
        if ((s_game.ev & STG_EV_OVER) != 0u)
            over = 1u;
        if ((s_game.ev & STG_EV_SEG) != 0u && s_game.seg == STG_GSEG_CLEAR) {
            clear = 1u;
            say_clear(frame);
        }
#endif
        if ((s_game.ev & STG_EV_SEG) != 0u && s_game.seg == STG_GSEG_TITLE)
            break;                                  /* 一局跑完，回到标题 */
#if STG_FRAMES_MAX != 0
        if (frame + 1u >= (uint32_t)STG_FRAMES_MAX)
            break;
#endif
    }
#if STG_JUDGE_AUTO
    say_auto((frame < STG_GAME_FRAME_LIMIT) ? frame + 1u : frame);
#endif
#if STG_JUDGE_REPLAY
    {
        const int partial = (STG_FRAMES_MAX != 0 && frame + 1u == (uint32_t)STG_FRAMES_MAX
                             && frame + 1u < s_rv.frames);
    if (frame < STG_GAME_FRAME_LIMIT)
        frame++;                                    /* 总帧数：含回到标题的那一帧 */
    say_replay_end(frame, hits, over);
    if ((!clear && !partial) || over)
        say_err("replay no clear", frame, over);
    if (s_rv_short || (!partial && frame != s_rv.frames))
        say_err("replay frames", frame, s_rv.frames);
    }
#endif
    say_end();
    return (s_fail == 0u) ? 0 : 1;
}

#elif STG_FIELD_MODE == 3

/* --- N_max 搜索（模拟时钟）------------------------------------------------ */

#define STG_NMAX_FRAME_LIMIT 200000u    /* 保险：B 批的一遍约 7000~8000 帧 */

static nmax_state s_nmax;

static void say_line(const char *s)
{
    board_puts(s);
    board_puts("\r\n");
}

static void search_one(const stg_main_ctx *ctx, uint32_t kind, const char *name)
{
    nmax_cfg        cfg;
    stg_press_clock ck;
    stg_compose_cfg cc;
    stg_press_out   o;
    uint32_t        frames = 0u;
    char            buf[240];

    board_puts("nmax load ");
    board_puts(name);
    board_puts("\r\n");

    stg_press_clock_b(&ck);
    nmax_cfg_default(&cfg);
    if (nmax_init(&s_nmax, &cfg, ck.field_cyc) != 0) {
        say_err("nmax cfg", 0u, 0u);
        return;
    }
    (void)nmax_format_cfg(&s_nmax, buf, (uint32_t)sizeof buf);
    say_line(buf);

    stg_press_begin(&s_field, kind);
    stg_field_set_pressure(&s_field, s_nmax.knob);
    cfg_init(&cc, STG_SLOT_FB0);
    while (s_nmax.status == NMAX_RUNNING && frames < STG_NMAX_FRAME_LIMIT) {
        gfx_list l;
        uint32_t ev;

        gfx_list_begin(&l, ctx->cmds[0], ctx->cap[0], ctx->tab);
        stg_press_frame(&s_field, &l, &cc, &ck, &o);
        if (o.bad != 0u)
            say_err("press frame", frames, o.bad);
        ev = nmax_feed(&s_nmax, o.e, o.act.total, o.frame_cyc);
        frames++;
        if (ev & NMAX_EV_PROBE) {
            (void)nmax_format_probe(&s_nmax.last, buf, (uint32_t)sizeof buf);
            say_line(buf);
        }
        if (ev & NMAX_EV_KNOB)
            stg_field_set_pressure(&s_field, s_nmax.knob);
    }
    (void)nmax_format_result(&s_nmax, buf, (uint32_t)sizeof buf);
    say_line(buf);
    if (s_nmax.status != NMAX_OK)
        say_err("nmax status", frames, s_nmax.status);
}

static int run(const stg_main_ctx *ctx)
{
    search_one(ctx, STG_KIND_ROUND16, "16x16");
    search_one(ctx, STG_KIND_SMALL8, "8x8");
    say_end();
    return (s_fail == 0u) ? 0 : 1;
}

#elif STG_FIELD_MODE == 2

/* --- 判据模式 ------------------------------------------------------------ */

static void say_frame(uint32_t frame, uint32_t seg, const frame_out *fo)
{
    char     buf[160];
    core_out o;

    core_out_init(&o, buf, (uint32_t)sizeof buf);
    core_out_str(&o, "f ");
    core_out_u32(&o, frame);
    core_out_ch(&o, ' ');
    core_out_u32(&o, seg);
    core_out_str(&o, " pool ");
    core_out_u32(&o, stg_pool_count(&s_field.pools.ebullet));
    core_out_ch(&o, ' ');
    core_out_u32(&o, stg_pool_count(&s_field.pools.pbullet));
    core_out_ch(&o, ' ');
    core_out_u32(&o, stg_pool_count(&s_field.pools.enemy));
    core_out_ch(&o, ' ');
    core_out_u32(&o, stg_pool_count(&s_field.pools.particle));
    core_out_str(&o, " act ");
    core_out_u32(&o, fo->act.total);
    core_out_str(&o, " hit ");
    core_out_u32(&o, s_field.hits);
    core_out_str(&o, " graze ");
    core_out_u32(&o, s_field.grazes);
    core_out_str(&o, " crc ");
    core_out_hex32(&o, fo->crc);
    core_out_str(&o, "\r\n");
    board_puts(buf);
}

static int run(const stg_main_ctx *ctx)
{
    uint32_t  seg, k, frame = 0u;
    frame_out fo;

    for (seg = 0u; seg < STG_SEG_N; seg++) {
        seg_begin(seg);
        for (k = 0u; k < seg_frames(seg, 1); k++) {
#if STG_FRAMES_MAX != 0
            if (frame >= (uint32_t)STG_FRAMES_MAX)
                break;
#endif
            frame_run(ctx, frame, &fo);
            say_frame(frame, seg, &fo);
            frame++;
        }
    }
    say_end();
    return (s_fail == 0u) ? 0 : 1;
}

#else  /* STG_FIELD_MODE == 1 */

/* --- 演示模式 ------------------------------------------------------------ */

/* 每 60 帧一行：活动元素数与三段周期数（墙钟相关，不进判据）。 */
static void say_demo(uint32_t frame, const frame_out *fo)
{
    char     buf[120];
    core_out o;

    core_out_init(&o, buf, (uint32_t)sizeof buf);
    core_out_str(&o, "d ");
    core_out_u32(&o, frame);
    core_out_str(&o, " act ");
    core_out_u32(&o, fo->act.total);
    core_out_str(&o, " step ");
    core_out_u32(&o, fo->cyc_step);
    core_out_str(&o, " bld ");
    core_out_u32(&o, fo->cyc_bld);
    core_out_str(&o, " exe ");
    core_out_u32(&o, fo->cyc_exe);
    core_out_str(&o, "\r\n");
    board_puts(buf);
}

static int run(const stg_main_ctx *ctx)
{
    uint32_t  seg = 0u, k = 0u, frame = 0u;
    frame_out fo;

    seg_begin(seg);
    for (;;) {
        if (board_should_quit())
            break;
#if STG_FRAMES_MAX != 0
        if (frame >= (uint32_t)STG_FRAMES_MAX)
            break;
#endif
        if (k >= seg_frames(seg, 0)) {
            seg = (seg + 1u < STG_SEG_N) ? seg + 1u : 0u;
            k = 0u;
            seg_begin(seg);
        }
        frame_run(ctx, frame, &fo);
        if (frame % 60u == 0u)
            say_demo(frame, &fo);
        k++;
        frame++;
    }
    say_end();
    return (s_fail == 0u) ? 0 : 1;
}

#endif /* STG_FIELD_MODE */

int stg_main(const stg_main_ctx *ctx, uint32_t fail0)
{
    s_fail  = fail0;
#if defined(G1_TEX_COMPARE) && G1_TEX_COMPARE
    s_compare_tex_commands=0u;
#endif
#if STG_PRESENTS
    s_seq   = 0u;
    s_shown = -1;
#endif
    exec_sw_reset(ctx->es);
    return run(ctx);
}

#endif /* STG_FIELD_MODE != 0 */
