/******************************************************************************
* @file test_stg_pool.c: 实体池的主机侧测试
*
* @brief  六组判据：
*           容量与溢出：四个池的容量；池满时不追加、溢出计数加一、池不变
*           随机 300 轮：每轮发射、打标记、更新一遍，池的全部内容与计数都等于
*               独立写的期望；每一遍移除的个数等于出界数加被标记数
*           顺序：发射时按递增编号，更新之后存活实体的编号严格递增；另有一个手算的小例子
*           出界边界的四条边（半开区间）与默认边界的取值
*           标记：标记的实体即使在界内也移除、计为被标记；越界的下标不动；其余标志位保留
*           画图坐标：目的左上角 (x >> 8) - w/2，负坐标向负无穷取整
*           危险度扫描（stg_pool_danger）：手算的几颗（d = 0 与 39、粗筛恰好 88 与 89 像素、
*               已标记跳过、外推 12 帧）；随机 300 个池（含远处的、已标记的、贴着粗筛边界的）对 64 位、
*               不粗筛的朴素参考逐项相同
*
*         **期望由另一份写法给出**：ref_step() 先逐个判「活不活」写进一张表，再按表收集，
*         与产品代码「一遍里边判边写」不是同一段逻辑。两者都错成同一个样子才会漏。
*
*         注错开关（见 stg_pool.h 的文件头），每个都必须让这里至少一行 FAIL：
*           STG_POOL_STALL  -> 随机轮次的内容比对与计数、顺序的小例子
*           STG_POOL_SWAP   -> 顺序判据（计数仍然对：这正是只数个数的判据抓不到的错）
******************************************************************************/

#include <stdio.h>
#include <string.h>

#include "core/format.h"
#include "core/xorshift32.h"
#include "game/stg_pool.h"
#include "test_stg_pool.h"

static uint32_t g_fail;

static void say(const char *s)
{
    fputs(s, stdout);
}

static void ck(const char *tag, int ok)
{
    char     line[160];
    core_out o;

    core_out_init(&o, line, (uint32_t)sizeof line);
    core_out_str(&o, tag);
    core_out_str(&o, ok ? " ok\n" : " FAIL\n");
    say(line);
    if (!ok)
        g_fail++;
}

static void out_i32(core_out *o, int32_t v)
{
    if (v < 0) {
        core_out_ch(o, '-');
        core_out_u32(o, (uint32_t)(-(int64_t)v));
    } else {
        core_out_u32(o, (uint32_t)v);
    }
}

/* 四个池加在一起约 375 KB，放静态区，不放栈上。 */
static stg_pools s_pools;

/* --- 独立写的期望 -------------------------------------------------------- */

/* 先判活不活，再收集。返回存活数。 */
static uint32_t ref_step(const stg_ent *in, uint32_t n, const stg_bounds *b,
                         stg_ent *out, uint32_t *oob, uint32_t *marked)
{
    static uint8_t alive[STG_CAP_EBULLET];
    uint32_t       i, k = 0u;

    *oob = 0u;
    *marked = 0u;
    for (i = 0u; i < n; i++) {
        int32_t nx = in[i].x + in[i].vx;
        int32_t ny = in[i].y + in[i].vy;
        int     inside = (nx >= b->x_lo) && (nx < b->x_hi) && (ny >= b->y_lo) && (ny < b->y_hi);

        if (in[i].flags & STG_F_REMOVE) {
            alive[i] = 0u;
            (*marked)++;
        } else if (!inside) {
            alive[i] = 0u;
            (*oob)++;
        } else {
            alive[i] = 1u;
        }
    }
    for (i = 0u; i < n; i++) {
        if (!alive[i])
            continue;
        out[k] = in[i];
        out[k].x += in[i].vx;
        out[k].y += in[i].vy;
        k++;
    }
    return k;
}

static int ent_eq(const stg_ent *a, const stg_ent *b)
{
    return a->x == b->x && a->y == b->y && a->vx == b->vx && a->vy == b->vy
        && a->sprite == b->sprite && a->flags == b->flags;
}

