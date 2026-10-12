/******************************************************************************
* @file test_blit_run.c: 批量提交 gfx_blit_run() 的主机侧测试
*
* @brief  随机 1000 组输入，gfx_blit_run() 的结果与对每条依次调用 gfx_blit() 逐字节相同——
*         整个缓冲区（含没写到的尾部）、used、rejected、overflow、first_error、ended 与成功条数。
*
*         每组先随机选一种「模板类别」，再随机选表的容量、预先追加几条、是否已经 END：
*           VALID  合法模板（源格式与方程匹配，宽高偶尔为 0）
*           EQ     方程号越界：6~15 走到校验报 BAD_EQUATION，-1、16、99 在参数检查报 BAD_ARG
*           C      方程不是 CONST 时 c 非零（BAD_FIELD）
*           KEY    色键开关不是 0 或 1（BAD_ARG）
*           WH     宽高越出 uint16（BAD_ARG）
*           SURF   表面号越界（BAD_ARG），或未登记、描述符非法（BAD_SURFACE）
*           FMT    源格式与方程不匹配、目的不是 XRGB（BAD_FORMAT），或目的在 TEX 空间（BAD_SPACE）
*           SAME   源表面等于目的表面，逐条坐标挤在一起，有的重叠（OVERLAP）有的不重叠
*         逐条坐标约 5% 越出 int16；容量 4~48，所以常常在中途表满；约 1/12 的组在已经 END 的表上跑。
*
*         另有 200 组「两次调用之间改写表面表」：前半段追加之后把源或目的槽位改掉，
*         后半段必须按改过的表重新校验（模板校验放在每次调用开头）。
*
*         覆盖行把参考侧（逐条 gfx_blit()）每种返回码出现的次数打出来，每一类都必须真的发生过，
*         否则「全部等价」可能只是因为那种情况从来没被走到。
*
*         注错开关见 lib/gfx/list.c 的 gfx_blit_run() 注释，每个都必须让这里至少一行 FAIL：
*           BLIT_RUN_W7           -> K1 全体等价
*           BLIT_RUN_SXY          -> K1 全体等价
*           BLIT_RUN_NO_FALLBACK  -> K1 源等于目的那一类（证明随机输入真的走到了逐条回退）
******************************************************************************/

#include <stdio.h>
#include <string.h>

#include "gfx/gfx.h"
#include "core/format.h"
#include "core/xorshift32.h"
#include "test_blit_run.h"

#define BR_GROUPS       1000u
#define BR_SPLIT_GROUPS 200u
#define BR_CAP_MAX      48u
#define BR_N_MAX        40u

enum { M_VALID, M_EQ, M_C, M_KEY, M_WH, M_SURF, M_FMT, M_SAME, M_COUNT };

/* 返回码到计数下标：0 是 OK，1~9 是 spec 错误码，10 FULL、11 CLOSED、12 BAD_ARG */
#define RC_FULL     10u
#define RC_CLOSED   11u
#define RC_BADARG   12u
#define RC_N        13u

typedef struct {
    gfx_blit_tmpl t;
    gfx_blit_pos  pos[BR_N_MAX];
    uint32_t      n;
    uint32_t      cap;
    uint32_t      prefill;
    int           end_first;
    uint32_t      mode;
    uint32_t      table_variant;
} br_group;

static uint32_t g_fail;
static uint32_t g_cnt[M_COUNT][RC_N];

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

static uint32_t rc_index(int code)
{
    if (code == GFX_BUILD_FULL)
        return RC_FULL;
    if (code == GFX_BUILD_CLOSED)
        return RC_CLOSED;
    if (code == GFX_BUILD_BAD_ARG)
        return RC_BADARG;
    if (code >= 0 && code <= 9)
        return (uint32_t)code;
    return RC_BADARG;       /* 不会出现：只有上面这几种返回值 */
}

/* --- 表面表 -------------------------------------------------------------- */

static void set_surf(gfx_surface_table *t, int id, uint32_t base, uint16_t w, uint16_t h,
                     uint8_t sl, uint8_t space, uint8_t fmt)
{
    gfx_surface s;

    s.base = base; s.width = w; s.height = h; s.stride_log2 = sl;
    s.space = space; s.format = fmt;
    (void)gfx_surface_set(t, id, &s);
}

