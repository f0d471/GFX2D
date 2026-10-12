/******************************************************************************
* @file stg_auto.c: 自动驾驶
*
* @brief  口径见 stg_auto.h。
******************************************************************************/

#include "stg_auto.h"

#include <stddef.h>

/* 九个候选的固定顺序：不动、上、下、左、右、左上、右上、左下、右下 */
static const uint32_t s_cand[STG_AUTO_N] = {
    0u,
    STG_KEY_UP,
    STG_KEY_DOWN,
    STG_KEY_LEFT,
    STG_KEY_RIGHT,
    STG_KEY_UP | STG_KEY_LEFT,
    STG_KEY_UP | STG_KEY_RIGHT,
    STG_KEY_DOWN | STG_KEY_LEFT,
    STG_KEY_DOWN | STG_KEY_RIGHT,
};

int stg_auto_take(uint32_t *idle, uint32_t keys)
{
    if (keys != 0u) {
        *idle = 0u;
        return 0;
    }
    if (*idle >= STG_AUTO_IDLE)
        return 1;
    (*idle)++;
    return 0;
}

uint32_t stg_auto_cand_keys(uint32_t i)
{
    return (i < STG_AUTO_N) ? s_cand[i] : 0u;
}

static int32_t clamp(int32_t v, int32_t lo, int32_t hi)
{
    return (v < lo) ? lo : (v > hi) ? hi : v;
}

static int32_t iabs(int32_t v)
{
    return (v < 0) ? -v : v;
}

/* 与游戏的移动同一套：高速，斜向每轴乘 181/256，走 STG_AUTO_LOOK 帧之后夹在移动范围内。
   自机从范围内出发、每帧同向走，逐帧夹与最后夹一次结果相同。 */
static void cand_pos(const stg_game *g, uint32_t keys, int32_t *x, int32_t *y)
{
    const int32_t dx = ((keys & STG_KEY_RIGHT) != 0u) - ((keys & STG_KEY_LEFT) != 0u);
    const int32_t dy = ((keys & STG_KEY_DOWN) != 0u) - ((keys & STG_KEY_UP) != 0u);
    int32_t       v = STG_SPEED_FAST;

    if (dx != 0 && dy != 0)
        v = (v * STG_DIAG_MUL) >> 8;
    *x = fx_px(clamp(g->f.px + dx * v * STG_AUTO_LOOK, STG_PLAYER_X_MIN, STG_PLAYER_X_MAX));
    *y = fx_px(clamp(g->f.py + dy * v * STG_AUTO_LOOK, STG_PLAYER_Y_MIN, STG_PLAYER_Y_MAX));
}

uint32_t stg_auto_keys(const stg_game *g, uint32_t *risk)
{
    stg_danger_q q;
    uint32_t     score[STG_AUTO_N];
    uint32_t     i, k, best = 0u;
    const int32_t hx = (g->boss.on != STG_BOSS_OFF) ? fx_px(g->boss.x) : STG_AUTO_HOME_X;

    q.look  = STG_AUTO_LOOK;
    q.px    = fx_px(g->f.px);
    q.py    = fx_px(g->f.py);
    q.reach = STG_AUTO_REACH_PX;
    q.r     = STG_AUTO_R_PX;
    q.n     = STG_AUTO_N;
    for (i = 0u; i < STG_AUTO_N; i++) {
        cand_pos(g, s_cand[i], &q.cx[i], &q.cy[i]);
        score[i] = 0u;
    }
    stg_pool_danger(&g->f.pools.ebullet, &q, score);

    for (i = 0u; i < STG_AUTO_N; i++)
        score[i] += STG_AUTO_PREF_W * (uint32_t)(iabs(q.cx[i] - hx) + iabs(q.cy[i] - STG_AUTO_HOME_Y));

    /* 取最小，平局取顺序在前的（严格小于才换） */
    for (k = 0u; k < STG_AUTO_N; k++) {
#ifdef INJECT_AUTO_ORDER
        i = STG_AUTO_N - 1u - k;        /* 注错：候选顺序倒过来 */
        if (k == 0u)
            best = i;
#else
        i = k;
#endif
        if (score[i] < score[best])
            best = i;
    }
    if (risk != NULL)
        for (i = 0u; i < STG_AUTO_N; i++)
            risk[i] = score[i];
    return s_cand[best];
}
