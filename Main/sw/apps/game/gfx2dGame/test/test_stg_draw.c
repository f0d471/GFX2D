/******************************************************************************
* @file test_stg_draw.c: 按池发出与组帧（sw 第 06 章 3.9 节，B 批）的主机侧测试
*
* @brief  四组，判据见 sw 第 06 章 3.9 节：
*           W1  剔除的边界：16x16 的弹，中心在四条边外 7 像素（还有 1 像素可见）发出、外 8 像素不发；
*               Q.8 的小数向负无穷取整
*           W2  随机池（16 个弹种号，含未用的 14、15；部分标记移除、位置越出游戏区）：分段扫（每段 7 条）与一次扫的结果逐条相同，
*               都等于独立重数——逐颗按 64 位算目的矩形、与游戏区求交，不用产品代码的无符号比较
*           W3  源左上角：RICE20 第 16、23、24、31 帧（品红那一行）；SMALL8 的颜色号；火花的帧号（列 = 寿命 & 3、行 = 寿命 >> 2）
*           W4  组帧：六段各跑 300 帧，每帧组一张表：拒绝与表满为 0；前三条是游戏区 SET_CLIP、FILL、自机 BLIT，
*               末两条是全屏 SET_CLIP 与 END；普查的 ALPHA 条数等于发出的敌弹数；
*               独立重数的活动元素数等于发出的条数；自机算一个活动元素
*
*         表面表用与 gfx2dGame 相同的槽号与尺寸（只登记描述符，不碰像素）。
******************************************************************************/

#include <stdint.h>
#include <stdio.h>

#include "core/format.h"
#include "core/xorshift32.h"
#include "gfx/census.h"
#include "gfx/gfx.h"
#include "game/stg_field.h"
#include "game/stg_kind.h"
#include "game/stg_pattern.h"
#include "game/stg_pool.h"
#include "test_stg_draw.h"

/* 与 gfx2dGame 游戏的表面槽（sw 第 06 章 3.12 节）相同的槽号（这里不含入口的头文件，app 测试不依赖入口的配置）。 */
#define SLOT_FB0    0
#define SLOT_BUL16  2
#define SLOT_BUL8   3
#define SLOT_RICE   4
#define SLOT_PSHOT  6
#define SLOT_SPARK  7
#define SLOT_SHIPS  8

#define LIST_CAP    8192u

static uint32_t g_fail;

static stg_ent            s_mem[4096];
static stg_field          s_field;
static gfx_cmd            s_cmds[LIST_CAP];
static gfx_blit_pos       s_a[4096], s_b[4096];
static gfx_surface_table  s_tab;

static void ck(const char *tag, int ok)
{
    char     line[220];
    core_out o;

    core_out_init(&o, line, (uint32_t)sizeof line);
    core_out_str(&o, tag);
    core_out_str(&o, ok ? " ok\n" : " FAIL\n");
    fputs(line, stdout);
    fflush(stdout);
    if (!ok)
        g_fail++;
}

static void geom_of(uint32_t kind, stg_draw_geom *g)
{
    g->w = (int32_t)stg_kind_tab[kind].w;
    g->h = (int32_t)stg_kind_tab[kind].h;
    g->cols_log2 = stg_kind_tab[kind].cols_log2;
}

/* 一次扫完（缓冲够大）。 */
static uint32_t draw_all(const stg_pool *p, uint32_t kind, gfx_blit_pos *out, uint32_t cap)
{
    stg_draw_geom g;
    gfx_rect      clip;
    uint32_t      cursor = 0u;

    geom_of(kind, &g);
    stg_rect_field(&clip);
    return stg_pool_draw(p, kind, &g, &clip, &cursor, out, cap);
}

/* --- 独立重数 ------------------------------------------------------------ */

/* 目的矩形 [dx, dx + w) x [dy, dy + h) 与游戏区 [0, 400) x [0, 480) 至少重叠一个像素。
   64 位、半开区间、按定义直接写，与产品代码的闭区间加无符号比较不是一段逻辑。 */
static int naive_visible(const stg_ent *e, int64_t w, int64_t h)
{
    const int64_t dx = (int64_t)(e->x >> 8) - w / 2;
    const int64_t dy = (int64_t)(e->y >> 8) - h / 2;

    return dx < STG_FIELD_W && dx + w > 0 && dy < STG_FIELD_H && dy + h > 0;
}

static uint32_t naive_count(const stg_pool *p, uint32_t kind)
{
    uint32_t i, n = 0u;

    for (i = 0u; i < stg_pool_count(p); i++) {
        stg_ent e;

        (void)stg_pool_get(p, i, &e);
        if ((e.flags & STG_F_REMOVE) != 0u || (e.flags & STG_F_KIND_MASK) != kind)
            continue;
        if (stg_kind_tab[kind].w == 0u)
            continue;
        n += (uint32_t)naive_visible(&e, stg_kind_tab[kind].w, stg_kind_tab[kind].h);
    }
    return n;
}