/* 0 帧缓冲 640x480 XRGB；1 预乘 64x64；2 非预乘 64x64；3 XRGB 64x64；4 未登记；
   5 控制字保留位非零（BAD_SURFACE）；6 TEX 空间（BAD_SPACE）；7 预乘，只拿来当目的（BAD_FORMAT）。
   variant 1、2 分别再把 1 号、3 号清掉，让合法模板也会遇到未登记的源。 */
static void make_table(gfx_surface_table *t, uint32_t variant)
{
    gfx_surface_reset(t);
    set_surf(t, 0, 0x00001000u, 640u, 480u, 12u, GFX_SPACE_DDR, GFX_FMT_XRGB8888);
    set_surf(t, 1, 0x00200000u, 64u, 64u, 8u, GFX_SPACE_DDR, GFX_FMT_ARGB8888_PM);
    set_surf(t, 2, 0x00210000u, 64u, 64u, 8u, GFX_SPACE_DDR, GFX_FMT_ARGB8888_ST);
    set_surf(t, 3, 0x00220000u, 64u, 64u, 8u, GFX_SPACE_DDR, GFX_FMT_XRGB8888);
    gfx_surface_set_raw(t, 5, 0x00230000u, 64u | (64u << 16), 0x80000000u | 8u | 0x20u);
    set_surf(t, 6, 0u, 64u, 64u, 8u, GFX_SPACE_TEX, GFX_FMT_XRGB8888);
    set_surf(t, 7, 0x00250000u, 64u, 64u, 8u, GFX_SPACE_DDR, GFX_FMT_ARGB8888_PM);
    if (variant == 1u)
        gfx_surface_set_raw(t, 1, 0u, 0u, 0u);
    if (variant == 2u)
        gfx_surface_set_raw(t, 3, 0u, 0u, 0u);
}

/* --- 随机输入 ------------------------------------------------------------ */

static uint32_t rnd(xs32 *g, uint32_t n)
{
    return xs32_next(g) % n;
}

static int32_t rnd_in(xs32 *g, int32_t lo, int32_t hi)      /* [lo, hi] */
{
    return lo + (int32_t)rnd(g, (uint32_t)(hi - lo + 1));
}

static const int32_t BR_FAR[] = { 32768, -32769, 40000, -40000, INT32_MIN, INT32_MAX };

/* 与源格式匹配的方程（规范条款 §2 末的三行） */
static int eq_for_src(xs32 *g, int src)
{
    static const int pm[] = { GFX_EQ_COPY, GFX_EQ_ALPHA, GFX_EQ_ADD, GFX_EQ_CONST, GFX_EQ_MUL };
    static const int st[] = { GFX_EQ_COPY, GFX_EQ_ADD, GFX_EQ_CONST, GFX_EQ_MUL, GFX_EQ_ALPHA_STRAIGHT };

    if (src == 1)
        return pm[rnd(g, 5u)];
    if (src == 2)
        return st[rnd(g, 5u)];
    return (int)rnd(g, 6u);                     /* XRGB 源六个方程都行 */
}