/* 编号放在 sprite 里：发射时递增，所以存活实体的编号必须严格递增。 */
static int ids_increasing(const stg_pool *p)
{
    uint32_t i;
    stg_ent  a, b;

    for (i = 1u; i < stg_pool_count(p); i++) {
        (void)stg_pool_get(p, i - 1u, &a);
        (void)stg_pool_get(p, i, &b);
        if (a.sprite >= b.sprite)
            return 0;
    }
    return 1;
}

/* --- 容量与溢出 ----------------------------------------------------------- */

static void p1_capacity(void)
{
    uint32_t i, rej = 0u;
    stg_ent  e;

    stg_pools_init(&s_pools);
    ck("P1 caps ebullet 16384 pbullet 256 enemy 64 particle 2048",
       s_pools.ebullet.cap == 16384u && s_pools.pbullet.cap == 256u
       && s_pools.enemy.cap == 64u && s_pools.particle.cap == 2048u);
    ck("P1 all pools start empty",
       stg_pool_count(&s_pools.ebullet) == 0u && stg_pool_count(&s_pools.pbullet) == 0u
       && stg_pool_count(&s_pools.enemy) == 0u && stg_pool_count(&s_pools.particle) == 0u);

    for (i = 0u; i < STG_CAP_EBULLET + 2u; i++) {
        if (stg_pool_add(&s_pools.ebullet, (int32_t)i, 0, 0, 0, (uint16_t)i, 0u) != 0)
            rej++;
    }
    ck("P1 ebullet full: 16384 in, 2 rejected, overflow 2",
       stg_pool_count(&s_pools.ebullet) == STG_CAP_EBULLET && rej == 2u
       && stg_pool_overflow(&s_pools.ebullet) == 2u);
    ck("P1 rejected add leaves last entity intact",
       stg_pool_get(&s_pools.ebullet, STG_CAP_EBULLET - 1u, &e) == 0
       && e.x == (int32_t)(STG_CAP_EBULLET - 1u));
    ck("P1 get past end refused", stg_pool_get(&s_pools.ebullet, STG_CAP_EBULLET, &e) == -1);

    for (i = 0u; i < STG_CAP_ENEMY; i++)
        (void)stg_pool_add(&s_pools.enemy, 0, 0, 0, 0, 0u, 0u);
    ck("P1 enemy full then add -> -1, overflow 1",
       stg_pool_add(&s_pools.enemy, 0, 0, 0, 0, 0u, 0u) == -1
       && stg_pool_overflow(&s_pools.enemy) == 1u && stg_pool_count(&s_pools.enemy) == STG_CAP_ENEMY);
    ck("P1 other pools untouched",
       stg_pool_overflow(&s_pools.pbullet) == 0u && stg_pool_overflow(&s_pools.particle) == 0u);
}

/* --- 随机轮次与顺序 ------------------------------------------------------- */

#define P2_CAP      1024u
#define P2_ROUNDS   300u

static int32_t rnd_range(xs32 *g, int32_t lo, int32_t hi)     /* [lo, hi] */
{
    return lo + (int32_t)(xs32_next(g) % (uint32_t)(hi - lo + 1));
}

