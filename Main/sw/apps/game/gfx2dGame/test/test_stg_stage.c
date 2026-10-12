/******************************************************************************
* @file test_stg_stage.c: 敌机、Boss 与关卡的主机侧测试
*
* @brief  S 组。期望值在测试里按关卡的常量另算，
*         不读 stg_stage.c 的表（S1 与出敌帧的核对除外：那两条查的就是表本身怎么被执行）：
*           S1  表：事件按帧号排序、最后一条是进 Boss 且只有一条、编号都在范围内；符卡四张、发射记录都合法，
*               终符的最大一次齐射不超过 24（令牌上限 48 是两次最大齐射）
*           S2  移动事件表：进入一步时加增量、走完最后一步保持速度；发射器第一次齐射推迟 delay 帧、
*               在移动之前的位置出生；出界用 32 像素外扩，边界是半开区间
*           S3  槽位：取第一个空槽，64 个满了拒绝并计数，空出来的槽再用
*           S4  击毁：扣血、得分、按机种计数、8 个火花；游戏里一架 DART 停在自机弹的正上方，
*               按射击节奏另算出第 20 帧击毁
*           S5  受击关掉、自机不射击，跑完一整局：各段（标题、杂兵段、Boss 进场、四张符卡、CLEAR、回标题）
*               的开始帧等于按 stg_stage.h 常量另算的值；出敌的帧就是事件表的帧号加 120
*           S6  受击关掉、自机照常射击，也走到 CLEAR：各段开始帧登记；得分等于测试另算的
*               10 × 擦弹 + 100 × DART + 300 × CROSS + 符卡奖励
*           S7  Boss：进场 74 帧从 (200, -48) 到 (200, 100)，之后按 (200 + 70 sin a, 100 + 12 sin 2a) 游动，
*               a 每帧加 91；进场与间隙里不受伤（目标存活为 0、血量不变）
*           S8  压力形态：Boss 停在 (200, 100)、不受伤、符卡不结束，自机停在出生点、不射击、不认按键，
*               终符的敌弹全部换成给定的弹种
*           S9  门控的旋钮加倍（200、400、800、1600、3200）时稳态敌弹数不减
*           S10 压力形态下改档之后，改档之前出生的弹全部出界所需的帧数不超过 SETTLE_MIN = 300
*           S11 游戏里的门控：K = 600 + (t * 3) >> 2；累计发出的弹数加剩余令牌等于 floor(sum K / 125)
*           S12 背景：横线下移 230/256 像素每帧，满 32 像素亮线相位加一；星点在范围内
******************************************************************************/

#include <stdint.h>
#include <stdio.h>

#include "core/fixmath.h"
#include "core/format.h"
#include "core/xorshift32.h"
#include "game/stg_enemy.h"
#include "game/stg_game.h"
#include "game/stg_kind.h"
#include "game/stg_pool.h"
#include "game/stg_stage.h"
#include "test_stg_stage.h"

/* 2.5 节的常量（测试另抄一份） */
#define C_TITLE     120u
#define C_BOSS_T    1740u       /* 进 Boss 的事件帧号（开局第 0 帧起） */
#define C_ENTER     74u
#define C_GAP       90u
#define C_CLEAR     180u
static const uint32_t c_timeout[4] = { 1800u, 1800u, 1800u, 2400u };

#define SETTLE_MIN  300u        /* sw 第 07 章 3.8 节 */
#define TAG         0x0100u     /* S10：给改档之前出生的弹打的记号（flags 的保留位） */

static uint32_t g_fail;

static stg_game    s_g;
static stg_enemies s_en;
static stg_ent     s_mem[4096];

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
    char     line[240];
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

/* --- S1 表 ----------------------------------------------------------------- */

