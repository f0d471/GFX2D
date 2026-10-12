/******************************************************************************
* @file test_stg_auto.c: 自动驾驶的主机侧测试
*
* @brief  A 组：
*           A1  接管：连续 600 帧无按键时仍用人的按键（全 0），第 601 帧起接管；任何一帧有按键立即交还、
*               这一帧用人的按键，之后要再满 600 帧无按键才重新接管
*           A2  偏好与平局：池空、自机在 (300, 400) 时往左；自机在 (200, 400)、正中停着一颗弹时，
*               上、下、左、右四个候选同分，取固定顺序里的第一个（上）
*           A3  躲弹：一颗弹正对自机飞来，离开原地、选中的候选总分比不动低；九个候选都有危险时取总分最小的
*           A4  打一局：受击打开、从第 0 帧起自动驾驶，跑到回标题（最多 20000 帧），两遍的逐帧状态散列相同；
*               打出活到哪一段、共几帧、扣了几个残机与 HI-SCORE
*
*         危险度扫描本身（stg_pool_danger）对朴素参考的比对在 test_stg_pool.c
*         的「danger vs 64-bit naive reference」一组。
*         注错开关：AUTO_ORDER（候选顺序倒过来）必须让 A2 的平局那一行见红。
******************************************************************************/

#include <stdint.h>
#include <stdio.h>

#include "core/format.h"
#include "game/stg_auto.h"
#include "game/stg_game.h"
#include "game/stg_pool.h"
#include "test_stg_auto.h"

#define PLAY_LIMIT  20000u

static uint32_t g_fail;

/* 游戏状态约 390 KB，放静态区。 */
static stg_game s_g;
static uint32_t s_hash[PLAY_LIMIT];

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

/* A1 */
static void t_take(void)
{
    uint32_t idle = 0u, i, n_auto = 0u;
    int      ok = 1;

    for (i = 0u; i < 599u; i++)
        n_auto += (uint32_t)stg_auto_take(&idle, 0u);
    ok &= (n_auto == 0u);
    ok &= (stg_auto_take(&idle, 0u) == 0);           /* 第 600 帧：还是人的（无按键） */
    ok &= (stg_auto_take(&idle, 0u) == 1);           /* 连续 600 帧无按键之后接管 */
    for (i = 0u; i < 100u; i++)
        ok &= (stg_auto_take(&idle, 0u) == 1);
    ck("A1 600 idle frames then take over, stays on while idle", ok);

    ok = (stg_auto_take(&idle, STG_KEY_BOMB) == 0);   /* 有按键：立即交还 */
    n_auto = 0u;
    for (i = 0u; i < 600u; i++)
        n_auto += (uint32_t)stg_auto_take(&idle, 0u);
    ok &= (n_auto == 0u) && (stg_auto_take(&idle, 0u) == 1);
    ok &= (stg_auto_take(&idle, STG_KEY_LEFT) == 0) && (stg_auto_take(&idle, 0u) == 0);
    ck("A1 any key hands back at once; another 600 idle frames before taking over again", ok);
}

static void place(stg_game *g, int32_t x_px, int32_t y_px)
{
    stg_game_init(g, STG_GAME_SEED);
    g->f.px = x_px * FX_ONE;
    g->f.py = y_px * FX_ONE;
}

/* A2 */
static void t_pref(void)
{
    uint32_t risk[STG_AUTO_N];
    uint32_t k;

    place(&s_g, 300, 400);
    k = stg_auto_keys(&s_g, risk);
    ck("A2 empty field at (300, 400): move left toward x = 200", k == STG_KEY_LEFT);

    place(&s_g, 200, 400);
    k = stg_auto_keys(&s_g, risk);
    ck("A2 empty field at (200, 400): stay", k == 0u && risk[0] == 0u);

    /* 正中停着一颗弹：不动的候选危险，上下左右离它 48 像素、偏好都是 96，四个同分 */
    (void)stg_pool_add(&s_g.f.pools.ebullet, 200 * FX_ONE, 400 * FX_ONE, 0, 0, 0u, 0u);
    k = stg_auto_keys(&s_g, risk);
    ck("A2 tie among up/down/left/right: the first in the fixed order (up)",
       k == STG_KEY_UP && risk[1] == risk[2] && risk[2] == risk[3] && risk[3] == risk[4]
       && risk[1] < risk[0]);
}