static void p2_random(void)
{
    static stg_ent mem[P2_CAP];
    static stg_ent before[P2_CAP];
    static stg_ent want[P2_CAP];
    stg_pool       p;
    stg_bounds     b;
    xs32           g;
    uint32_t       round, next_id = 1u, want_overflow = 0u;
    uint32_t       bad_state = 0u, bad_count = 0u, bad_sum = 0u, bad_order = 0u;
    uint32_t       tot_oob = 0u, tot_marked = 0u, tot_alive = 0u, tot_added = 0u;

    stg_pool_init(&p, mem, P2_CAP);
    stg_bounds_field(&b);
    xs32_seed(&g, 0x57A70001u);

    for (round = 0u; round < P2_ROUNDS; round++) {
        uint32_t       adds = xs32_next(&g) % 160u;
        uint32_t       i, n0, nw, oob, marked;
        stg_step_stats st;

        for (i = 0u; i < adds; i++) {
            /* 位置落在边界内外各一些，速度在 ±6 像素/帧；少数速度为 0 */
            int32_t x  = rnd_range(&g, -16 * FX_ONE, (STG_FIELD_W + 16) * FX_ONE);
            int32_t y  = rnd_range(&g, -16 * FX_ONE, (STG_FIELD_H + 16) * FX_ONE);
            int32_t vx = (xs32_next(&g) % 8u == 0u) ? 0 : rnd_range(&g, -6 * FX_ONE, 6 * FX_ONE);
            int32_t vy = rnd_range(&g, -6 * FX_ONE, 6 * FX_ONE);
            uint16_t fl = (uint16_t)(xs32_next(&g) & (STG_F_KIND_MASK | STG_F_GRAZED | STG_F_NOCANCEL));

            if (stg_pool_add(&p, x, y, vx, vy, (uint16_t)next_id, fl) == 0) {
                next_id++;
                tot_added++;
            } else {
                want_overflow++;
            }
        }

        /* 约 6% 打标记：碰撞与炸弹只打标志 */
        n0 = stg_pool_count(&p);
        for (i = 0u; i < n0; i++)
            if (xs32_next(&g) % 16u == 0u)
                stg_pool_mark(&p, i);

        for (i = 0u; i < n0; i++)
            (void)stg_pool_get(&p, i, &before[i]);
        nw = ref_step(before, n0, &b, want, &oob, &marked);

        stg_pool_step(&p, &b, &st);

        if (stg_pool_count(&p) != nw) {
            bad_state++;
        } else {
            for (i = 0u; i < nw; i++) {
                stg_ent e;

                (void)stg_pool_get(&p, i, &e);
                if (!ent_eq(&e, &want[i])) {
                    bad_state++;
                    break;
                }
            }
        }
        if (st.oob != oob || st.marked != marked)
            bad_count++;
        if (n0 - stg_pool_count(&p) != st.oob + st.marked)
            bad_sum++;
        if (!ids_increasing(&p))
            bad_order++;

        tot_oob += st.oob;
        tot_marked += st.marked;
        tot_alive += stg_pool_count(&p);
    }

    ck("P2 300 rounds: pool content == independent expectation", bad_state == 0u);
    ck("P2 300 rounds: oob/marked counters == expectation", bad_count == 0u);
    ck("P2 300 rounds: removed == oob + marked", bad_sum == 0u);
    ck("P2 overflow == rejected adds", stg_pool_overflow(&p) == want_overflow);
    ck("P3 300 rounds: survivors keep relative order", bad_order == 0u);
    {
        char     line[160];
        core_out o;

        core_out_init(&o, line, (uint32_t)sizeof line);
        core_out_str(&o, "P2 totals added ");
        core_out_u32(&o, tot_added);
        core_out_str(&o, " oob ");
        core_out_u32(&o, tot_oob);
        core_out_str(&o, " marked ");
        core_out_u32(&o, tot_marked);
        core_out_str(&o, " overflow ");
        core_out_u32(&o, want_overflow);
        core_out_str(&o, " alive-sum ");
        core_out_u32(&o, tot_alive);
        core_out_str(&o, "\n");
        say(line);
        /* 覆盖面：三种去向都要真的发生过，否则上面几条可能是空转 */
        ck("P2 coverage: oob, marked and overflow all > 0",
           tot_oob > 0u && tot_marked > 0u && want_overflow > 0u);
    }
}

/* 手算的小例子：A B C D E，B 已标记，D 这一步出界。期望 A C E，计数 oob 1、marked 1。 */
static void p3_small(void)
{
    static stg_ent mem[8];
    stg_pool       p;
    stg_bounds     b;
    stg_step_stats st;
    stg_ent        e0, e1, e2;
    int            ok;

    stg_pool_init(&p, mem, 8u);
    stg_bounds_field(&b);
    (void)stg_pool_add(&p, 100 * FX_ONE, 100 * FX_ONE, 0, FX_ONE, 'A', 0u);
    (void)stg_pool_add(&p, 110 * FX_ONE, 100 * FX_ONE, 0, FX_ONE, 'B', 0u);
    (void)stg_pool_add(&p, 120 * FX_ONE, 100 * FX_ONE, 0, FX_ONE, 'C', 0u);
    (void)stg_pool_add(&p, 130 * FX_ONE, (STG_FIELD_H + STG_MARGIN_PX) * FX_ONE - 1, 0, 1, 'D', 0u);
    (void)stg_pool_add(&p, 140 * FX_ONE, 100 * FX_ONE, 0, FX_ONE, 'E', 0u);
    stg_pool_mark(&p, 1u);
    stg_pool_step(&p, &b, &st);

    ok = stg_pool_count(&p) == 3u && st.oob == 1u && st.marked == 1u
      && stg_pool_get(&p, 0u, &e0) == 0 && stg_pool_get(&p, 1u, &e1) == 0
      && stg_pool_get(&p, 2u, &e2) == 0
      && e0.sprite == 'A' && e1.sprite == 'C' && e2.sprite == 'E'
      && e0.y == 101 * FX_ONE && e2.y == 101 * FX_ONE;
    ck("P3 small: [A B* C D> E] -> [A C E], oob 1 marked 1", ok);
}

