/******************************************************************************
* @file stg_main.c: 弹幕场的画面入口（SW7 2.6 节「入口」）
*
* @brief  只在 -DSTG_FIELD=<n> 时有内容；默认构建与 SW6 的两种构建里这个文件是空的。
*
*         一帧：场推进一帧（发射、碰撞，SW7 2.3 节的 ①②③）-> 组帧 -> 普查自查 ->
*         软件执行器 -> 游戏区 CRC -> 呈现。双缓冲按全局帧序号 s_seq 的奇偶选面，
*         不按段内帧号（SW6 4.23 节的教训）。
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
*         N_max 搜索（STG_FIELD=3，SW8 2.3 节）：无窗口、不执行命令表、不呈现，每帧只做场推进、组帧、
*         普查与模拟时钟（stg_press），把 e、活动元素数与帧代价喂给 lib/core/nmax；16x16 与 8x8 两种负载
*         各搜一遍，打 nmax cfg、逐档 nmax probe 与 nmax result。结果不是 OK 记一次 fail。
*         板上跑它也只是模拟时钟下验证流程；板上真实周期的驱动在 SW9。
******************************************************************************/

#include "stg_main.h"

#if STG_FIELD_MODE != 0

#include <stddef.h>

#include "board.h"

#include "core/crc32.h"
#include "core/format.h"
#include "gfx/census.h"

#include "assets.h"

#include "core/nmax.h"

#include "game/stg_field.h"
#include "game/stg_pattern.h"
#include "game/stg_press.h"

#include "stg_slots.h"

#if STG_FIELD_MODE < 1 || STG_FIELD_MODE > 3
#error "STG_FIELD 只认 1（演示）、2（判据）与 3（N_max 搜索）"
#endif

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

static stg_field s_field;           /* 场的全部状态，约 380 KB */
static uint32_t  s_fail;

/* 画面模式（1、2）才用的：执行、呈现与游戏区 CRC。N_max 搜索（3）不执行也不呈现。 */
#define STG_DRAWS   (STG_FIELD_MODE != 3)

#if STG_DRAWS
static uint32_t  s_seq;             /* 全局帧序号：选命令表与帧缓冲 */
static int       s_shown = -1;      /* 正在显示的那一面 */

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

/* 游戏的表面槽（stg_slots.h）。自机取 ships.png 的第 0 格（32x32，左上角 (0, 0)，色键 0）。 */
static void cfg_init(stg_compose_cfg *c, int dst)
{
    c->dst                     = dst;
    c->src_slot[STG_SRC_BUL16] = STG_SLOT_BUL16;
    c->src_slot[STG_SRC_BUL8]  = STG_SLOT_BUL8;
    c->src_slot[STG_SRC_RICE]  = STG_SLOT_RICE;
    c->src_slot[STG_SRC_PSHOT] = STG_SLOT_PSHOT;
    c->src_slot[STG_SRC_SPARK] = STG_SLOT_SPARK;
    c->player_slot             = STG_SLOT_SHIPS;
    c->player_w                = (int32_t)ASSET_SHIPS_CELL_W;
    c->player_h                = (int32_t)ASSET_SHIPS_CELL_H;
    c->player_key              = ASSET_SHIPS_KEY;
}

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

    board_present(dst);
    s_shown = dst;
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
    char     buf[48];
    core_out o;

    core_out_init(&o, buf, (uint32_t)sizeof buf);
    core_out_str(&o, "fail ");
    core_out_u32(&o, s_fail);
    core_out_str(&o, "\r\ndone\r\n");
    board_puts(buf);
}

#if STG_FIELD_MODE == 3

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
#if STG_DRAWS
    s_seq   = 0u;
    s_shown = -1;
#endif
    exec_sw_reset(ctx->es);
    return run(ctx);
}

#endif /* STG_FIELD_MODE != 0 */