static void s1_tables(void)
{
    stg_emitter em;
    uint32_t    i, j, n_boss = 0u, max_gated = 0u;
    int         ok = 1;

    for (i = 0u; i < stg_stage_n_events; i++) {
        const stg_event *e = &stg_stage_events[i];

        if (i > 0u && e->t < stg_stage_events[i - 1u].t)
            ok = 0;
        if (e->type == STG_EVT_BOSS)
            n_boss++;
        if (e->type == STG_EVT_SPAWN
            && (e->etype >= STG_ETYPE_N || e->move >= stg_n_moves || e->group >= stg_n_em_groups))
            ok = 0;
    }
    ok &= (n_boss == 1u && stg_stage_events[stg_stage_n_events - 1u].type == STG_EVT_BOSS);
    for (i = 0u; i < stg_n_moves; i++)
        for (j = 0u; j < stg_moves[i].n; j++)
            ok &= (stg_moves[i].steps[j].frames != 0u);
    for (i = 0u; i < stg_n_em_groups; i++) {
        ok &= (stg_em_groups[i].n >= 1u && stg_em_groups[i].n <= STG_ENEMY_EM_MAX);
        for (j = 0u; j < stg_em_groups[i].n; j++)
            ok &= (stg_emitter_init(&em, stg_em_groups[i].recs[j], 0, 0) == 0);
    }
    ck("S1 events sorted, one BOSS as the last entry, indices in range, emitter records valid", ok);

    ok = (stg_cards[0].gap == 0u);
    for (i = 0u; i < STG_CARD_N; i++) {
        const stg_card *c = &stg_cards[i];

        ok &= (c->n_em >= 1u && c->n_em <= STG_CARD_EM_MAX && c->hp > 0 && c->timeout == c_timeout[i]);
        ok &= (i == 0u || c->gap == C_GAP);
        ok &= (c->gated == ((i == STG_CARD_N - 1u) ? 1u : 0u));
        for (j = 0u; j < c->n_em; j++) {
            ok &= (stg_emitter_init(&em, c->em[j].rec, 0, 0) == 0 && c->em[j].rec->count != 0u);
            if (c->gated && c->em[j].rec->count > max_gated)
                max_gated = c->em[j].rec->count;
        }
    }
    ok &= (max_gated <= STG_GATE_CARRY / 2u && stg_cards[STG_CARD_N - 1u].n_em == 6u);
    ck("S1 4 cards: timeouts 1800/1800/1800/2400, gaps 0/90/90/90, only the final is gated (6 emitters, volley <= 24)", ok);
    {
        uint32_t v[2];

        v[0] = stg_stage_n_events;
        v[1] = max_gated;
        info("S1 events max_gated_volley", v, 2u);
    }
}

/* --- S2 移动、延迟与出界 --------------------------------------------------- */

