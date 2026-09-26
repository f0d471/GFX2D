/******************************************************************************
* @file test_stg_collide.c: 碰撞（SW7 2.4 节，自机判定点对敌弹）的主机侧测试
*
* @brief  八组，判据见 SW7 第 3 节「碰撞」一行：
*           X1  边界：距离恰好等于半径和不命中、差 1/256 像素命中、重叠命中，四个方向各一例
*           X2  勾股数 3k、4k、5k：恰好相切不命中，再近 1/256 像素命中，四个象限
*           X3  坐标差接近游戏区尺寸（自机在角上、弹在对角）：粗筛挡住，没有命中与擦弹；
*               只有一个轴的差大也挡住。平方在粗筛之后才做，UBSan 不报溢出
*           X4  擦弹每颗只计一次
*           X5  受击关掉：不计命中、不打标记，擦弹照算（本来会命中的按擦弹计）；再打开后照常命中
*           X6  已标记移除的不参与
*           X7  随机样本对朴素参考：64 位平方、不粗筛，命中数、擦弹数与每颗的标志全同；
*               一半的轮次坐标落在整像素网格上，恰好相切的情况大量出现
*           X8  场里跑自机狙一段：每帧被标记移除的个数等于上一帧的命中数，累计计数对得上
*
*         注错开关（见 stg_pool.h 的文件头），每个都必须见红：
*           COLLIDE_LE       -> X1 的相切、X2 的相切、X7
*           COLLIDE_NO_CULL  -> X3（平方溢出：64 位构建的 UBSan 在 stderr 报 runtime error，结果也错）
******************************************************************************/

#include <stdint.h>
#include <stdio.h>

#include "core/format.h"
#include "core/xorshift32.h"
#include "game/stg_field.h"
#include "game/stg_pattern.h"
#include "game/stg_pool.h"
#include "test_stg_collide.h"

static uint32_t g_fail;

/* 池与场都放静态区。 */
static stg_ent   s_mem[4096];
static stg_ent   s_ref[4096];
static stg_field s_field;

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

/* 16 项判定半径：弹种 0 为 r0，1 为 r1，其余为 0。 */
static void radii(int32_t r[16], int32_t r0, int32_t r1)
{
    uint32_t i;

    for (i = 0u; i < 16u; i++)
        r[i] = 0;
    r[0] = r0;
    r[1] = r1;
}

static void probe(stg_probe *pr, int32_t px, int32_t py, int on)
{
    pr->px          = px;
    pr->py          = py;
    pr->r_hit       = STG_PLAYER_R_HIT;
    pr->r_graze     = STG_PLAYER_R_GRAZE;
    pr->hit_enabled = on;
}

/* 一颗弹、一次碰撞，返回 (命中, 擦弹, 标志)。 */
typedef struct {
    uint32_t hits, grazes;
    uint16_t flags;
} one_res;

static one_res one(const int32_t *r, const stg_probe *pr, int32_t x, int32_t y, uint16_t flags)
{
    stg_pool      p;
    stg_hit_stats st;
    stg_ent       e;
    one_res       res;

    stg_pool_init(&p, s_mem, 4096u);
    (void)stg_pool_add(&p, x, y, 0, 0, 0u, flags);
    if ((flags & STG_F_REMOVE) != 0u)
        stg_pool_mark(&p, 0u);
    stg_pool_collide(&p, r, pr, &st);
    (void)stg_pool_get(&p, 0u, &e);
    res.hits   = st.hits;
    res.grazes = st.grazes;
    res.flags  = e.flags;
    return res;
}

static int is_hit(one_res a)
{
    return a.hits == 1u && a.grazes == 0u && (a.flags & STG_F_REMOVE) != 0u
        && (a.flags & STG_F_GRAZED) == 0u;
}

static int is_graze(one_res a)
{
    return a.hits == 0u && a.grazes == 1u && (a.flags & STG_F_REMOVE) == 0u
        && (a.flags & STG_F_GRAZED) != 0u;
}

static int is_none(one_res a, uint16_t flags0)
{
    return a.hits == 0u && a.grazes == 0u && a.flags == flags0;
}

/* --- X1 边界 ------------------------------------------------------------- */