/* A3 */
static void t_dodge(void)
{
    uint32_t risk[STG_AUTO_N];
    uint32_t k, i, best;
    int      ok;

    /* 一颗弹从上方 60 像素处以每帧 5 像素正对自机飞来：12 帧后正好到自机的位置 */
    place(&s_g, 200, 400);
    (void)stg_pool_add(&s_g.f.pools.ebullet, 200 * FX_ONE, 340 * FX_ONE, 0, 5 * FX_ONE, 0u, 0u);
    k = stg_auto_keys(&s_g, risk);
    ok = (k != 0u) && (risk[0] >= 1600u);
    for (i = 0u; i < STG_AUTO_N; i++)
        if (stg_auto_cand_keys(i) == k)
            ok &= (risk[i] < risk[0]);
    ck("A3 a bullet aimed at the player: leaves the spot, the chosen candidate scores lower than staying", ok);

    /* 九个候选都压着弹：取总分最小的 */
    place(&s_g, 200, 300);
    for (i = 0u; i < 40u; i++)
        (void)stg_pool_add(&s_g.f.pools.ebullet, (160 + (int32_t)(i % 8u) * 11) * FX_ONE,
                           (260 + (int32_t)(i / 8u) * 17) * FX_ONE, 0, 0, 0u, 0u);
    k = stg_auto_keys(&s_g, risk);
    best = 0u;
    for (i = 1u; i < STG_AUTO_N; i++)
        if (risk[i] < risk[best])
            best = i;
    ok = (k == stg_auto_cand_keys(best));
    for (i = 0u; i < STG_AUTO_N; i++)
        ok &= (risk[i] > 0u);
    ck("A3 every candidate in danger: the lowest total wins", ok);
}

static uint32_t mix(uint32_t h, uint32_t v)
{
    return (h ^ v) * 16777619u;
}

static uint32_t state_hash(const stg_game *g)
{
    uint32_t h = 2166136261u;

    h = mix(h, (uint32_t)g->f.px);
    h = mix(h, (uint32_t)g->f.py);
    h = mix(h, g->score);
    h = mix(h, g->lives);
    h = mix(h, g->bombs);
    h = mix(h, g->graze);
    h = mix(h, g->seg);
    h = mix(h, stg_pool_count(&g->f.pools.ebullet));
    h = mix(h, stg_pool_count(&g->f.pools.pbullet));
    h = mix(h, stg_pool_count(&g->f.pools.enemy));
    return h;
}

/* 一局：返回走了几帧；seg_max 是到过的最后一段，lost 是扣掉的残机数 */
static uint32_t play(int check, uint32_t *seg_max, uint32_t *lost, uint32_t *diff)
{
    uint32_t f;

    stg_game_init(&s_g, STG_GAME_SEED);
    *seg_max = 0u;
    *lost    = 0u;
    *diff    = 0u;
    for (f = 0u; f < PLAY_LIMIT; f++) {
        stg_game_step(&s_g, stg_auto_keys(&s_g, NULL));
        if (check)
            *diff += (s_hash[f] != state_hash(&s_g));
        else
            s_hash[f] = state_hash(&s_g);
        if (s_g.seg > *seg_max && s_g.seg != STG_GSEG_TITLE)
            *seg_max = s_g.seg;
        *lost += ((s_g.ev & STG_EV_LOST) != 0u);
        if ((s_g.ev & STG_EV_SEG) != 0u && s_g.seg == STG_GSEG_TITLE)
            return f + 1u;
    }
    return f;
}

/* A4 */
static void t_game(void)
{
    uint32_t n1, n2, seg1, seg2, lost1, lost2, d1, d2, hi;
    char     line[200];
    core_out o;

    n1    = play(0, &seg1, &lost1, &d1);
    hi    = s_g.hiscore;           /* 局末回标题时得分已清零，取 HI-SCORE */
    n2    = play(1, &seg2, &lost2, &d2);
    core_out_init(&o, line, (uint32_t)sizeof line);
    core_out_str(&o, "A4 autopilot plays one round twice: ");
    core_out_u32(&o, n1);
    core_out_str(&o, " frames, reached ");
    core_out_str(&o, stg_game_seg_name(seg1));
    core_out_str(&o, ", lives lost ");
    core_out_u32(&o, lost1);
    core_out_str(&o, ", hi-score ");
    core_out_u32(&o, hi);
    core_out_str(&o, ", identical");
    ck(line, n1 == n2 && seg1 == seg2 && lost1 == lost2 && d2 == 0u && n1 < PLAY_LIMIT);
}

uint32_t test_stg_auto_run(void)
{
    g_fail = 0u;
    t_take();
    t_pref();
    t_dodge();
    t_game();
    return g_fail;
}