static void s2_move(void)
{
    static const stg_move mv_t[] = { { 0, 512, 3u }, { 256, -512, 2u } };
    static const stg_move mv_r[] = { { 256, 0, 1u } };
    static const stg_move mv_l[] = { { -1, 0, 1u } };
    static const stg_move mv_0[] = { { 0, 0, 1u } };
    static const stg_emit_rec rec = { 1u, STG_DIR_ABS, 0u, 16384u, 0u, 0u, 10u, 0u, 0u, 0, 0, 0, 0 };
    static const stg_emit_rec *const recs[] = { &rec };
    static const int32_t exp_dx[8] = { 0, 0, 0, 256, 512, 768, 1024, 1280 };
    static const int32_t exp_dy[8] = { 512, 1024, 1536, 1536, 1536, 1536, 1536, 1536 };
    const int32_t x0 = 100 * FX_ONE, y0 = 100 * FX_ONE;
    stg_pool      p;
    stg_ent       b;
    stg_emit_ctx  ctx;
    xs32          rng;
    uint32_t      i, first = 0u;
    int           ok = 1, ok_em;

    xs32_seed(&rng, 1u);
    ctx.px  = 200 * FX_ONE;
    ctx.py  = 400 * FX_ONE;
    ctx.rng = &rng;
    stg_enemies_init(&s_en);
    stg_pool_init(&p, s_mem, 4096u);
    ok &= (stg_enemy_spawn(&s_en, STG_ETYPE_DART, x0, y0, mv_t, 2u, recs, 1u, 5u) == 0);
    for (i = 0u; i < 16u; i++) {
        (void)stg_enemies_step(&s_en, &p, &ctx);
        if (i < 8u)
            ok &= (s_en.e[0].x == x0 + exp_dx[i] && s_en.e[0].y == y0 + exp_dy[i]);
        if (first == 0u && stg_pool_count(&p) != 0u)
            first = i + 1u;
    }
    ck("S2 move table: +increment on entering a step, velocity kept after the last step", ok);
    /* 延迟 5：第 6 次走（下标 5）齐射，出生在那一次移动之前的位置（走了 5 步之后）；间隔 10，第 16 次再发 */
    ok_em = (first == 6u && stg_pool_get(&p, 0u, &b) == 0 && b.x == x0 + 512 && b.y == y0 + 1536
             && stg_pool_count(&p) == 2u);
    ck("S2 emitter delay 5: first volley on the 6th step at the pre-move position, interval 10", ok_em);

    stg_enemies_init(&s_en);
    ok = (stg_enemy_spawn(&s_en, STG_ETYPE_DART, (432 * FX_ONE) - 1 - 256, 0, mv_r, 1u, recs, 0u, 0u) == 0);
    ok &= (stg_enemy_spawn(&s_en, STG_ETYPE_DART, -32 * FX_ONE, 0, mv_0, 1u, recs, 0u, 0u) == 1);
    ok &= (stg_enemy_spawn(&s_en, STG_ETYPE_DART, -32 * FX_ONE, 511 * FX_ONE, mv_l, 1u, recs, 0u, 0u) == 2);
    ok &= (stg_enemies_step(&s_en, &p, &ctx) == 1u);                /* 第 2 架越过 -32 */
    ok &= (s_en.e[0].alive == 1u && s_en.e[0].x == 432 * FX_ONE - 1 && s_en.e[1].alive == 1u && s_en.e[2].alive == 0u);
    ok &= (stg_enemies_step(&s_en, &p, &ctx) == 1u && s_en.e[0].alive == 0u && s_en.e[1].alive == 1u);
    ck("S2 out of bounds with 32 px margin: x = 432*256-1 kept, 432*256 removed, -32*256 kept, one less removed", ok);
}

/* --- S3 槽位 ---------------------------------------------------------------- */

static void s3_slots(void)
{
    static const stg_move mv_0[] = { { 0, 0, 1u } };
    uint32_t i;
    int      ok = 1;

    stg_enemies_init(&s_en);
    for (i = 0u; i < STG_ENEMY_SLOTS; i++)
        ok &= (stg_enemy_spawn(&s_en, i & 1u, 100 * FX_ONE, 100 * FX_ONE, mv_0, 1u, 0, 0u, 0u) == (int)i);
    ok &= (stg_enemy_spawn(&s_en, 0u, 0, 0, mv_0, 1u, 0, 0u, 0u) == -1 && s_en.spawn_overflow == 1u);
    s_en.e[5].alive = 0u;
    ok &= (stg_enemy_spawn(&s_en, 1u, 0, 0, mv_0, 1u, 0, 0u, 0u) == 5 && stg_enemies_alive(&s_en) == 64u);
    ck("S3 slots: first free, 65th refused (overflow 1), freed slot 5 reused", ok);
}

/* --- S4 击毁 ---------------------------------------------------------------- */