/* --- 出界边界 ------------------------------------------------------------- */

static int survives(int32_t x, int32_t y)
{
    static stg_ent mem[1];
    stg_pool       p;
    stg_bounds     b;

    stg_pool_init(&p, mem, 1u);
    stg_bounds_field(&b);
    (void)stg_pool_add(&p, x, y, 0, 0, 0u, 0u);
    stg_pool_step(&p, &b, 0);
    return stg_pool_count(&p) == 1u;
}

static void p4_bounds(void)
{
    stg_bounds b;
    int32_t    xl = -STG_MARGIN_PX * FX_ONE;
    int32_t    xh = (STG_FIELD_W + STG_MARGIN_PX) * FX_ONE;
    int32_t    yl = -STG_MARGIN_PX * FX_ONE;
    int32_t    yh = (STG_FIELD_H + STG_MARGIN_PX) * FX_ONE;
    char       line[160];
    core_out   o;

    stg_bounds_field(&b);
    core_out_init(&o, line, (uint32_t)sizeof line);
    core_out_str(&o, "P4 field bounds q8 x [");
    out_i32(&o, b.x_lo);
    core_out_str(&o, ", ");
    out_i32(&o, b.x_hi);
    core_out_str(&o, ") y [");
    out_i32(&o, b.y_lo);
    core_out_str(&o, ", ");
    out_i32(&o, b.y_hi);
    core_out_str(&o, ")");
    ck(line, b.x_lo == -2560 && b.x_hi == 104960 && b.y_lo == -2560 && b.y_hi == 125440);

    ck("P4 x_lo inclusive, x_lo-1 removed", survives(xl, 0) && !survives(xl - 1, 0));
    ck("P4 x_hi-1 kept, x_hi removed", survives(xh - 1, 0) && !survives(xh, 0));
    ck("P4 y_lo inclusive, y_lo-1 removed", survives(0, yl) && !survives(0, yl - 1));
    ck("P4 y_hi-1 kept, y_hi removed", survives(0, yh - 1) && !survives(0, yh));
}

/* --- 标记 ----------------------------------------------------------------- */

static void p5_mark(void)
{
    static stg_ent mem[4];
    stg_pool       p;
    stg_bounds     b;
    stg_step_stats st;
    stg_ent        e;
    uint16_t       keep = (uint16_t)(0x5u | STG_F_GRAZED | STG_F_NOCANCEL);

    stg_bounds_field(&b);

    /* 在界内也移除，计为被标记；又标记又出界的，计为被标记（先看标记） */
    stg_pool_init(&p, mem, 4u);
    (void)stg_pool_add(&p, 200 * FX_ONE, 200 * FX_ONE, 0, 0, 1u, 0u);
    (void)stg_pool_add(&p, -100 * FX_ONE, 200 * FX_ONE, 0, 0, 2u, 0u);
    stg_pool_mark(&p, 0u);
    stg_pool_mark(&p, 1u);
    stg_pool_step(&p, &b, &st);
    ck("P5 marked in-bounds and marked out-of-bounds both count as marked",
       stg_pool_count(&p) == 0u && st.marked == 2u && st.oob == 0u);

    /* 下标越界不动任何东西 */
    stg_pool_init(&p, mem, 4u);
    (void)stg_pool_add(&p, 200 * FX_ONE, 200 * FX_ONE, 0, 0, 1u, 0u);
    stg_pool_mark(&p, 1u);
    stg_pool_mark(&p, 100u);
    stg_pool_step(&p, &b, &st);
    ck("P5 mark past end ignored", stg_pool_count(&p) == 1u && st.marked == 0u);

    /* 其余标志位跟着实体走；发射时带进来的 REMOVE 位不收 */
    stg_pool_init(&p, mem, 4u);
    (void)stg_pool_add(&p, 200 * FX_ONE, 200 * FX_ONE, FX_ONE, 0, 1u,
                       (uint16_t)(keep | STG_F_REMOVE));
    stg_pool_step(&p, &b, &st);
    ck("P5 add strips REMOVE, other flags survive step",
       stg_pool_count(&p) == 1u && stg_pool_get(&p, 0u, &e) == 0
       && e.flags == keep && e.x == 201 * FX_ONE && st.marked == 0u);
}

