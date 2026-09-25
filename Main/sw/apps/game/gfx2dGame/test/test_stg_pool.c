/******************************************************************************
* @file test_stg_pool.c: 实体池（SW7 2.2 节）的主机侧测试
*
* @brief  六组，判据见 Engineering/Todo/sw/SW7-游戏本体.md 第 3 节「池与回收」一行：
*           P1  容量与溢出：四个池的容量；池满时不追加、溢出计数加一、池不变
*           P2  随机 300 轮：每轮发射、打标记、更新一遍，池的全部内容与计数都等于
*               独立写的期望；每一遍移除的个数等于出界数加被标记数
*           P3  顺序：发射时按递增编号，更新之后存活实体的编号严格递增；另有一个手算的小例子
*           P4  出界边界的四条边（半开区间）与默认边界的取值
*           P5  标记：标记的实体即使在界内也移除、计为被标记；越界的下标不动；其余标志位保留
*           P6  画图的目的左上角 (x >> 8) - w/2，负坐标向负无穷取整
*
*         **期望由另一份写法给出**：ref_step() 先逐个判「活不活」写进一张表，再按表收集，
*         与产品代码「一遍里边判边写」不是同一段逻辑。两者都错成同一个样子才会漏。
*
*         注错开关（见 stg_pool.h 的文件头），每个都必须让这里至少一行 FAIL：
*           STG_POOL_STALL  -> P2 的内容比对与计数、P3 的小例子
*           STG_POOL_SWAP   -> P3 的顺序（计数仍然对：这正是只数个数的判据抓不到的错）
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

/* --- P1 容量与溢出 ------------------------------------------------------- */

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

/* --- P2/P3 随机轮次 ------------------------------------------------------ */

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

/* --- P4 出界边界 --------------------------------------------------------- */

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

/* --- P5 标记 ------------------------------------------------------------- */

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

/* --- P6 画图坐标 --------------------------------------------------------- */

static void p6_draw(void)
{
    ck("P6 draw origin (x>>8)-w/2: 0/16 -> -8, -1/16 -> -9, 100.99px/20 -> 90, 100.99px/6 -> 97",
       stg_draw_origin(0, 16) == -8
       && stg_draw_origin(-1, 16) == -9
       && stg_draw_origin(100 * FX_ONE + 255, 20) == 90
       && stg_draw_origin(100 * FX_ONE + 255, 6) == 97);
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

    return g_fail;
}
