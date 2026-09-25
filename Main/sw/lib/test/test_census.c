/******************************************************************************
* @file test_census.c: 按方程的普查（SW8 2.4 节）的主机侧测试
*
* @brief  判据见 Engineering/Todo/sw/SW8-演示模式与统计埋点.md 第 3 节「普查（A 批）」一行。
*         对账的另一边是软件执行器：同一串提交上，普查的各方程面积之和必须等于
*         exec_sw_stats.pixel_total，各方程条数之和加 cmd_other 必须等于 exec_sw_stats.cmd。
*         执行器是逐像素数出来的，普查是矩形求交算出来的，两份写法不同。
*
*           C1  十一个整帧用例（gfx_case_build）。set-clip-state 有两张表，第二张依赖
*               第一张留下的裁剪框：普查的状态必须像执行器那样跨提交保留
*           C2  随机合法命令表 300 串：每串 1~4 张表连着提交（裁剪框跨表持续），
*               表里有 FILL、BLIT、表中间的 SET_CLIP、NOP，部分出屏、零宽高，
*               目的表面一半是帧缓冲、一半是比屏幕小的 200x150；有的表以 END 结束、有的靠 COUNT 耗尽
*           C3  边界：END 之后的记录不读、非法操作码与方程号返回 -1 且之前的计数保留、复位
*
*         覆盖行把 C2 里几类情况真正出现的次数打出来，每一类都必须大于 0。
*
*         注错开关见 lib/gfx/census.h，每个都必须让这里至少一行 FAIL：
*           CENSUS_NO_SETCLIP  -> C1 的 set-clip-state、C2
*           CENSUS_NO_SURF     -> C2（整帧用例的目的表面正好是 640x480，这一项在那里不起作用）
******************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx/gfx.h"
#include "gfx/census.h"
#include "core/format.h"
#include "core/xorshift32.h"
#include "test_census.h"

static uint32_t g_fail;

static void say(const char *s)
{
    fputs(s, stdout);
}

static void ck(const char *tag, int ok)
{
    char     line[200];
    core_out o;

    core_out_init(&o, line, (uint32_t)sizeof line);
    core_out_str(&o, tag);
    core_out_str(&o, ok ? " ok\n" : " FAIL\n");
    say(line);
    if (!ok)
        g_fail++;
}

/* --- C1 整帧用例 --------------------------------------------------------- */

static void c1_cases(void)
{
    uint8_t *dev = (uint8_t *)malloc(GFX_CASE_DEV_BYTES);
    uint32_t idx;

    if (dev == 0) {
        ck("C1 out of memory", 0);
        return;
    }

    for (idx = 0u; idx < GFX_CASE_N; idx++) {
        gfx_surface_table t;
        gfx_mem           m;
        gfx_cmd           cmds[2u * GFX_CASE_MAX_CMD];
        exec_sw_state     es;
        exec_sw_stats     st;
        gfx_census_state  cs;
        gfx_census        c;
        uint32_t          n1, n2 = 0u;
        int               e1, e2 = GFX_OK, r1, r2 = 0;
        char              line[160];
        core_out          o;

        memset(dev, 0, GFX_CASE_DEV_BYTES);
        m.dev_base = 0u;
        m.host     = dev;
        m.bytes    = GFX_CASE_DEV_BYTES;
        n1 = gfx_case_build(idx, &t, &m, GFX_CASE_FB_BASE, GFX_CASE_TEX_BASE, cmds, &n2);

        exec_sw_stats_clear(&st);
        exec_sw_reset(&es);
        e1 = exec_sw_run(&es, &t, &m, cmds, n1, n1, &st, 0);
        if (n2 > 0u)
            e2 = exec_sw_run(&es, &t, &m, cmds + n1, n2, n2, &st, 0);

        gfx_census_reset(&cs);
        gfx_census_clear(&c);
        r1 = gfx_census_run(&cs, &t, cmds, n1, &c);
        if (n2 > 0u)
            r2 = gfx_census_run(&cs, &t, cmds + n1, n2, &c);

        core_out_init(&o, line, (uint32_t)sizeof line);
        core_out_str(&o, "C1 ");
        core_out_str(&o, gfx_case_name(idx));
        core_out_str(&o, n2 > 0u ? " (2 lists)" : "");
        core_out_str(&o, " cmd ");
        core_out_u32(&o, gfx_census_cmd_total(&c));
        core_out_str(&o, "/");
        core_out_u32(&o, st.cmd);
        core_out_str(&o, " pix ");
        core_out_u32(&o, gfx_census_pix_total(&c));
        core_out_str(&o, "/");
        core_out_u32(&o, st.pixel_total);
        ck(line, e1 == GFX_OK && e2 == GFX_OK && r1 == 0 && r2 == 0
                 && gfx_census_cmd_total(&c) == st.cmd
                 && gfx_census_pix_total(&c) == st.pixel_total);
    }
    free(dev);
}