/* --- 画图坐标 ------------------------------------------------------------- */

static void p6_draw(void)
{
    ck("P6 draw origin (x>>8)-w/2: 0/16 -> -8, -1/16 -> -9, 100.99px/20 -> 90, 100.99px/6 -> 97",
       stg_draw_origin(0, 16) == -8
       && stg_draw_origin(-1, 16) == -9
       && stg_draw_origin(100 * FX_ONE + 255, 20) == 90
       && stg_draw_origin(100 * FX_ONE + 255, 6) == 97);
}

/* --- 危险度扫描 ----------------------------------------------------------- */

#define DG_LOOK     12
#define DG_R        40
#define DG_SPREAD   48
#define DG_REACH    (DG_SPREAD + DG_R)

/* 朴素参考：不粗筛，64 位 */
static void ref_danger(const stg_pool *p, const stg_danger_q *q, uint64_t *risk)
{
    uint32_t i, j;

    for (i = 0u; i < p->n; i++) {
        stg_ent e;
        int64_t bx, by;

        (void)stg_pool_get(p, i, &e);
        if ((e.flags & STG_F_REMOVE) != 0u)
            continue;
        bx = ((int64_t)e.x + (int64_t)q->look * e.vx) >> 8;
        by = ((int64_t)e.y + (int64_t)q->look * e.vy) >> 8;
        for (j = 0u; j < q->n; j++) {
            const int64_t dx = bx - q->cx[j], dy = by - q->cy[j];
            const int64_t d2 = dx * dx + dy * dy;
            const int64_t r2 = (int64_t)q->r * q->r;

            if (d2 < r2)
                risk[j] += (uint64_t)(r2 - d2);
        }
    }
}

static int32_t rnd(xs32 *r, int32_t lo, int32_t hi)
{
    return lo + (int32_t)(xs32_next(r) % (uint32_t)(hi - lo + 1));
}

static void dg_query(stg_danger_q *q, int32_t px, int32_t py)
{
    q->look  = DG_LOOK;
    q->px    = px;
    q->py    = py;
    q->reach = DG_REACH;
    q->r     = DG_R;
    q->n     = 0u;
}

