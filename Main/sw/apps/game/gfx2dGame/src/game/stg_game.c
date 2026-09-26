/******************************************************************************
* @file stg_game.c: 游戏状态与一帧的顺序
*
* @brief  口径见 stg_game.h 与 SW7 2.4 节。全部是整数运算，没有浮点、没有 malloc、
*         没有文件级的可变状态。
******************************************************************************/

#include "stg_game.h"

#include "stg_kind.h"

/* 自机弹池的边界：游戏区四周外扩 12 像素（6x24 光束的半高），半开区间。 */
static void pb_bounds(stg_bounds *b)
{
    b->x_lo = -STG_PB_MARGIN_PX * FX_ONE;
    b->x_hi = (STG_FIELD_W + STG_PB_MARGIN_PX) * FX_ONE;
    b->y_lo = -STG_PB_MARGIN_PX * FX_ONE;
    b->y_hi = (STG_FIELD_H + STG_PB_MARGIN_PX) * FX_ONE;
}

/* 一局从标题开始：池清空、随机数重新播种、自机回出生点、残机与炸弹补满、得分清零。
   HI-SCORE、受击钩子、上一帧的按键与帧数不动。 */
static void round_reset(stg_game *g)
{
    stg_field_init(&g->f, g->seed);
    g->phase     = STG_PH_TITLE;
    g->phase_t   = 0u;
    g->slow      = 0u;
    g->shot_wait = 0u;
    g->lives     = STG_LIVES_INIT;
    g->bombs     = STG_BOMBS_INIT;
    g->inv       = 0u;
    g->bomb_req  = 0u;
    g->score     = 0u;
    g->graze     = 0u;
    g->cancelled = 0u;
}

void stg_game_init(stg_game *g, uint32_t seed)
{
    g->seed      = seed;
    g->frame     = 0u;
    g->keys_prev = 0u;
    g->flash     = 0u;
    g->hits_on   = 1u;
    g->hiscore   = 0u;
    g->ev        = 0u;
    pb_bounds(&g->pb_bounds);
    round_reset(g);
}

void stg_game_set_hits(stg_game *g, int on)
{
    g->hits_on = (on != 0) ? 1u : 0u;
}

/* 一个轴：负向键与正向键只按一个时取它的方向，都按或都不按为 0。 */
static int32_t axis(uint32_t keys, uint32_t neg, uint32_t pos)
{
    const uint32_t n = ((keys & neg) != 0u) ? 1u : 0u;
    const uint32_t p = ((keys & pos) != 0u) ? 1u : 0u;

    return (int32_t)p - (int32_t)n;
}

static int32_t clamp(int32_t v, int32_t lo, int32_t hi)
{
    return (v < lo) ? lo : (v > hi) ? hi : v;
}

/* 第 3 步前半：移动。斜向时每轴 (v * 181) >> 8，v 取正值算完再补符号。 */
static void player_move(stg_game *g, uint32_t keys)
{
    const int32_t dx = axis(keys, STG_KEY_LEFT, STG_KEY_RIGHT);
    const int32_t dy = axis(keys, STG_KEY_UP, STG_KEY_DOWN);
    int32_t       v = g->slow ? STG_SPEED_SLOW : STG_SPEED_FAST;

    if (dx != 0 && dy != 0)
        v = (v * STG_DIAG_MUL) >> 8;
    g->f.px = clamp(g->f.px + dx * v, STG_PLAYER_X_MIN, STG_PLAYER_X_MAX);
    g->f.py = clamp(g->f.py + dy * v, STG_PLAYER_Y_MIN, STG_PLAYER_Y_MAX);
}

/* 第 3 步后半：自动射击，倒数计数到 0 就发两束（先左后右）、再装入 3。 */
static void player_shoot(stg_game *g)
{
    const uint16_t kind = (uint16_t)STG_KIND_PSHOT;

    if (g->shot_wait != 0u) {
        g->shot_wait--;
        return;
    }
    g->shot_wait = STG_SHOT_INTERVAL - 1u;
    (void)stg_pool_add(&g->f.pools.pbullet, g->f.px - STG_SHOT_DX, g->f.py + STG_SHOT_DY,
                       0, STG_SHOT_VY, stg_kind_sprite(kind, 0u), kind);
    (void)stg_pool_add(&g->f.pools.pbullet, g->f.px + STG_SHOT_DX, g->f.py + STG_SHOT_DY,
                       0, STG_SHOT_VY, stg_kind_sprite(kind, 0u), kind);
}