static void x1_boundary(void)
{
    const int32_t px = 200 * FX_ONE, py = 400 * FX_ONE;
    const int32_t s = STG_PLAYER_R_HIT + 4 * FX_ONE;     /* 2 + 4 像素 = 1536 */
    int32_t       r[16];
    stg_probe     pr;
    int           tangent_ok = 1, closer_ok = 1;
    int           d;

    radii(r, 4 * FX_ONE, 0);
    probe(&pr, px, py, 1);

    /* 四个方向：+x、-x、+y、-y */
    for (d = 0; d < 4; d++) {
        const int32_t sx = (d == 0) ? 1 : (d == 1) ? -1 : 0;
        const int32_t sy = (d == 2) ? 1 : (d == 3) ? -1 : 0;

        if (!is_graze(one(r, &pr, px + sx * s, py + sy * s, 0u)))
            tangent_ok = 0;
        if (!is_hit(one(r, &pr, px + sx * (s - 1), py + sy * (s - 1), 0u)))
            closer_ok = 0;
    }
    ck("X1 distance == r_p + r_b (1536) in 4 directions: no hit, graze", tangent_ok);
    ck("X1 distance == r_p + r_b - 1/256 px in 4 directions: hit, marked", closer_ok);
    ck("X1 overlap (same center): hit, marked", is_hit(one(r, &pr, px, py, 0u)));
}

/* --- X2 勾股数 ----------------------------------------------------------- */

static void x2_pythagoras(void)
{
    /* r_p + r_b = 512 + 1048 = 1560 = 5 * 312；(936, 1248) 恰好相切 */
    const int32_t px = 100 * FX_ONE, py = 300 * FX_ONE;
    const int32_t a = 3 * 312, b = 4 * 312;
    int32_t       r[16];
    stg_probe     pr;
    int           q, tangent_ok = 1, closer_ok = 1;

    radii(r, 1048, 0);
    probe(&pr, px, py, 1);
    for (q = 0; q < 4; q++) {
        const int32_t sx = (q & 1) ? -1 : 1;
        const int32_t sy = (q & 2) ? -1 : 1;
        const int32_t dx = (q < 2) ? a : b;
        const int32_t dy = (q < 2) ? b : a;

        if (!is_graze(one(r, &pr, px + sx * dx, py + sy * dy, 0u)))
            tangent_ok = 0;
        if (!is_hit(one(r, &pr, px + sx * dx, py + sy * (dy - 1), 0u)))
            closer_ok = 0;
    }
    ck("X2 3k-4k-5k tangent (936, 1248) vs 1560 in 4 quadrants: no hit", tangent_ok);
    ck("X2 one step closer (936, 1247): hit", closer_ok);
}

/* --- X3 大坐标差 --------------------------------------------------------- */

static void x3_far(void)
{
    /* 自机在游戏区左上角外扩处，弹在右下角外扩处：坐标差接近游戏区尺寸加两倍外扩 */
    const int32_t lo = -STG_MARGIN_PX * FX_ONE;
    const int32_t hx = (STG_FIELD_W + STG_MARGIN_PX) * FX_ONE - 1;
    const int32_t hy = (STG_FIELD_H + STG_MARGIN_PX) * FX_ONE - 1;
    const int32_t t = STG_PLAYER_R_GRAZE + 4 * FX_ONE;
    int32_t       r[16];
    stg_probe     pr, pr2;
    int           ok = 1;

    radii(r, 4 * FX_ONE, 0);
    probe(&pr, lo, lo, 1);
    probe(&pr2, hx, hy, 1);

    ok &= is_none(one(r, &pr, hx, hy, 0u), 0u);                 /* 两个轴都大 */
    ok &= is_none(one(r, &pr2, lo, lo, 0u), 0u);                /* 反过来 */
    ok &= is_none(one(r, &pr, lo, hy, 0u), 0u);                 /* 只有 y 大 */
    ok &= is_none(one(r, &pr, hx, lo, 0u), 0u);                 /* 只有 x 大 */
    ok &= is_none(one(r, &pr, lo + t - 1, hy, 0u), 0u);         /* x 在阈值内、y 很大 */
    ok &= is_none(one(r, &pr, hx, lo + t - 1, 0u), 0u);         /* y 在阈值内、x 很大 */
    ok &= is_none(one(r, &pr, lo + t, lo, 0u), 0u);             /* 恰在阈值上：粗筛挡住 */
    ok &= is_graze(one(r, &pr, lo + t - 1, lo, 0u));            /* 阈值内一格：擦弹 */
    ck("X3 differences near field size (104959, 125439), either axis: culled, no hit, no graze", ok);
}

/* --- X4 擦弹一次 --------------------------------------------------------- */

static void x4_graze_once(void)
{
    const int32_t px = 200 * FX_ONE, py = 400 * FX_ONE;
    int32_t       r[16];
    stg_probe     pr;
    stg_pool      p;
    stg_hit_stats s1, s2, s3;
    stg_ent       e;

    radii(r, 4 * FX_ONE, 0);
    probe(&pr, px, py, 1);
    stg_pool_init(&p, s_mem, 4096u);
    (void)stg_pool_add(&p, px + 4000, py, 0, 0, 0u, 0u);
    (void)stg_pool_add(&p, px, py - 4500, 0, 0, 0u, 0u);
    stg_pool_collide(&p, r, &pr, &s1);
    stg_pool_collide(&p, r, &pr, &s2);
    stg_pool_collide(&p, r, &pr, &s3);
    (void)stg_pool_get(&p, 0u, &e);
    ck("X4 two bullets inside graze radius, 3 passes: grazes 2, 0, 0; no hit",
       s1.grazes == 2u && s2.grazes == 0u && s3.grazes == 0u
       && s1.hits + s2.hits + s3.hits == 0u && e.flags == STG_F_GRAZED);
}