static void p7_danger(void)
{
    stg_pool    *p = &s_pools.ebullet;
    stg_danger_q q;
    uint32_t     risk[STG_DANGER_MAX];
    uint64_t     ref[STG_DANGER_MAX];
    xs32         r;
    uint32_t     c, i, bad = 0u, near = 0u, edge = 0u;
    int          ok;

    /* 手算：自机 (200, 300)；候选 0 在自机上、候选 1 在 (260, 300)（只为看粗筛，比实际的散布远）、候选 2 在 (200, 339) */
    stg_pool_init(p, s_pools.ebullet_mem, STG_CAP_EBULLET);
    dg_query(&q, 200, 300);
    q.n = 3u;
    q.cx[0] = 200; q.cy[0] = 300;
    q.cx[1] = 260; q.cy[1] = 300;
    q.cx[2] = 200; q.cy[2] = 339;
    /* 12 帧后正好到 (200, 300)：候选 0 加 1600，候选 2 在 39 处加 1600 - 1521 = 79 */
    (void)stg_pool_add(p, 200 * FX_ONE, (300 - 36) * FX_ONE, 0, 3 * FX_ONE, 0u, 0u);
    /* 停在 (288, 300)：离自机恰好 88，不被粗筛掉；离候选 1 是 28，加 1600 - 784 = 816 */
    (void)stg_pool_add(p, 288 * FX_ONE, 300 * FX_ONE, 0, 0, 0u, 0u);
    /* 停在 (289, 300)：离自机 89，被粗筛掉（不筛的话候选 1 会加 759） */
    (void)stg_pool_add(p, 289 * FX_ONE, 300 * FX_ONE, 0, 0, 0u, 0u);
    /* 停在 (247, 301)：离候选 1 的 d² = 170，加 1430；同位置再放一颗并标记移除，跳过 */
    (void)stg_pool_add(p, 247 * FX_ONE, 301 * FX_ONE, 0, 0, 0u, 0u);
    (void)stg_pool_add(p, 247 * FX_ONE, 301 * FX_ONE, 0, 0, 0u, 0u);
    stg_pool_mark(p, 4u);
    for (i = 0u; i < 3u; i++)
        risk[i] = 0u;
    stg_pool_danger(p, &q, risk);
    ok = (risk[0] == 1600u && risk[1] == 816u + 1430u && risk[2] == 79u);
    ck("P7 danger by hand: d=0 -> 1600, d=39 -> 79, box edge 88 in / 89 out, marked skipped", ok);

    xs32_seed(&r, 0xA070D00Du);
    for (c = 0u; c < 300u; c++) {
        const uint32_t n = 1u + xs32_next(&r) % 400u;

        stg_pool_init(p, s_pools.ebullet_mem, STG_CAP_EBULLET);
        dg_query(&q, rnd(&r, 16, 384), rnd(&r, 16, 464));
        q.n = STG_DANGER_MAX;
        for (i = 0u; i < q.n; i++) {
            q.cx[i] = q.px + rnd(&r, -DG_SPREAD, DG_SPREAD);
            q.cy[i] = q.py + rnd(&r, -DG_SPREAD, DG_SPREAD);
            risk[i] = 0u;
            ref[i]  = 0u;
        }
        for (i = 0u; i < n; i++) {
            /* 一半外推之后落在自机 ±100 像素里，一半在全场；速度 ±6 像素/帧 */
            const int32_t vx = rnd(&r, -1536, 1536), vy = rnd(&r, -1536, 1536);
            int32_t       ex, ey, bx, by, ax, ay;

            if (i & 1u) {
                ex = (q.px + rnd(&r, -100, 100)) * FX_ONE + rnd(&r, 0, 255) - DG_LOOK * vx;
                ey = (q.py + rnd(&r, -100, 100)) * FX_ONE + rnd(&r, 0, 255) - DG_LOOK * vy;
            } else {
                ex = rnd(&r, -10 * FX_ONE, 410 * FX_ONE);
                ey = rnd(&r, -10 * FX_ONE, 490 * FX_ONE);
            }
            (void)stg_pool_add(p, ex, ey, vx, vy, 0u, (uint16_t)(xs32_next(&r) & 0xFu));
            if ((xs32_next(&r) & 7u) == 0u)
                stg_pool_mark(p, p->n - 1u);
            bx = fx_px(ex + DG_LOOK * vx);
            by = fx_px(ey + DG_LOOK * vy);
            ax = (bx > q.px) ? bx - q.px : q.px - bx;
            ay = (by > q.py) ? by - q.py : q.py - by;
            near += (ax <= DG_REACH && ay <= DG_REACH);
            edge += (ax == DG_REACH || ay == DG_REACH || ax == DG_REACH + 1 || ay == DG_REACH + 1);
        }
        stg_pool_danger(p, &q, risk);
        ref_danger(p, &q, ref);
        for (i = 0u; i < q.n; i++)
            bad += ((uint64_t)risk[i] != ref[i]);
    }
    {
        char     line[160];
        core_out o;

        core_out_init(&o, line, (uint32_t)sizeof line);
        core_out_str(&o, "P7 danger vs 64-bit naive reference: 300 pools, ");
        core_out_u32(&o, near);
        core_out_str(&o, " bullets inside the box, ");
        core_out_u32(&o, edge);
        core_out_str(&o, " on its edge, mismatches ");
        core_out_u32(&o, bad);
        ck(line, bad == 0u && edge > 0u);
    }
    stg_pool_init(p, s_pools.ebullet_mem, STG_CAP_EBULLET);
}

uint32_t test_stg_pool_run(void)
{
    g_fail = 0u;

    memset(&s_pools, 0, sizeof s_pools);
    p1_capacity();
    p2_random();
    p3_small();
    p4_bounds();
    p5_mark();
    p6_draw();
    p7_danger();

    return g_fail;
}