static void gen_group(xs32 *g, br_group *gr, uint32_t force_mode)
{
    gfx_blit_tmpl *t = &gr->t;
    uint32_t       i;

    gr->mode          = (force_mode < M_COUNT) ? force_mode : rnd(g, M_COUNT);
    gr->table_variant = (rnd(g, 10u) == 0u) ? 1u + rnd(g, 2u) : 0u;
    gr->cap           = 4u + rnd(g, BR_CAP_MAX - 3u);
    gr->prefill       = rnd(g, 4u);
    gr->end_first     = (rnd(g, 12u) == 0u);
    gr->n             = rnd(g, BR_N_MAX + 1u);

    t->src    = (int)rnd(g, 4u);
    t->dst    = (rnd(g, 2u) == 0u) ? 0 : 3;
    t->w      = 1 + (int)rnd(g, 32u);
    t->h      = 1 + (int)rnd(g, 32u);
    t->eq     = eq_for_src(g, t->src);
    t->c      = 0u;
    t->key_en = (int)rnd(g, 2u);
    t->key    = xs32_next(g);
    if (t->src == t->dst)
        t->src = 3 - t->dst;                    /* 源等于目的只在 SAME 类里出现 */
    if (t->src == 0 || t->src == 3)
        t->eq = eq_for_src(g, t->src);

    switch (gr->mode) {
    case M_VALID:
        if (rnd(g, 10u) == 0u)
            t->w = 0;
        if (rnd(g, 10u) == 0u)
            t->h = 0;
        break;
    case M_EQ:
        t->src = 3;
        if (rnd(g, 2u) == 0u) {
            t->eq = 6 + (int)rnd(g, 10u);
        } else {
            static const int far_eq[] = { -1, 16, 99 };
            t->eq = far_eq[rnd(g, 3u)];
        }
        break;
    case M_C:
        t->src = 3;
        do {
            t->eq = (int)rnd(g, 6u);
        } while (t->eq == GFX_EQ_CONST);
        t->c = (uint8_t)(1u + rnd(g, 255u));
        break;
    case M_KEY: {
        static const int far_key[] = { 2, -1, 7 };
        t->key_en = far_key[rnd(g, 3u)];
        break;
    }
    case M_WH: {
        static const int far_wh[] = { -1, 65536, 100000 };
        if (rnd(g, 2u) == 0u)
            t->w = far_wh[rnd(g, 3u)];
        else
            t->h = far_wh[rnd(g, 3u)];
        break;
    }
    case M_SURF: {
        static const int far_id[] = { -1, 16, 99, 4, 5 };
        if (rnd(g, 2u) == 0u)
            t->src = far_id[rnd(g, 5u)];
        else
            t->dst = far_id[rnd(g, 5u)];
        break;
    }
    case M_FMT:
        switch (rnd(g, 4u)) {
        case 0u: t->src = 1; t->eq = GFX_EQ_ALPHA_STRAIGHT; break;
        case 1u: t->src = 2; t->eq = GFX_EQ_ALPHA;          break;
        case 2u: t->src = 3; t->dst = 7;                    break;
        default: t->src = 3; t->dst = 6;                    break;
        }
        break;
    default:                                    /* M_SAME */
        t->src = t->dst = (rnd(g, 2u) == 0u) ? 0 : 3;
        t->eq  = eq_for_src(g, t->src);
        t->w   = 1 + (int)rnd(g, 16u);
        t->h   = 1 + (int)rnd(g, 16u);
        break;
    }
    if (t->eq == GFX_EQ_CONST && gr->mode != M_C)
        t->c = (uint8_t)rnd(g, 256u);

    for (i = 0u; i < gr->n; i++) {
        gfx_blit_pos *p = &gr->pos[i];

        if (gr->mode == M_SAME) {
            p->dx = rnd_in(g, 0, 47);
            p->dy = rnd_in(g, 0, 47);
            p->sx = rnd_in(g, 0, 47);
            p->sy = rnd_in(g, 0, 47);
        } else {
            p->dx = rnd_in(g, -40, 680);
            p->dy = rnd_in(g, -40, 520);
            p->sx = rnd_in(g, -8, 72);
            p->sy = rnd_in(g, -8, 72);
        }
        if (rnd(g, 20u) == 0u) {
            int32_t far = BR_FAR[rnd(g, 6u)];

            switch (rnd(g, 4u)) {
            case 0u:  p->dx = far; break;
            case 1u:  p->dy = far; break;
            case 2u:  p->sx = far; break;
            default:  p->sy = far; break;
            }
        }
    }
}

/* --- 两条路径 ------------------------------------------------------------ */

static void begin_list(gfx_list *l, gfx_cmd *buf, const br_group *gr,
                       const gfx_surface_table *t)
{
    uint32_t i, w;
    gfx_rect r;

    /* 缓冲区先写一段固定的垃圾：没写到的尾部也要两边一样，才能整块逐字节比 */
    for (i = 0u; i < BR_CAP_MAX; i++)
        for (w = 0u; w < 8u; w++)
            buf[i].w[w] = 0xA5A50000u ^ (i << 4) ^ w;

    gfx_list_begin(l, buf, gr->cap, t);
    r.x0 = 10; r.y0 = 20; r.x1 = 29; r.y1 = 39;
    for (i = 0u; i < gr->prefill; i++) {
        if ((i & 1u) == 0u)
            (void)gfx_nop(l);
        else
            (void)gfx_fill(l, 0, r, 0x00123456u, GFX_EQ_COPY, 0u);
    }
    if (gr->end_first)
        (void)gfx_list_end(l);
}