/* --- X5 受击关掉 --------------------------------------------------------- */

static void x5_hits_off(void)
{
    const int32_t px = 200 * FX_ONE, py = 400 * FX_ONE;
    int32_t       r[16];
    stg_probe     pr;
    stg_pool      p;
    stg_hit_stats s1, s2, s3;
    stg_ent       e;

    radii(r, 4 * FX_ONE, 0);
    probe(&pr, px, py, 0);
    stg_pool_init(&p, s_mem, 4096u);
    (void)stg_pool_add(&p, px, py, 0, 0, 0u, 0u);           /* 重叠：本来会命中 */
    (void)stg_pool_add(&p, px + 3000, py, 0, 0, 0u, 0u);    /* 擦弹 */
    stg_pool_collide(&p, r, &pr, &s1);
    stg_pool_collide(&p, r, &pr, &s2);
    (void)stg_pool_get(&p, 0u, &e);
    ck("X5 hits off: no hit, no mark; overlapping bullet counts as graze (2 grazes), once",
       s1.hits == 0u && s1.grazes == 2u && s2.hits == 0u && s2.grazes == 0u
       && e.flags == STG_F_GRAZED && stg_pool_count(&p) == 2u);

    pr.hit_enabled = 1;
    stg_pool_collide(&p, r, &pr, &s3);
    (void)stg_pool_get(&p, 0u, &e);
    ck("X5 hits back on: the overlapping (already grazed) bullet hits",
       s3.hits == 1u && s3.grazes == 0u && (e.flags & STG_F_REMOVE) != 0u);
}

/* --- X6 已标记的跳过 ----------------------------------------------------- */

static void x6_marked(void)
{
    int32_t   r[16];
    stg_probe pr;

    radii(r, 4 * FX_ONE, 0);
    probe(&pr, 0, 0, 1);
    ck("X6 marked bullet on top of the player: skipped, flags unchanged",
       is_none(one(r, &pr, 0, 0, STG_F_REMOVE), STG_F_REMOVE));
}

/* --- X7 随机对朴素参考 --------------------------------------------------- */

/* 朴素参考：64 位平方，不做粗筛。与产品代码是两段写法。 */
static void ref_collide(stg_ent *e, uint32_t n, const int32_t *r, const stg_probe *pr,
                        uint32_t *hits, uint32_t *grazes)
{
    uint32_t i;

    *hits = 0u;
    *grazes = 0u;
    for (i = 0u; i < n; i++) {
        const int64_t rb = r[e[i].flags & 15u];
        const int64_t dx = (int64_t)e[i].x - pr->px;
        const int64_t dy = (int64_t)e[i].y - pr->py;
        const int64_t d2 = dx * dx + dy * dy;
        const int64_t rh = (int64_t)pr->r_hit + rb;
        const int64_t rg = (int64_t)pr->r_graze + rb;

        if (e[i].flags & STG_F_REMOVE)
            continue;
        if (pr->hit_enabled && d2 < rh * rh) {
            (*hits)++;
            e[i].flags = (uint16_t)(e[i].flags | STG_F_REMOVE);
        } else if (!(e[i].flags & STG_F_GRAZED) && d2 < rg * rg) {
            (*grazes)++;
            e[i].flags = (uint16_t)(e[i].flags | STG_F_GRAZED);
        }
    }
}