/* --- W1 剔除的边界 ------------------------------------------------------- */

static void w1_edges(void)
{
    /* ROUND16：dx = (x >> 8) - 8。中心 -7 像素时 dx = -15，最右一列落在 x = 0 上 */
    static const struct {
        int32_t x, y;
        int     vis;
    } c[] = {
        { -7 * FX_ONE,        240 * FX_ONE, 1 },
        { -8 * FX_ONE,        240 * FX_ONE, 0 },
        { -7 * FX_ONE - 1,    240 * FX_ONE, 0 },    /* -7.004 像素向负无穷取整成 -8 */
        { 407 * FX_ONE + 255, 240 * FX_ONE, 1 },    /* dx = 399 */
        { 408 * FX_ONE,       240 * FX_ONE, 0 },
        { 200 * FX_ONE,       -7 * FX_ONE,  1 },
        { 200 * FX_ONE,       -8 * FX_ONE,  0 },
        { 200 * FX_ONE,       487 * FX_ONE, 1 },    /* dy = 479 */
        { 200 * FX_ONE,       488 * FX_ONE, 0 },
        { -7 * FX_ONE,        -7 * FX_ONE,  1 },    /* 角上只剩 1 个像素 */
        { 407 * FX_ONE,       487 * FX_ONE, 1 },
        { 408 * FX_ONE,       488 * FX_ONE, 0 },
    };
    stg_pool p;
    uint32_t i, k, j = 0u;
    int      ok = 1;

    stg_pool_init(&p, s_mem, 4096u);
    for (i = 0u; i < sizeof c / sizeof c[0]; i++)
        (void)stg_pool_add(&p, c[i].x, c[i].y, 0, 0, 0u, STG_KIND_ROUND16);
    k = draw_all(&p, STG_KIND_ROUND16, s_a, 4096u);

    /* 发出的应当恰好是 vis 为 1 的那几颗，按池顺序 */
    for (i = 0u; i < sizeof c / sizeof c[0]; i++) {
        if (!c[i].vis)
            continue;
        if (j >= k || s_a[j].dx != (c[i].x >> 8) - 8 || s_a[j].dy != (c[i].y >> 8) - 8)
            ok = 0;
        j++;
    }
    ck("W1 16x16 edges: centre 7 px outside (1 px visible) drawn, 8 px outside culled, floor of Q.8",
       ok && k == j && j == 6u);
}

/* --- W2 分段扫、一次扫与独立重数 ----------------------------------------- */

static void w2_chunks(void)
{
    xs32     g;
    stg_pool p;
    uint32_t rd, bad_chunk = 0u, bad_naive = 0u, tot = 0u, tot_cull = 0u;

    xs32_seed(&g, 0x5E7D0001u);
    for (rd = 0u; rd < 50u; rd++) {
        uint32_t i, kind;

        stg_pool_init(&p, s_mem, 4096u);
        for (i = 0u; i < 3000u; i++) {
            const int32_t  x = (int32_t)(xs32_next(&g) % (440u * 256u)) - 20 * FX_ONE;
            const int32_t  y = (int32_t)(xs32_next(&g) % (520u * 256u)) - 20 * FX_ONE;
            const uint16_t kd = (uint16_t)(xs32_next(&g) % 16u);   /* 14、15 是未用弹种 */
            const uint16_t sp = (uint16_t)(xs32_next(&g) % 48u);

            (void)stg_pool_add(&p, x, y, 0, 0, sp, kd);
            if (xs32_next(&g) % 10u == 0u)
                stg_pool_mark(&p, i);
        }
        for (kind = 0u; kind < STG_KIND_N_MAX; kind++) {
            stg_draw_geom gm;
            gfx_rect      clip;
            uint32_t      one, many = 0u, cursor = 0u, j, n_kind = 0u;

            one = draw_all(&p, kind, s_a, 4096u);
            geom_of(kind, &gm);
            stg_rect_field(&clip);
            while (cursor < stg_pool_count(&p))
                many += stg_pool_draw(&p, kind, &gm, &clip, &cursor, s_b + many, 7u);
            if (one != many)
                bad_chunk++;
            for (j = 0u; j < one && j < many; j++) {
                if (s_a[j].dx != s_b[j].dx || s_a[j].dy != s_b[j].dy
                    || s_a[j].sx != s_b[j].sx || s_a[j].sy != s_b[j].sy) {
                    bad_chunk++;
                    break;
                }
            }
            if (one != naive_count(&p, kind))
                bad_naive++;
            tot += one;
            for (j = 0u; j < stg_pool_count(&p); j++) {
                stg_ent e;

                (void)stg_pool_get(&p, j, &e);
                if ((e.flags & 15u) == kind && (e.flags & STG_F_REMOVE) == 0u)
                    n_kind++;
            }
            tot_cull += n_kind - one;
        }
    }
    {
        char     line[120];
        core_out o;

        core_out_init(&o, line, (uint32_t)sizeof line);
        core_out_str(&o, "W2 totals 50 pools x 3000: drawn ");
        core_out_u32(&o, tot);
        core_out_str(&o, " culled or unused kind ");
        core_out_u32(&o, tot_cull);
        core_out_str(&o, "\n");
        fputs(line, stdout);
    }
    ck("W2 chunked scan (7 per call) == one scan, record by record", bad_chunk == 0u);
    ck("W2 drawn count == naive recount (64-bit rect vs field) for kinds 0..15", bad_naive == 0u);
}