/* 参考：对每条依次调用 gfx_blit()，顺带按类别记下每种返回码的次数 */
static uint32_t run_ref(gfx_list *l, const br_group *gr, uint32_t from, uint32_t to, int count)
{
    uint32_t i, ok = 0u;

    for (i = from; i < to; i++) {
        const gfx_blit_pos *p = &gr->pos[i];
        int rc = gfx_blit(l, gr->t.src, (int)p->sx, (int)p->sy, gr->t.dst, (int)p->dx, (int)p->dy,
                          gr->t.w, gr->t.h, gr->t.eq, gr->t.c, gr->t.key_en, gr->t.key);

        if (rc == GFX_OK)
            ok++;
        if (count)
            g_cnt[gr->mode][rc_index(rc)]++;
    }
    return ok;
}

static int same_state(const gfx_list *a, const gfx_cmd *ba, uint32_t oka,
                      const gfx_list *b, const gfx_cmd *bb, uint32_t okb)
{
    return memcmp(ba, bb, sizeof(gfx_cmd) * BR_CAP_MAX) == 0
        && a->used == b->used && a->rejected == b->rejected && a->overflow == b->overflow
        && a->first_error == b->first_error && a->ended == b->ended && oka == okb;
}

/* --- K1 随机 1000 组 ------------------------------------------------------ */