static void s4_kill(void)
{
    static const stg_move mv_0[] = { { 0, 0, 1u } };
    stg_game *g = &s_g;
    stg_pool  part;
    uint32_t  hits[STG_ENEMY_SLOTS], score = 0u, kills[STG_ETYPE_N] = { 0u, 0u };
    uint32_t  i, p, m, want = 0u, dmg = 0u, v;
    stg_ent   e;
    int       ok;

    stg_enemies_init(&s_en);
    stg_pool_init(&part, s_mem, 4096u);
    (void)stg_enemy_spawn(&s_en, STG_ETYPE_DART, 200 * FX_ONE, 300 * FX_ONE, mv_0, 1u, 0, 0u, 0u);
    (void)stg_enemy_spawn(&s_en, STG_ETYPE_CROSS, 100 * FX_ONE, 300 * FX_ONE, mv_0, 1u, 0, 0u, 0u);
    for (i = 0u; i < STG_ENEMY_SLOTS; i++)
        hits[i] = 0u;
    hits[0] = 3u;
    hits[1] = 11u;
    ok = (stg_enemies_settle(&s_en, hits, &part, &score, kills) == 1u);
    ok &= (s_en.e[0].alive == 0u && s_en.e[1].alive == 1u && s_en.e[1].hp == 1 && score == 100u && kills[0] == 1u);
    ok &= (stg_pool_count(&part) == 8u && stg_pool_get(&part, 2u, &e) == 0 && e.x == 200 * FX_ONE
           && e.vx == 0 && e.vy == 512 && e.sprite == 16u && e.flags == STG_KIND_SPARK);
    hits[0] = 0u;
    hits[1] = 1u;
    ok &= (stg_enemies_settle(&s_en, hits, &part, &score, kills) == 1u && score == 400u && kills[1] == 1u);
    ck("S4 settle: DART 3 hp dies on 3 hits (+100, 8 sparks), CROSS 12 hp survives 11 then dies (+300)", ok);

    /* 游戏里：DART 停在 (200, 200)，自机在 (200, 400) 不动。第 4v 帧的两束在第 4v + m 帧命中，
       m 是 (384 - 11 m - 200)^2 + 7^2 < 13^2 的最小值；累计伤害到 3 的那一帧击毁。 */
    m = 0u;
    while (!((184 - 11 * (int32_t)m) * (184 - 11 * (int32_t)m) + 49 < 169))
        m++;
    for (p = 0u; want == 0u && p < 200u; p++) {
        if (p >= m && ((p - m) & 3u) == 0u)
            dmg += 2u;                          /* 第 p - m 帧发的两束在第 p 帧命中 */
        if (dmg >= 3u)
            want = p;
    }
    stg_game_init(g, STG_GAME_SEED);
    g->ev_i = stg_stage_n_events;
    for (i = 0u; i < C_TITLE; i++)
        stg_game_step(g, 0u);
    (void)stg_enemy_spawn(&g->en, STG_ETYPE_DART, 200 * FX_ONE, 200 * FX_ONE, mv_0, 1u, 0, 0u, 0u);
    v = 0xFFFFFFFFu;
    for (p = 0u; p < 200u && v == 0xFFFFFFFFu; p++) {
        stg_game_step(g, 0u);
        if (g->en.e[0].alive == 0u)
            v = p;
    }
    ck("S4 game: DART above the player dies on play frame 20 (independent), score 100, 8 sparks",
       v == want && want == 20u && g->score == 100u && g->kills[0] == 1u
       && stg_pool_count(&g->f.pools.particle) == 8u);
}

/* --- S5、S6、S7 整局 --------------------------------------------------------- */

typedef struct {
    uint32_t start[STG_GSEG_N + 1u];     /* 最后一项：回到标题 */
    uint32_t frames;
    uint32_t score, graze, kills[STG_ETYPE_N], bonus;
    uint32_t n_kill_cards, spawn_frames_ok, spawn_n, spawn_overflow, max_alive, max_ebullet;
    int      boss_ok, gap_ok;
} run_res;