static void x7_random(void)
{
    const uint32_t rounds = 200u, n = 2000u;
    xs32           g;
    uint32_t       rd, i;
    uint32_t       mism = 0u, tot_hit = 0u, tot_graze = 0u, tot_cull = 0u, tot_tangent = 0u;
    int32_t        r[16];

    xs32_seed(&g, 0x5E7C0001u);
    for (rd = 0u; rd < rounds; rd++) {
        const int      grid = (rd & 1u) != 0u;              /* 一半的轮次在整像素网格上 */
        stg_pool       p;
        stg_probe      pr;
        stg_hit_stats  st;
        uint32_t       rh, rg;
        int32_t        k;

        for (k = 0; k < 16; k++)
            r[k] = 0;
        r[0] = (int32_t)(1u + xs32_next(&g) % 8u) * FX_ONE;         /* 1~8 像素 */
        r[1] = (int32_t)(xs32_next(&g) % 2048u);                    /* 0~8 像素，任意 1/256 */
        r[2] = 3 * FX_ONE;
        pr.px          = (int32_t)(xs32_next(&g) % (400u * 256u));
        pr.py          = (int32_t)(xs32_next(&g) % (480u * 256u));
        pr.r_hit       = STG_PLAYER_R_HIT;
        pr.r_graze     = STG_PLAYER_R_GRAZE;
        pr.hit_enabled = (xs32_next(&g) % 4u) != 0u;

        stg_pool_init(&p, s_mem, 4096u);
        for (i = 0u; i < n; i++) {
            int32_t  dx, dy;
            uint16_t fl = (uint16_t)(xs32_next(&g) % 4u);           /* 弹种 0~3，3 的半径为 0 */

            if (grid) {
                dx = ((int32_t)(xs32_next(&g) % 61u) - 30) * FX_ONE;
                dy = ((int32_t)(xs32_next(&g) % 61u) - 30) * FX_ONE;
            } else {
                dx = (int32_t)(xs32_next(&g) % 16385u) - 8192;
                dy = (int32_t)(xs32_next(&g) % 16385u) - 8192;
            }
            if (xs32_next(&g) % 8u == 0u)
                fl = (uint16_t)(fl | STG_F_GRAZED);
            (void)stg_pool_add(&p, pr.px + dx, pr.py + dy, 0, 0, 0u, fl);
            if (xs32_next(&g) % 16u == 0u)
                stg_pool_mark(&p, i);
        }
        for (i = 0u; i < n; i++) {
            const int64_t rb = r[s_mem[i].flags & 15u];
            const int64_t dx = (int64_t)s_mem[i].x - pr.px;
            const int64_t dy = (int64_t)s_mem[i].y - pr.py;
            const int64_t t = pr.r_graze + rb;
            const int64_t h = pr.r_hit + rb;

            s_ref[i] = s_mem[i];
            if (dx >= t || dx <= -t || dy >= t || dy <= -t)
                tot_cull++;
            if (dx * dx + dy * dy == h * h)
                tot_tangent++;
        }

        stg_pool_collide(&p, r, &pr, &st);
        ref_collide(s_ref, n, r, &pr, &rh, &rg);
        if (st.hits != rh || st.grazes != rg)
            mism++;
        for (i = 0u; i < n; i++) {
            if (s_mem[i].flags != s_ref[i].flags) {
                mism++;
                break;
            }
        }
        tot_hit += rh;
        tot_graze += rg;
    }

    {
        char     line[200];
        core_out o;

        core_out_init(&o, line, (uint32_t)sizeof line);
        core_out_str(&o, "X7 totals rounds 200 x 2000: hits ");
        core_out_u32(&o, tot_hit);
        core_out_str(&o, " grazes ");
        core_out_u32(&o, tot_graze);
        core_out_str(&o, " culled ");
        core_out_u32(&o, tot_cull);
        core_out_str(&o, " exact-tangent ");
        core_out_u32(&o, tot_tangent);
        core_out_str(&o, "\n");
        fputs(line, stdout);
    }
    ck("X7 200 rounds: hits, grazes and every flag == naive 64-bit reference", mism == 0u);
    ck("X7 coverage: hits, grazes, culled and exact-tangent cases all occurred",
       tot_hit > 0u && tot_graze > 0u && tot_cull > 0u && tot_tangent > 0u);
}

/* --- X8 场里的一段 ------------------------------------------------------- */

static void x8_field(void)
{
    uint32_t k, prev_hits = 0u, bad = 0u, sum_hits = 0u, sum_grazes = 0u;

    (void)stg_pattern_load(&s_field, 2u);       /* 自机狙：弹朝自机飞 */
    for (k = 0u; k < 240u; k++) {
        stg_field_step(&s_field);
        if (s_field.last.marked != prev_hits)
            bad++;
        prev_hits = s_field.frame_hit.hits;
        sum_hits += s_field.frame_hit.hits;
        sum_grazes += s_field.frame_hit.grazes;
    }
    ck("X8 aim segment 240 frames: marked removals each frame == previous frame's hits; totals add up",
       bad == 0u && sum_hits == s_field.hits && sum_grazes == s_field.grazes && s_field.hits > 0u);
    {
        char     line[120];
        core_out o;

        core_out_init(&o, line, (uint32_t)sizeof line);
        core_out_str(&o, "X8 aim 240 frames: hits ");
        core_out_u32(&o, s_field.hits);
        core_out_str(&o, " grazes ");
        core_out_u32(&o, s_field.grazes);
        core_out_str(&o, "\n");
        fputs(line, stdout);
    }
}

uint32_t test_stg_collide_run(void)
{
    g_fail = 0u;
    x1_boundary();
    x2_pythagoras();
    x3_far();
    x4_graze_once();
    x5_hits_off();
    x6_marked();
    x7_random();
    x8_field();
    return g_fail;
}