static void b1_random(void)
{
    static gfx_cmd    bufa[BR_CAP_MAX], bufb[BR_CAP_MAX];
    gfx_surface_table ta, tb;
    gfx_list          la, lb;
    br_group          gr;
    xs32              g;
    uint32_t          k, bad = 0u, bad_same = 0u, n_same = 0u, full_mid = 0u;
    uint32_t          appended = 0u, rejected = 0u;

    memset(g_cnt, 0, sizeof g_cnt);
    xs32_seed(&g, 0xB1A70001u);

    for (k = 0u; k < BR_GROUPS; k++) {
        uint32_t oka, okb, ov0;

        gen_group(&g, &gr, M_COUNT);
        make_table(&ta, gr.table_variant);
        make_table(&tb, gr.table_variant);

        begin_list(&la, bufa, &gr, &ta);
        ov0 = la.overflow;
        oka = run_ref(&la, &gr, 0u, gr.n, 1);

        begin_list(&lb, bufb, &gr, &tb);
        okb = gfx_blit_run(&lb, &gr.t, gr.pos, gr.n);

        if (!same_state(&la, bufa, oka, &lb, bufb, okb)) {
            bad++;
            if (gr.mode == M_SAME)
                bad_same++;
        }
        if (gr.mode == M_SAME)
            n_same++;
        if (oka > 0u && la.overflow > ov0)
            full_mid++;
        appended += oka;
        rejected += gr.n - oka;
    }

    ck("K1 1000 groups: run == per-call gfx_blit (buffer used rejected overflow first_error ended ok)",
       bad == 0u);
    {
        char     line[120];
        core_out o;

        core_out_init(&o, line, (uint32_t)sizeof line);
        core_out_str(&o, "K1 src==dst groups ");
        core_out_u32(&o, n_same);
        core_out_str(&o, ": run == per-call gfx_blit");
        ck(line, bad_same == 0u && n_same > 0u);

        core_out_init(&o, line, (uint32_t)sizeof line);
        core_out_str(&o, "K1 calls appended ");
        core_out_u32(&o, appended);
        core_out_str(&o, " rejected ");
        core_out_u32(&o, rejected);
        core_out_str(&o, " full-mid-run groups ");
        core_out_u32(&o, full_mid);
        core_out_str(&o, "\n");
        say(line);
    }

    /* 覆盖：每一类都真的发生过 */
    {
        char     line[240];
        core_out o;
        uint32_t coord = g_cnt[M_VALID][RC_BADARG] + g_cnt[M_SAME][RC_BADARG];
        uint32_t full = 0u, closed = 0u, m;
        int      ok;

        for (m = 0u; m < M_COUNT; m++) {
            full   += g_cnt[m][RC_FULL];
            closed += g_cnt[m][RC_CLOSED];
        }
        ok = g_cnt[M_VALID][GFX_OK] > 0u
          && g_cnt[M_EQ][GFX_E_BAD_EQUATION] > 0u && g_cnt[M_EQ][RC_BADARG] > 0u
          && g_cnt[M_C][GFX_E_BAD_FIELD] > 0u
          && g_cnt[M_KEY][RC_BADARG] > 0u
          && g_cnt[M_WH][RC_BADARG] > 0u
          && g_cnt[M_SURF][GFX_E_BAD_SURFACE] > 0u && g_cnt[M_SURF][RC_BADARG] > 0u
          && g_cnt[M_FMT][GFX_E_BAD_FORMAT] > 0u && g_cnt[M_FMT][GFX_E_BAD_SPACE] > 0u
          && g_cnt[M_SAME][GFX_E_OVERLAP] > 0u && g_cnt[M_SAME][GFX_OK] > 0u
          && coord > 0u && full > 0u && full_mid > 0u && closed > 0u;

        core_out_init(&o, line, (uint32_t)sizeof line);
        core_out_str(&o, "K1 coverage ok=");        core_out_u32(&o, g_cnt[M_VALID][GFX_OK]);
        core_out_str(&o, " eq=");                   core_out_u32(&o, g_cnt[M_EQ][GFX_E_BAD_EQUATION]);
        core_out_str(&o, "/");                      core_out_u32(&o, g_cnt[M_EQ][RC_BADARG]);
        core_out_str(&o, " c=");                    core_out_u32(&o, g_cnt[M_C][GFX_E_BAD_FIELD]);
        core_out_str(&o, " key=");                  core_out_u32(&o, g_cnt[M_KEY][RC_BADARG]);
        core_out_str(&o, " wh=");                   core_out_u32(&o, g_cnt[M_WH][RC_BADARG]);
        core_out_str(&o, " surf=");                 core_out_u32(&o, g_cnt[M_SURF][GFX_E_BAD_SURFACE]);
        core_out_str(&o, "/");                      core_out_u32(&o, g_cnt[M_SURF][RC_BADARG]);
        core_out_str(&o, " fmt=");                  core_out_u32(&o, g_cnt[M_FMT][GFX_E_BAD_FORMAT]);
        core_out_str(&o, "/");                      core_out_u32(&o, g_cnt[M_FMT][GFX_E_BAD_SPACE]);
        core_out_str(&o, " overlap=");              core_out_u32(&o, g_cnt[M_SAME][GFX_E_OVERLAP]);
        core_out_str(&o, " same-ok=");              core_out_u32(&o, g_cnt[M_SAME][GFX_OK]);
        core_out_str(&o, " coord=");                core_out_u32(&o, coord);
        core_out_str(&o, " full=");                 core_out_u32(&o, full);
        core_out_str(&o, " closed=");               core_out_u32(&o, closed);
        ck(line, ok);
    }
}

/* --- K2 两次调用之间改写表面表 -------------------------------------------- */

static void retarget(gfx_surface_table *t, const br_group *gr, uint32_t how)
{
    int id = (how == 1u) ? gr->t.dst : gr->t.src;

    if (id < 0 || id >= (int)GFX_SURF_N)
        return;
    if (how == 2u)                              /* 源改成非预乘：与 ALPHA 不再匹配 */
        set_surf(t, id, 0x00300000u, 64u, 64u, 8u, GFX_SPACE_DDR, GFX_FMT_ARGB8888_ST);
    else                                        /* 清掉：未登记 */
        gfx_surface_set_raw(t, id, 0u, 0u, 0u);
}