/* 一整局：从进游戏到 CLEAR 结束回到标题。shoot = 0 时自机不射击。受击关掉，不按键。 */
static void run_round(uint32_t shoot, run_res *r)
{
    stg_game *g = &s_g;
    uint32_t  k, i, prev_seg = STG_GSEG_TITLE, ev_next = 0u;
    uint32_t  card_start = 0u, snap_score = 0u, snap_graze = 0u, snap_k[STG_ETYPE_N] = { 0u, 0u };
    uint32_t  prev_on = STG_BOSS_OFF, snap_over = 0u;
    int32_t   prev_hp = 0;

    for (i = 0u; i <= STG_GSEG_N; i++)
        r->start[i] = 0xFFFFFFFFu;
    r->start[0] = 0u;
    r->bonus = 0u;
    r->n_kill_cards = 0u;
    r->spawn_frames_ok = 1u;
    r->spawn_n = 0u;
    r->boss_ok = 1;
    r->gap_ok = 1;
    r->max_alive = 0u;
    r->max_ebullet = 0u;

    stg_game_init(g, STG_GAME_SEED);
    g->shoot_on = shoot;
    stg_game_set_hits(g, 0);
    for (k = 0u; k < 20000u; k++) {
        const stg_boss *b = &g->boss;

        stg_game_step(g, 0u);
        if (stg_enemies_alive(&g->en) > r->max_alive)
            r->max_alive = stg_enemies_alive(&g->en);
        if (stg_pool_count(&g->f.pools.ebullet) > r->max_ebullet)
            r->max_ebullet = stg_pool_count(&g->f.pools.ebullet);

        /* 出敌的帧：事件表里出敌事件的帧号加 120（同一帧的几条算一次） */
        if ((g->ev & STG_EV_SPAWN) != 0u) {
            while (ev_next < stg_stage_n_events && stg_stage_events[ev_next].type != STG_EVT_SPAWN)
                ev_next++;
            if (ev_next >= stg_stage_n_events || stg_stage_events[ev_next].t + C_TITLE != k)
                r->spawn_frames_ok = 0u;
            while (ev_next < stg_stage_n_events && stg_stage_events[ev_next].t + C_TITLE == k) {
                if (stg_stage_events[ev_next].type == STG_EVT_SPAWN)
                    r->spawn_n++;
                ev_next++;
            }
        }

        /* 符卡奖励：按结束之前那一段的符卡序号另算 */
        if ((g->ev & STG_EV_CARD_KILL) != 0u) {
            r->bonus += 10000u * (prev_seg - STG_GSEG_SPELL1 + 1u);
            if (prev_seg == STG_GSEG_SPELL1 + 3u)
                r->bonus += 50000u;
            r->n_kill_cards++;
        }

        /* Boss：进场逐帧下移、之后的游动位置；进场与间隙里不受伤。
           第一张符卡开始的那一帧（第 4 步切换）第 5 步已经游动了一步，a = 91 x (k - card_start + 1)。 */
        if (b->on == STG_BOSS_CARD && b->card == 0u && prev_on == STG_BOSS_ENTER)
            card_start = k;
        if (b->on == STG_BOSS_ENTER) {
            const uint32_t n = k - (C_TITLE + C_BOSS_T) + 1u;       /* 已经下移了几次 */

            if (b->x != 200 * FX_ONE || b->y != -48 * FX_ONE + (int32_t)n * 2 * FX_ONE)
                r->boss_ok = 0;
            if (g->tg[STG_ENEMY_SLOTS].alive != 0)
                r->gap_ok = 0;
        } else if (b->on != STG_BOSS_OFF) {
            const uint16_t a = (uint16_t)(91u * (k - card_start + 1u));
            const int32_t  ex = 200 * FX_ONE + ((70 * fx_sin(a)) >> 6);
            const int32_t  ey = 100 * FX_ONE + ((12 * fx_sin((uint16_t)(2u * a))) >> 6);

            if (b->x != ex || b->y != ey)
                r->boss_ok = 0;
            /* 上一帧与这一帧都在间隙里：这一帧的第 6 步 Boss 不是目标、血量不变
               （血量耗尽的那一帧在第 7 步才进间隙，那一帧的第 6 步还在符卡里） */
            if (b->on == STG_BOSS_GAP && prev_on == STG_BOSS_GAP
                && (g->tg[STG_ENEMY_SLOTS].alive != 0 || b->hp != prev_hp))
                r->gap_ok = 0;
        }
        prev_hp = b->hp;
        prev_on = b->on;

        if ((g->ev & STG_EV_SEG) != 0u) {
            if (g->seg == STG_GSEG_TITLE) {
                r->start[STG_GSEG_N] = k;
                break;
            }
            if (r->start[g->seg] == 0xFFFFFFFFu)
                r->start[g->seg] = k;
        }
        prev_seg = g->seg;
        snap_score = g->score;
        snap_graze = g->graze;
        snap_k[0] = g->kills[0];
        snap_k[1] = g->kills[1];
        snap_over = g->en.spawn_overflow;
    }
    r->frames   = k;
    r->score    = snap_score;
    r->graze    = snap_graze;
    r->kills[0] = snap_k[0];
    r->kills[1] = snap_k[1];
    r->spawn_overflow = snap_over;
}