/* --- C2 随机合法命令表 --------------------------------------------------- */

#define C2_SEQS         300u
#define C2_LISTS_MAX    4u
#define C2_CMDS_MAX     40u

/* 设备地址布局：帧缓冲、200x150 的小目的、三张纹理，互不重叠 */
#define C2_FB           0x00001000u     /* 640x480，行跨 4096 */
#define C2_SMALL        0x001E1000u     /* 200x150，行跨 1024 */
#define C2_TEX_PM       0x00207000u     /* 64x64 预乘，行跨 256 */
#define C2_TEX_X        0x0020B000u     /* 64x64 XRGB */
#define C2_TEX_ST       0x0020F000u     /* 32x32 非预乘，行跨 128 */
#define C2_BYTES        0x00210000u

typedef struct {
    uint32_t setclip_mid;       /* 表中间的 SET_CLIP，后面还有绘制 */
    uint32_t partial;           /* 部分出屏的绘制 */
    uint32_t zero;              /* 零宽或零高 */
    uint32_t small_bound;       /* 目的是 200x150，矩形越过它的右边或下边（表面这一项起作用） */
    uint32_t carry;             /* 前一张表留下了非全屏的裁剪框，这一张接着画 */
    uint32_t no_end;            /* 靠 COUNT 耗尽结束的表 */
} c2_cover;

static void set_surf(gfx_surface_table *t, int id, uint32_t base, uint16_t w, uint16_t h,
                     uint8_t sl, uint8_t fmt)
{
    gfx_surface s;

    s.base = base; s.width = w; s.height = h; s.stride_log2 = sl;
    s.space = GFX_SPACE_DDR; s.format = fmt;
    (void)gfx_surface_set(t, id, &s);
}

static int32_t rnd_in(xs32 *g, int32_t lo, int32_t hi)
{
    return lo + (int32_t)(xs32_next(g) % (uint32_t)(hi - lo + 1));
}

static int blit_eq(xs32 *g, int src)
{
    static const int pm[] = { GFX_EQ_COPY, GFX_EQ_ALPHA, GFX_EQ_ADD, GFX_EQ_CONST, GFX_EQ_MUL };
    static const int st[] = { GFX_EQ_COPY, GFX_EQ_ADD, GFX_EQ_CONST, GFX_EQ_MUL, GFX_EQ_ALPHA_STRAIGHT };

    if (src == 2)
        return pm[xs32_next(g) % 5u];
    if (src == 4)
        return st[xs32_next(g) % 5u];
    return (int)(xs32_next(g) % 6u);
}

/* 追加一条绘制：成功返回 1，并按参数记覆盖。 */
static int add_draw(gfx_list *l, xs32 *g, c2_cover *cv)
{
    int      dst = (xs32_next(g) % 10u < 7u) ? 0 : 1;
    int32_t  x = rnd_in(g, -100, 700), y = rnd_in(g, -100, 560);
    int32_t  w = (xs32_next(g) % 10u == 0u) ? 0 : rnd_in(g, 1, 160);
    int32_t  h = (xs32_next(g) % 10u == 0u) ? 0 : rnd_in(g, 1, 160);
    int      rc;

    if (xs32_next(g) & 1u) {
        gfx_rect r;
        int      eq = (int)(xs32_next(g) % 6u);
        uint32_t argb = xs32_next(g);

        if (eq == GFX_EQ_ALPHA)
            argb |= 0xFF000000u;        /* 预乘约束：A = 255 时任何 RGB 都合法 */
        r.x0 = x; r.y0 = y; r.x1 = x + w - 1; r.y1 = y + h - 1;
        rc = gfx_fill(l, dst, r, argb, eq, (uint8_t)xs32_next(g));
    } else {
        static const int srcs[] = { 2, 3, 4 };
        int src = srcs[xs32_next(g) % 3u];
        int eq  = blit_eq(g, src);

        if (w > 80) w = w / 2;
        if (h > 80) h = h / 2;
        rc = gfx_blit(l, src, rnd_in(g, -8, 72), rnd_in(g, -8, 72), dst, (int)x, (int)y,
                      (int)w, (int)h, eq, (eq == GFX_EQ_CONST) ? (uint8_t)xs32_next(g) : 0u,
                      (int)(xs32_next(g) & 1u), 0x00FF00FFu);
    }
    if (rc != GFX_OK)
        return 0;

    if (w == 0 || h == 0) {
        cv->zero++;
    } else {
        if (x < 0 || y < 0 || x + w > GFX_SCREEN_W || y + h > GFX_SCREEN_H)
            cv->partial++;
        if (dst == 1 && x < 200 && y < 150 && (x + w > 200 || y + h > 150))
            cv->small_bound++;
    }
    return 1;
}