static void b2_split(void)
{
    static gfx_cmd    bufa[BR_CAP_MAX], bufb[BR_CAP_MAX];
    gfx_surface_table ta, tb;
    gfx_list          la, lb;
    br_group          gr;
    xs32              g;
    uint32_t          k, bad = 0u, changed = 0u;

    xs32_seed(&g, 0xB2A70002u);
    for (k = 0u; k < BR_SPLIT_GROUPS; k++) {
        uint32_t m, how, oka, okb, ok_first;

        gen_group(&g, &gr, (rnd(&g, 2u) == 0u) ? M_VALID : M_SAME);
        gr.end_first = 0;
        gr.cap       = BR_CAP_MAX;
        m            = rnd(&g, gr.n + 1u);
        how          = rnd(&g, 3u);
        make_table(&ta, 0u);
        make_table(&tb, 0u);

        begin_list(&la, bufa, &gr, &ta);
        ok_first = run_ref(&la, &gr, 0u, m, 0);
        retarget(&ta, &gr, how);
        oka = ok_first + run_ref(&la, &gr, m, gr.n, 0);

        begin_list(&lb, bufb, &gr, &tb);
        okb = gfx_blit_run(&lb, &gr.t, gr.pos, m);
        retarget(&tb, &gr, how);
        okb += gfx_blit_run(&lb, &gr.t, gr.pos + m, gr.n - m);

        if (!same_state(&la, bufa, oka, &lb, bufb, okb))
            bad++;
        /* 改写之后后半段有被拒的，说明这一组真的测到了「开头重查」 */
        if (la.rejected > 0u && m < gr.n && ok_first == m && oka < gr.n)
            changed++;
    }
    ck("K2 200 groups, table rewritten between two calls: equivalent", bad == 0u);
    {
        char     line[96];
        core_out o;

        core_out_init(&o, line, (uint32_t)sizeof line);
        core_out_str(&o, "K2 coverage groups rejected after rewrite ");
        core_out_u32(&o, changed);
        ck(line, changed > 0u);
    }
}

/* --- K3 边界 ------------------------------------------------------------ */

static void b3_edges(void)
{
    gfx_surface_table t;
    gfx_list          l;
    gfx_cmd           buf[4];
    gfx_blit_tmpl     tm;
    gfx_blit_pos      p;

    make_table(&t, 0u);
    memset(&tm, 0, sizeof tm);
    tm.src = 3; tm.dst = 0; tm.w = 8; tm.h = 8; tm.eq = GFX_EQ_COPY;
    p.dx = 1; p.dy = 2; p.sx = 3; p.sy = 4;

    /* 表没有绑定表面表：与 gfx_blit() 一样什么也不动 */
    gfx_list_begin(&l, buf, 4u, 0);
    ck("K3 unbound list: returns 0, no counters touched",
       gfx_blit_run(&l, &tm, &p, 1u) == 0u && l.rejected == 0u && l.used == 0u
       && l.first_error == GFX_OK);

    /* n = 0：什么也不动 */
    gfx_list_begin(&l, buf, 4u, &t);
    ck("K3 n=0: nothing appended",
       gfx_blit_run(&l, &tm, &p, 0u) == 0u && l.used == 0u && l.rejected == 0u);

    /* 一条合法记录的字节，与 gfx_blit() 追加的逐字对上 */
    {
        gfx_cmd  buf2[4];
        gfx_list l2;

        gfx_list_begin(&l, buf, 4u, &t);
        gfx_list_begin(&l2, buf2, 4u, &t);
        (void)gfx_blit_run(&l, &tm, &p, 1u);
        (void)gfx_blit(&l2, 3, 3, 4, 0, 1, 2, 8, 8, GFX_EQ_COPY, 0u, 0, 0u);
        ck("K3 one record: w0..w7 == gfx_blit",
           l.used == 1u && l2.used == 1u && memcmp(&buf[0], &buf2[0], sizeof(gfx_cmd)) == 0
           && buf[0].w[1] == (3u | (4u << 16)) && buf[0].w[2] == (1u | (2u << 16)) && buf[0].w[7] == 0u);
    }
}

uint32_t test_blit_run_run(void)
{
    g_fail = 0u;

    say("libTest gfx_blit_run: batch == per-call\n");
    _Static_assert(sizeof(gfx_blit_pos) == 16u, "gfx_blit_pos 必须是 16 字节");

    b1_random();
    b2_split();
    b3_edges();

    return g_fail;
}