static void s5_s7_round(void)
{
    run_res  r;
    uint32_t want[STG_GSEG_N + 1u], i, n_spawn = 0u;
    int      ok = 1;

    for (i = 0u; i < stg_stage_n_events; i++)
        n_spawn += (stg_stage_events[i].type == STG_EVT_SPAWN) ? 1u : 0u;

    /* 2.5 节的常量另算 */
    want[STG_GSEG_TITLE]       = 0u;
    want[STG_GSEG_STAGE]       = C_TITLE;
    want[STG_GSEG_BOSS]        = C_TITLE + C_BOSS_T;
    want[STG_GSEG_SPELL1]      = want[STG_GSEG_BOSS] + C_ENTER;
    want[STG_GSEG_SPELL1 + 1u] = want[STG_GSEG_SPELL1] + c_timeout[0];
    want[STG_GSEG_SPELL1 + 2u] = want[STG_GSEG_SPELL1 + 1u] + C_GAP + c_timeout[1];
    want[STG_GSEG_SPELL1 + 3u] = want[STG_GSEG_SPELL1 + 2u] + C_GAP + c_timeout[2];
    want[STG_GSEG_CLEAR]       = want[STG_GSEG_SPELL1 + 3u] + C_GAP + c_timeout[3];
    want[STG_GSEG_N]           = want[STG_GSEG_CLEAR] + C_CLEAR;

    run_round(0u, &r);
    for (i = 0u; i <= STG_GSEG_N; i++)
        ok &= (r.start[i] == want[i]);
    ck("S5 no shots, hits off: segment starts == independent (title, stage, boss, 4 cards, clear, title)", ok);
    info("S5 starts", r.start, STG_GSEG_N + 1u);
    {
        uint32_t v[2];

        v[0] = r.max_alive;
        v[1] = r.max_ebullet;
        info("S5 max enemies_alive ebullets", v, 2u);
    }
    ck("S5 spawn frames == event frame + 120, all spawn events executed, no slot overflow",
       r.spawn_frames_ok != 0u && r.spawn_n == n_spawn && r.spawn_overflow == 0u);
    ck("S7 boss: entry 2 px/frame from (200, -48), then (200 + 70 sin a, 100 + 12 sin 2a), a += 91", r.boss_ok);
    ck("S7 boss not a target during entry and gaps, hp unchanged", r.gap_ok);
    ck("S5 no shots: no kills, no card bonus, score == 10 x graze",
       r.kills[0] == 0u && r.kills[1] == 0u && r.n_kill_cards == 0u && r.score == 10u * r.graze);

    run_round(1u, &r);
    ok = (r.start[STG_GSEG_CLEAR] != 0xFFFFFFFFu && r.start[STG_GSEG_N] == r.start[STG_GSEG_CLEAR] + C_CLEAR);
    for (i = 1u; i < STG_GSEG_N; i++)
        ok &= (r.start[i] != 0xFFFFFFFFu && r.start[i] > r.start[i - 1u]);
    ck("S6 shooting, hits off: reaches CLEAR and back to title, segments in order", ok);
    info("S6 starts", r.start, STG_GSEG_N + 1u);
    ck("S6 score == 10 x graze + 100 x DART + 300 x CROSS + card bonus (independent)",
       r.score == 10u * r.graze + 100u * r.kills[0] + 300u * r.kills[1] + r.bonus
       && s_g.hiscore == r.score && s_g.score == 0u);
    {
        uint32_t v[6];

        v[0] = r.kills[0];
        v[1] = r.kills[1];
        v[2] = r.n_kill_cards;
        v[3] = r.bonus;
        v[4] = r.graze;
        v[5] = r.score;
        info("S6 darts crosses cards_killed bonus graze score", v, 6u);
    }
    ck("S6 boss path and gap invulnerability also hold with shooting", r.boss_ok && r.gap_ok);
}