/* 第 6 步后半：自机判定点对敌弹。受击开关 = 无敌为 0 且受击钩子打开；擦弹照算、按擦弹得分。
   返回本帧是否被击中。 */
static int player_collide(stg_game *g)
{
#ifdef INJECT_GAME_NO_INVINCIBLE
    g->f.hit_enabled = (g->hits_on != 0u);     /* 注错：无敌期不生效 */
#else
    g->f.hit_enabled = (g->inv == 0u && g->hits_on != 0u);
#endif
    stg_field_collide(&g->f);
    g->graze += g->f.frame_hit.grazes;
    g->score += g->f.frame_hit.grazes * STG_SCORE_GRAZE;
    return g->f.frame_hit.hits != 0u;
}

/* 炸弹：可消除的敌弹打移除标记、原位生成火花（粒子池满了计粒子池的溢出数），无敌延长到至少 180 帧。 */
static void bomb(stg_game *g)
{
    g->bombs--;
    g->cancelled = stg_pool_cancel(&g->f.pools.ebullet, &g->f.pools.particle, 0, STG_SPARK_VY,
                                   (uint16_t)STG_SPARK_LIFE, (uint16_t)STG_KIND_SPARK);
    if (g->inv < STG_INV_BOMB)
        g->inv = STG_INV_BOMB;
    g->ev |= STG_EV_BOMB;
}

void stg_game_step(stg_game *g, uint32_t keys)
{
    const uint32_t pressed = keys & ~g->keys_prev;
    int            hit = 0;

    /* 1. 输入与阶段切换 */
    g->ev        = 0u;
    g->flash     = 0u;
    g->cancelled = 0u;
    g->keys_prev = keys;
    if (g->phase == STG_PH_TITLE && g->phase_t >= STG_TITLE_FRAMES) {
        g->phase   = STG_PH_PLAY;
        g->phase_t = 0u;
        g->ev     |= STG_EV_START;
    } else if (g->phase == STG_PH_CLEAR && g->phase_t >= STG_CLEAR_FRAMES) {
        round_reset(g);
    }
    g->slow     = (g->phase == STG_PH_PLAY && (keys & STG_KEY_SLOW) != 0u) ? 1u : 0u;
    g->bomb_req = (g->phase == STG_PH_PLAY && (pressed & STG_KEY_BOMB) != 0u && g->bombs != 0u) ? 1u : 0u;

    /* 2. 四个池各一遍，粒子池再走一遍寿命 */
    stg_pool_step(&g->f.pools.ebullet, &g->f.bounds, &g->f.last);
    stg_pool_step(&g->f.pools.pbullet, &g->pb_bounds, 0);
    stg_pool_step(&g->f.pools.enemy, &g->f.bounds, 0);
    stg_pool_step(&g->f.pools.particle, &g->f.bounds, 0);
    stg_pool_age(&g->f.pools.particle);

    if (g->phase == STG_PH_PLAY) {
        /* 3. 自机 */
        player_move(g, keys);
        player_shoot(g);

        /* 4、5. 事件表、符卡表、敌机与 Boss：C 批 C2 */

        /* 6. 碰撞 */
        hit = player_collide(g);
    }

    /* 7. 结算：无敌倒计时先减一，再处理本帧的受击与炸弹 */
    if (g->inv != 0u)
        g->inv--;
    if (hit) {
        g->ev |= STG_EV_HIT;
#ifdef INJECT_GAME_NO_AUTOBOMB
        if (0) {                                /* 注错：自动炸弹失效 */
#else
        if (g->bombs != 0u) {
#endif
            if (!g->bomb_req)
                g->ev |= STG_EV_AUTOBOMB;
            g->bomb_req = 1u;
        } else {
            g->lives--;
            g->flash  = 1u;
            g->inv    = STG_INV_HIT;
            g->bombs  = STG_BOMBS_INIT;
            g->ev    |= STG_EV_LOST;
        }
    }
    if (g->bomb_req && g->bombs != 0u)
        bomb(g);
    g->bomb_req = 0u;

    /* HI-SCORE 在局的结束之前取 max：结束那一帧的得分也算 */
    if (g->score > g->hiscore)
        g->hiscore = g->score;
    if (g->phase == STG_PH_PLAY && g->lives == 0u) {
        round_reset(g);
        g->ev |= STG_EV_OVER;
    }
    g->phase_t++;
    g->frame++;
}