static void c2_random(void)
{
    uint8_t          *dev = (uint8_t *)calloc(1u, C2_BYTES);
    static gfx_cmd    buf[C2_CMDS_MAX + 1u];
    gfx_surface_table t;
    gfx_mem           m;
    xs32              g;
    c2_cover          cv;
    uint32_t          seq, bad = 0u, exec_err = 0u, lists = 0u, cmds = 0u;

    if (dev == 0) {
        ck("C2 out of memory", 0);
        return;
    }
    memset(&cv, 0, sizeof cv);
    m.dev_base = 0u;
    m.host     = dev;
    m.bytes    = C2_BYTES;

    gfx_surface_reset(&t);
    set_surf(&t, 0, C2_FB,     640u, 480u, 12u, GFX_FMT_XRGB8888);
    set_surf(&t, 1, C2_SMALL,  200u, 150u, 10u, GFX_FMT_XRGB8888);
    set_surf(&t, 2, C2_TEX_PM,  64u,  64u,  8u, GFX_FMT_ARGB8888_PM);
    set_surf(&t, 3, C2_TEX_X,   64u,  64u,  8u, GFX_FMT_XRGB8888);
    set_surf(&t, 4, C2_TEX_ST,  32u,  32u,  7u, GFX_FMT_ARGB8888_ST);

    xs32_seed(&g, 0xCE450001u);
    {
        uint32_t i;

        for (i = C2_TEX_PM; i < C2_BYTES; i += 4u) {
            uint32_t v = xs32_next(&g);

            memcpy(dev + i, &v, 4u);
        }
    }

    for (seq = 0u; seq < C2_SEQS; seq++) {
        exec_sw_state    es;
        exec_sw_stats    st;
        gfx_census_state cs;
        gfx_census       c;
        uint32_t         nl = 1u + xs32_next(&g) % C2_LISTS_MAX, li;
        int              clip_set = 0;

        exec_sw_reset(&es);
        exec_sw_stats_clear(&st);
        gfx_census_reset(&cs);
        gfx_census_clear(&c);

        for (li = 0u; li < nl; li++) {
            gfx_list l;
            uint32_t k, nc = 1u + xs32_next(&g) % C2_CMDS_MAX;
            int      pending_clip = 0, drew_after_carry = 0;
            int      carried = clip_set;        /* 前面的表留下过 SET_CLIP */

            gfx_list_begin(&l, buf, C2_CMDS_MAX + 1u, &t);
            for (k = 0u; k < nc; k++) {
                uint32_t pick = xs32_next(&g) % 10u;

                if (pick < 8u) {
                    if (add_draw(&l, &g, &cv)) {
                        if (pending_clip) {
                            cv.setclip_mid++;
                            pending_clip = 0;
                        }
                        if (li > 0u && carried && !drew_after_carry) {
                            cv.carry++;
                            drew_after_carry = 1;
                        }
                    }
                } else if (pick == 8u) {
                    gfx_rect r;

                    r.x0 = rnd_in(&g, -50, 650);
                    r.y0 = rnd_in(&g, -50, 500);
                    r.x1 = r.x0 + ((xs32_next(&g) % 10u == 0u) ? 0 : rnd_in(&g, 1, 400)) - 1;
                    r.y1 = r.y0 + rnd_in(&g, 1, 300) - 1;
                    if (gfx_set_clip(&l, r) == GFX_OK) {
                        clip_set = 1;
                        if (l.used > 1u)
                            pending_clip = 1;
                    }
                } else {
                    (void)gfx_nop(&l);
                }
            }
            if (xs32_next(&g) & 1u)
                (void)gfx_list_end(&l);
            else
                cv.no_end++;

            if (exec_sw_run(&es, &t, &m, buf, l.used, l.used, &st, 0) != GFX_OK)
                exec_err++;
            if (gfx_census_run(&cs, &t, buf, l.used, &c) != 0)
                exec_err++;
            lists++;
            cmds += l.used;
        }

        if (gfx_census_pix_total(&c) != st.pixel_total || gfx_census_cmd_total(&c) != st.cmd)
            bad++;
    }
    free(dev);

    {
        char     line[200];
        core_out o;

        core_out_init(&o, line, (uint32_t)sizeof line);
        core_out_str(&o, "C2 300 sequences (");
        core_out_u32(&o, lists);
        core_out_str(&o, " lists, ");
        core_out_u32(&o, cmds);
        core_out_str(&o, " records): pix == pixel_total, cmd == cmd");
        ck(line, bad == 0u && exec_err == 0u);

        core_out_init(&o, line, (uint32_t)sizeof line);
        core_out_str(&o, "C2 coverage setclip-mid=");
        core_out_u32(&o, cv.setclip_mid);
        core_out_str(&o, " partial=");
        core_out_u32(&o, cv.partial);
        core_out_str(&o, " zero=");
        core_out_u32(&o, cv.zero);
        core_out_str(&o, " small-dst-bound=");
        core_out_u32(&o, cv.small_bound);
        core_out_str(&o, " clip-carry=");
        core_out_u32(&o, cv.carry);
        core_out_str(&o, " no-end=");
        core_out_u32(&o, cv.no_end);
        ck(line, cv.setclip_mid > 0u && cv.partial > 0u && cv.zero > 0u
                 && cv.small_bound > 0u && cv.carry > 0u && cv.no_end > 0u);
    }
}