/* --- S8 压力形态 ------------------------------------------------------------ */

static uint32_t kinds_all(const stg_pool *p, uint32_t kind)
{
    uint32_t i;
    stg_ent  e;

    for (i = 0u; stg_pool_get(p, i, &e) == 0; i++)
        if ((e.flags & STG_F_KIND_MASK) != kind)
            return 0u;
    return 1u;
}

static void s8_press(void)
{
    stg_game *g = &s_g;
    xs32      rng;
    uint32_t  i, k;
    int       ok = 1;

    xs32_seed(&rng, 0x5E7B0C08u);
    for (k = 0u; k < 2u; k++) {
        const uint32_t kind = (k == 0u) ? 0u : 4u;

        stg_game_press_begin(g, kind);
        ok &= (g->phase == STG_PH_PLAY && g->seg == STG_GSEG_SPELL1 + 3u && g->boss.on == STG_BOSS_CARD
               && g->boss.card == 3u && g->hits_on == 0u && g->shoot_on == 0u);
        stg_game_set_knob(g, 1600u);
        for (i = 0u; i < 3000u; i++) {
            stg_game_step(g, xs32_next(&rng) & 0x3Fu);
            ok &= (g->boss.x == 200 * FX_ONE && g->boss.y == 100 * FX_ONE && g->boss.hp == 600
                   && g->boss.on == STG_BOSS_CARD && g->seg == STG_GSEG_SPELL1 + 3u
                   && g->f.px == 200 * FX_ONE && g->f.py == 400 * FX_ONE
                   && stg_pool_count(&g->f.pools.pbullet) == 0u && g->lives == 3u && g->bombs == 3u);
        }
        ok &= (kinds_all(&g->f.pools.ebullet, kind) != 0u && stg_pool_count(&g->f.pools.ebullet) > 1000u);
    }
    ck("S8 press: boss fixed at (200, 100), no damage, card never ends, player fixed, no shots, keys ignored, kind 0 / 4", ok);
}

/* --- S9 旋钮加倍 ------------------------------------------------------------- */

static void s9_knob(void)
{
    static const uint32_t knobs[5] = { 200u, 400u, 800u, 1600u, 3200u };
    stg_game *g = &s_g;
    uint32_t  avg[5], i, j, k;
    int       ok = 1;

    stg_game_press_begin(g, 0u);
    for (k = 0u; k < 5u; k++) {
        uint64_t sum = 0u;

        stg_game_set_knob(g, knobs[k]);
        for (i = 0u; i < SETTLE_MIN; i++)
            stg_game_step(g, 0u);
        for (j = 0u; j < 300u; j++) {
            stg_game_step(g, 0u);
            sum += stg_pool_count(&g->f.pools.ebullet);
        }
        avg[k] = (uint32_t)(sum / 300u);
        if (k > 0u && avg[k] < avg[k - 1u])
            ok = 0;
    }
    ck("S9 knob 200, 400, 800, 1600, 3200: steady bullet count never decreases", ok && avg[0] > 0u);
    info("S9 steady bullets", avg, 5u);
}

/* --- S10 到达稳态的帧数 ------------------------------------------------------ */

static uint32_t tagged(const stg_pool *p)
{
    uint32_t i, n = 0u;
    stg_ent  e;

    for (i = 0u; stg_pool_get(p, i, &e) == 0; i++)
        n += ((e.flags & TAG) != 0u) ? 1u : 0u;
    return n;
}

