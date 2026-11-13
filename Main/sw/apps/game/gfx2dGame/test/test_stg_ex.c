/******************************************************************************
* @file test_stg_ex.c: EX 关卡的主机侧测试
*
* @brief  E 组（EX 路线，见 stg_stage.c 中的 EX 路线定义；不链 bsp）：
*           E1  经典路线纯度：常规按键跑 2400 帧，关卡集指针始终是经典、ex 恒 0
*           E2  触发：标题→开局的过渡帧上四方向同按进入 EX，Boss 按时进场，
*               段名走 EX 的符卡名；受击关掉时符卡按超时推进
*           E3  确定性：同种子同按键跑两遍 EX，2500 帧后的得分/擦弹/Boss 血量/池计数逐项相同
*           E4  表契约：EX 事件表最后一条是进 Boss、出敌的移动/发射器组都落在 EX 表界内、
*               符卡结构合法（发射器数不超上限、名字与经典不同）
******************************************************************************/

#include <stdio.h>
#include <string.h>

#include "core/format.h"
#include "game/stg_game.h"
#include "game/stg_stage.h"
#include "test_stg_ex.h"

#define ALL4 (STG_KEY_UP | STG_KEY_DOWN | STG_KEY_LEFT | STG_KEY_RIGHT)

static uint32_t g_fail;

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

static void ex_run(stg_game *g, uint32_t frames, uint32_t ex_at)
{
    uint32_t f;

    stg_game_init(g, STG_GAME_SEED);
    stg_game_set_hits(g, 0);
    for (f = 0u; f < frames; f++) {
        const uint32_t keys = (ex_at != 0u && f == ex_at) ? ALL4 : 0u;

        stg_game_step(g, keys);
    }
}

static void e1_classic_purity(void)
{
    stg_game g;
    uint32_t f, ok = 1u;

    stg_game_init(&g, STG_GAME_SEED);
    stg_game_set_hits(&g, 0);
    for (f = 0u; f < 2400u; f++) {
        /* 常规走位按键：单方向与低速交替，绝无四向同按 */
        const uint32_t keys = ((f >> 4) & 1u) ? STG_KEY_LEFT : STG_KEY_RIGHT;

        stg_game_step(&g, keys);
        if (g.ex != 0u || g.ss != &stg_stage_classic)
            ok = 0u;
    }
    ck("L1 classic route: 2400 frames of normal keys keep stage set classic, ex 0", ok);
    ck("L1 classic route: boss entered by frame 2400", g.boss.on != STG_BOSS_OFF);
}

static void e2_ex_trigger(void)
{
    stg_game    g;
    uint32_t    f;
    const char *name;

    ex_run(&g, 2400u, STG_TITLE_FRAMES);
    ck("L2 all-arrows at the transition frame selects EX stage set", g.ex == 1u && g.ss == &stg_stage_ex);
    /* Boss 进场（EX 事件表 2120 帧 + 74 帧进场）后第一张符卡在跑，段名是 EX 的 */
    ck("L2 boss entered and card running by frame 2400",
       g.boss.on == STG_BOSS_CARD && g.boss.card == 0u);
    name = stg_game_seg_name_g(&g, g.seg);
    ck("L2 HUD segment name is the EX card name", strcmp(name, "EX1 LOTUS RING") == 0);
    ck("L2 classic seg name untouched", strcmp(stg_game_seg_name(g.seg), "SPELL 1 RING+AIM") == 0);
    /* 受击已关：第一张符卡按超时结束进第二张（1800 帧超时 + 进场 74 + 间隙） */
    for (f = 2400u; f < 4300u; f++)
        stg_game_step(&g, 0u);
    ck("L2 with hits off, EX card 1 times out into card 2",
       g.boss.on == STG_BOSS_CARD && g.boss.card == 1u &&
       strcmp(stg_game_seg_name_g(&g, g.seg), "EX2 TWIN FAN") == 0);
    /* 回到标题之后 EX 复位（round_reset 把关卡集指回经典） */
    for (f = 4300u; f < 4300u + 1800u + 90u + 1800u + 90u + 1800u + 90u + 2400u + STG_CLEAR_FRAMES + 8u; f++)
        stg_game_step(&g, 0u);
    ck("L2 after full EX clear and return to title, stage set resets to classic",
       g.ex == 0u && g.ss == &stg_stage_classic);
}

static void e3_determinism(void)
{
    stg_game a, b;

    (void)ex_run(&a, 2500u, STG_TITLE_FRAMES);
    (void)ex_run(&b, 2500u, STG_TITLE_FRAMES);
    ck("L3 two identical EX runs: score, graze, boss hp, bullet count equal",
       a.score == b.score && a.graze == b.graze && a.boss.hp == b.boss.hp &&
       stg_pool_count(&a.f.pools.ebullet) == stg_pool_count(&b.f.pools.ebullet) &&
       a.kills[0] == b.kills[0] && a.seg == b.seg && a.ev_i == b.ev_i);
}

static void e4_table_contract(void)
{
    uint32_t i, ok_tail, ok_bounds, ok_cards;

    ok_tail = (stg_ex_events[stg_ex_n_events - 1u].type == STG_EVT_BOSS);
    ok_bounds = 1u;
    for (i = 0u; i < stg_ex_n_events; i++) {
        const stg_event *e = &stg_ex_events[i];

        if (e->type == STG_EVT_SPAWN && (e->move >= stg_n_ex_moves || e->group >= stg_n_ex_em_groups))
            ok_bounds = 0u;
    }
    ok_cards = 1u;
    for (i = 0u; i < STG_CARD_N; i++) {
        if (stg_ex_cards[i].n_em == 0u || stg_ex_cards[i].n_em > STG_CARD_EM_MAX)
            ok_cards = 0u;
        if (strcmp(stg_ex_cards[i].name, stg_cards[i].name) == 0)
            ok_cards = 0u;
    }
    ck("L4 EX event table ends with the boss event", ok_tail);
    ck("L4 EX spawns reference in-table moves and emitter groups", ok_bounds);
    ck("L4 EX cards are structurally valid and renamed", ok_cards);
}

uint32_t test_stg_ex_run(void)
{
    g_fail = 0u;
    e1_classic_purity();
    e2_ex_trigger();
    e3_determinism();
    e4_table_contract();
    return g_fail;
}