/* --- C3 边界 ------------------------------------------------------------ */

static void c3_edges(void)
{
    gfx_surface_table t;
    gfx_cmd           l[4];
    gfx_census_state  cs;
    gfx_census        c;
    gfx_rect          r;

    gfx_surface_reset(&t);
    set_surf(&t, 0, C2_FB, 640u, 480u, 12u, GFX_FMT_XRGB8888);
    memset(l, 0, sizeof l);

    /* FILL 10x10、END、FILL：END 之后的不读 */
    l[0].w[0] = GFX_OP_FILL;
    l[0].w[2] = 5u | (6u << 16);
    l[0].w[3] = 10u | (10u << 16);
    l[1].w[0] = GFX_OP_END;
    l[2] = l[0];
    gfx_census_reset(&cs);
    gfx_census_clear(&c);
    ck("C3 END stops: 1 FILL 100 px",
       gfx_census_run(&cs, &t, l, 3u, &c) == 0 && c.cmd_by_eq[GFX_EQ_COPY] == 1u
       && c.pix_by_eq[GFX_EQ_COPY] == 100u && gfx_census_cmd_total(&c) == 1u);

    /* 零面积也计一条；NOP 进无方程那一格 */
    l[1].w[0] = GFX_OP_NOP;
    l[2].w[3] = 0u | (10u << 16);
    l[3].w[0] = GFX_OP_END;
    gfx_census_clear(&c);
    ck("C3 zero area counts 1 cmd 0 px, NOP -> cmd_other",
       gfx_census_run(&cs, &t, l, 4u, &c) == 0 && c.cmd_by_eq[GFX_EQ_COPY] == 2u
       && c.pix_by_eq[GFX_EQ_COPY] == 100u && c.cmd_other == 1u);

    /* 非法操作码（BAND）：停下、返回 -1，之前的计数保留 */
    l[1].w[0] = GFX_OP_BAND;
    gfx_census_clear(&c);
    ck("C3 BAND -> -1, earlier counts kept",
       gfx_census_run(&cs, &t, l, 4u, &c) == -1 && c.cmd_by_eq[GFX_EQ_COPY] == 1u
       && gfx_census_cmd_total(&c) == 1u);

    /* 方程号 6：停下、返回 -1，不拿它索引计数数组 */
    l[1] = l[0];
    l[1].w[0] = GFX_OP_FILL | (6u << 8);
    gfx_census_clear(&c);
    ck("C3 eq 6 -> -1", gfx_census_run(&cs, &t, l, 4u, &c) == -1 && gfx_census_cmd_total(&c) == 1u);

    /* SET_CLIP 截到游戏区；复位回到全屏 */
    memset(l, 0, sizeof l);
    l[0].w[0] = GFX_OP_SET_CLIP;
    l[0].w[1] = (uint32_t)((int32_t)-20 & 0xFFFF) | (600u << 16);
    l[0].w[3] = 100u | (100u << 16);
    gfx_census_reset(&cs);
    gfx_census_clear(&c);
    (void)gfx_census_run(&cs, &t, l, 1u, &c);
    r = cs.clip;
    ck("C3 SET_CLIP clamped to screen at set time: (-20,600)+100x100 -> [0..79]x[600..479] empty",
       r.x0 == 0 && r.x1 == 79 && r.y0 == 600 && r.y1 == 479 && c.cmd_other == 1u);
    gfx_census_reset(&cs);
    ck("C3 reset -> full screen", cs.clip.x0 == 0 && cs.clip.y0 == 0
       && cs.clip.x1 == GFX_SCREEN_W - 1 && cs.clip.y1 == GFX_SCREEN_H - 1);
}

uint32_t test_census_run(void)
{
    g_fail = 0u;

    say("libTest census: per-equation census vs exec_sw\n");
    c1_cases();
    c2_random();
    c3_edges();

    return g_fail;
}