static void s10_settle(void)
{
    static const uint32_t knobs[7] = { 200u, 400u, 800u, 1600u, 3200u, 200u, 3200u };
    stg_game *g = &s_g;
    uint32_t  settle[7], i, k, mx = 0u;
    int       ok = 1;

    stg_game_press_begin(g, 0u);
    stg_game_set_knob(g, 100u);
    for (i = 0u; i < 400u; i++)
        stg_game_step(g, 0u);
    for (k = 0u; k < 7u; k++) {
        stg_pool *p = &g->f.pools.ebullet;

        /* 改档：此刻池里的弹都是改档之前出生的，打上记号 */
        for (i = 0u; i < p->n; i++)
            p->ent[i].flags = (uint16_t)(p->ent[i].flags | TAG);
        stg_game_set_knob(g, knobs[k]);
        settle[k] = 0xFFFFFFFFu;
        for (i = 1u; i <= 600u; i++) {
            stg_game_step(g, 0u);
            if (settle[k] == 0xFFFFFFFFu && tagged(p) == 0u)
                settle[k] = i;
        }
        if (settle[k] > SETTLE_MIN)
            ok = 0;
        if (settle[k] > mx && settle[k] != 0xFFFFFFFFu)
            mx = settle[k];
    }
    ck("S10 press form: bullets born before a knob change all gone within SETTLE_MIN = 300 frames", ok);
    info("S10 settle frames per change", settle, 7u);
    info("S10 settle max", &mx, 1u);
}

/* --- S11 游戏里的门控 -------------------------------------------------------- */

static void s11_gate(void)
{
    stg_game *g = &s_g;
    uint64_t  sum_k = 0u;
    uint32_t  j, v[3];
    int       ok = 1;

    stg_game_press_begin(g, 0u);
    g->press = 0u;              /* 游戏里的终符：K 随符卡时间上升，Boss 游动 */
    for (j = 0u; j < 2000u; j++) {
        const uint32_t k = 600u + ((j * 3u) >> 2);

        stg_game_step(g, 0u);
        sum_k += k;
        ok &= (g->boss.gate_k == k && g->boss.on == STG_BOSS_CARD
               && (uint64_t)g->boss.gate_fired + g->boss.gate_tok == sum_k / 125u
               && g->boss.gate_acc == (uint32_t)(sum_k % 125u));
    }
    ck("S11 in-game gate: K = 600 + (t*3)>>2, fired + tokens == floor(sum K / 125) every frame", ok);
    v[0] = g->boss.gate_k;
    v[1] = g->boss.gate_fired;
    v[2] = stg_pool_count(&g->f.pools.ebullet);
    info("S11 frame 1999: K fired_total ebullets", v, 3u);
}

/* --- S12 背景 ---------------------------------------------------------------- */

static void s12_bg(void)
{
    stg_bg   b;
    xs32     rng;
    uint32_t n, i;
    int      ok = 1;

    xs32_seed(&rng, 0x5E7B0C0Cu);
    stg_bg_init(&b, &rng);
    for (n = 1u; n <= 1000u; n++) {
        stg_bg_step(&b);
        ok &= (b.scroll == (230u * n) % 8192u && b.phase == ((230u * n) / 8192u) % 4u);
        for (i = 0u; i < STG_BG_STARS; i++)
            ok &= (b.star[i].x >= 0 && b.star[i].x < 398 * FX_ONE && b.star[i].y >= -2 * FX_ONE
                   && b.star[i].y < 480 * FX_ONE && b.star[i].vy >= 307 && b.star[i].vy <= 921);
    }
    ok &= (stg_bg_hline_y(&b, 14u) <= 479 && stg_bg_vline_x(12u) == 392);
    ck("S12 background: scroll 230/256 px per frame, phase +1 per 32 px, stars in range", ok);
}

uint32_t test_stg_stage_run(void)
{
    g_fail = 0u;
    s1_tables();
    s2_move();
    s3_slots();
    s4_kill();
    s5_s7_round();
    s8_press();
    s9_knob();
    s10_settle();
    s11_gate();
    s12_bg();
    return g_fail;
}