/* --- W3 源左上角 --------------------------------------------------------- */

static void w3_source(void)
{
    stg_pool p;
    uint32_t k;

    stg_pool_init(&p, s_mem, 4096u);
    (void)stg_pool_add(&p, 100 * FX_ONE, 100 * FX_ONE, 0, 0, 16u, STG_KIND_RICE20 + 1u);
    (void)stg_pool_add(&p, 100 * FX_ONE, 100 * FX_ONE, 0, 0, 23u, STG_KIND_RICE20 + 1u);
    (void)stg_pool_add(&p, 100 * FX_ONE, 100 * FX_ONE, 0, 0, 24u, STG_KIND_RICE20 + 1u);
    (void)stg_pool_add(&p, 100 * FX_ONE, 100 * FX_ONE, 0, 0, 31u, STG_KIND_RICE20 + 1u);
    k = draw_all(&p, STG_KIND_RICE20 + 1u, s_a, 4096u);
    ck("W3 RICE20 frames 16, 23, 24, 31 -> source (0,20) (140,20) (160,20) (300,20), dest centre - 10",
       k == 4u && s_a[0].sx == 0 && s_a[0].sy == 20 && s_a[1].sx == 140 && s_a[1].sy == 20
       && s_a[2].sx == 160 && s_a[2].sy == 20 && s_a[3].sx == 300 && s_a[3].sy == 20
       && s_a[0].dx == 90 && s_a[0].dy == 90);

    stg_pool_init(&p, s_mem, 4096u);
    (void)stg_pool_add(&p, 100 * FX_ONE, 100 * FX_ONE, 0, 0, 2u, STG_KIND_SMALL8 + 2u);
    k = draw_all(&p, STG_KIND_SMALL8 + 2u, s_a, 4096u);
    ck("W3 SMALL8 amber (frame 2) source (16,0), dest centre - 4",
       k == 1u && s_a[0].sx == 16 && s_a[0].sy == 0 && s_a[0].dx == 96 && s_a[0].dy == 96);

    stg_pool_init(&p, s_mem, 4096u);
    (void)stg_pool_add(&p, 100 * FX_ONE, 100 * FX_ONE, 0, 0, 16u, STG_KIND_SPARK);
    (void)stg_pool_add(&p, 100 * FX_ONE, 100 * FX_ONE, 0, 0, 7u, STG_KIND_SPARK);
    (void)stg_pool_add(&p, 100 * FX_ONE, 100 * FX_ONE, 0, 0, 1u, STG_KIND_SPARK);
    k = draw_all(&p, STG_KIND_SPARK, s_a, 4096u);
    ck("W3 SPARK life 16, 7, 1 -> source (0,32) (24,8) (8,0)",
       k == 3u && s_a[0].sx == 0 && s_a[0].sy == 32 && s_a[1].sx == 24 && s_a[1].sy == 8
       && s_a[2].sx == 8 && s_a[2].sy == 0);
}

/* --- W4 组帧 ------------------------------------------------------------- */

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

static void w4_compose(void)
{
    stg_compose_cfg cfg;
    uint32_t        seg, k, frames = 0u;
    uint32_t        bad_list = 0u, bad_shape = 0u, bad_census = 0u, bad_naive = 0u, bad_act = 0u;
    uint32_t        max_act = 0u;
    int             rc = 0;

    gfx_surface_reset(&s_tab);
    rc |= surf(SLOT_FB0,    0x02000000u, 640u, 480u, 12u, GFX_FMT_XRGB8888);
    rc |= surf(SLOT_BUL16,  0x01000000u,  64u,  16u,  8u, GFX_FMT_ARGB8888_PM);
    rc |= surf(SLOT_BUL8,   0x01010000u,  32u,   8u,  7u, GFX_FMT_ARGB8888_PM);
    rc |= surf(SLOT_RICE,   0x01020000u, 320u,  80u, 11u, GFX_FMT_ARGB8888_PM);
    rc |= surf(SLOT_PSHOT,  0x01050000u,   6u,  24u,  5u, GFX_FMT_ARGB8888_PM);
    rc |= surf(SLOT_SPARK,  0x01060000u,  32u,  40u,  7u, GFX_FMT_ARGB8888_PM);
    rc |= surf(SLOT_SHIPS,  0x01100000u, 128u,  96u,  9u, GFX_FMT_XRGB8888);
    ck("W4 surface table (FB, bullet atlases, pshot, spark, ships) registers", rc == 0);

    cfg.dst                     = SLOT_FB0;
    cfg.src_slot[STG_SRC_BUL16] = SLOT_BUL16;
    cfg.src_slot[STG_SRC_BUL8]  = SLOT_BUL8;
    cfg.src_slot[STG_SRC_RICE]  = SLOT_RICE;
    cfg.src_slot[STG_SRC_PSHOT] = SLOT_PSHOT;
    cfg.src_slot[STG_SRC_SPARK] = SLOT_SPARK;
    cfg.player_slot             = SLOT_SHIPS;
    cfg.player_w                = 32;
    cfg.player_h                = 32;
    cfg.player_key              = 0x00000000u;

    for (seg = 0u; seg < STG_SEG_N; seg++) {
        (void)stg_pattern_load(&s_field, seg);
        for (k = 0u; k < 300u; k++) {
            gfx_list          l;
            stg_active        act;
            gfx_census_state  cs;
            gfx_census        cc;
            gfx_rect          fr;
            uint32_t          naive, kd;

            stg_field_step(&s_field);
            gfx_list_begin(&l, s_cmds, LIST_CAP, &s_tab);
            if (stg_field_compose(&s_field, &l, &cfg, &act) != 0)
                bad_list++;
            if (l.rejected != 0u || l.overflow != 0u || l.first_error != 0 || !l.ended)
                bad_list++;

            stg_rect_field(&fr);
            if (l.used < 5u
                || gfx_op(&s_cmds[0]) != GFX_OP_SET_CLIP
                || gfx_op(&s_cmds[1]) != GFX_OP_FILL
                || gfx_op(&s_cmds[2]) != GFX_OP_BLIT || gfx_eq(&s_cmds[2]) != GFX_EQ_COPY
                || gfx_op(&s_cmds[l.used - 2u]) != GFX_OP_SET_CLIP
                || gfx_op(&s_cmds[l.used - 1u]) != GFX_OP_END
                || l.used != act.total + 4u)
                bad_shape++;

            gfx_census_reset(&cs);
            gfx_census_clear(&cc);
            if (gfx_census_run(&cs, &s_tab, s_cmds, l.used, &cc) != 0
                || cc.cmd_by_eq[GFX_EQ_ALPHA] != act.ebullet)
                bad_census++;

            naive = 0u;
            for (kd = 0u; kd < STG_KIND_N_MAX; kd++)
                naive += naive_count(&s_field.pools.ebullet, kd);
            if (naive != act.ebullet)
                bad_naive++;
            if (act.player != 1u || act.total != act.ebullet + 1u)
                bad_act++;
            if (act.total > max_act)
                max_act = act.total;
            frames++;
        }
    }
    {
        char     line[120];
        core_out o;

        core_out_init(&o, line, (uint32_t)sizeof line);
        core_out_str(&o, "W4 totals 6 segments x 300 frames: frames ");
        core_out_u32(&o, frames);
        core_out_str(&o, " max active ");
        core_out_u32(&o, max_act);
        core_out_str(&o, "\n");
        fputs(line, stdout);
    }
    ck("W4 every frame: compose ok, rejected 0, overflow 0, first_error 0, ended", bad_list == 0u);
    ck("W4 every frame: SET_CLIP field, FILL, player BLIT COPY ... SET_CLIP screen, END; used == active + 4",
       bad_shape == 0u);
    ck("W4 every frame: census ALPHA commands == enemy bullets drawn", bad_census == 0u);
    ck("W4 every frame: naive recount of visible enemy bullets == drawn", bad_naive == 0u);
    ck("W4 every frame: player counted once, total == enemy bullets + 1", bad_act == 0u);
}

uint32_t test_stg_draw_run(void)
{
    g_fail = 0u;
    w1_edges();
    w2_chunks();
    w3_source();
    w4_compose();
    return g_fail;
}
