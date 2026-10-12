/******************************************************************************
* @file test_stg_game.c: 自机与规则的主机侧测试
*
* @brief  R 组。期望值都在测试里按规则的常量另算：
*           R1  移动：八个方向、高低速、斜向每轴 (v * 181) >> 8；对立方向当作该轴不动；贴边夹住；
*               标题期间不动
*           R2  射击：标题期间不射；开局第一帧两束在 (±7, -16) 像素、向上 11 像素/帧；
*               每 4 帧一次，每帧的存活数等于按出界边界另算的值
*           R3  受击：没有炸弹时扣残机、闪白、炸弹补满；无敌恰好 120 帧（期间停在自机上的弹只算擦弹），
*               第 121 帧照常受击；无敌期间自机隐去的帧数
*           R4  自动炸弹：受击且有炸弹时不扣残机、扣一个炸弹，可消除的敌弹全部打标记、原位生成火花，
*               不可消除的留下；火花向上走、16 帧后打标记；炸弹之后恰好 180 帧不受击
*           R5  手动炸弹只认按下沿：按住不连放；标题里按住的键开局时不算按下；没有炸弹时不放
*           R6  炸弹消 3000 颗：粒子池满了只生成 2048 个火花，另计粒子溢出 952，敌弹池的溢出不动
*           R7  残机用完：回标题、残机炸弹补满、得分清零、HI-SCORE 保留，120 帧后开局
*           R8  擦弹得分：受击钩子关掉时停在自机周围的一片弹按测试里另写的 64 位距离算擦弹数，
*               得分 = 10 × 擦弹数；再走一帧不重复计
*           R9  自机弹对敌机（stg_pool_hit_targets）：恰好相切不命中、再近 1/256 像素命中（四个方向与勾股数）；
*               一颗弹只打第一个目标；存活为 0 的目标穿过去；已标记的弹不参与；
*               随机样本对 64 位、不粗筛的朴素参考，每个目标的命中数与每颗的标志全同
*           R10 粒子寿命（stg_pool_age）与弹种位图
*
*         注错开关（见 stg_game.h 与 stg_pool.h 的文件头），每个都必须见红：
*           GAME_NO_AUTOBOMB    -> R4 auto bomb
*           GAME_NO_INVINCIBLE  -> R3 invincible
*           PHIT_LE             -> R9 tangent
******************************************************************************/

#include <stdint.h>
#include <stdio.h>

#include "core/format.h"
#include "core/xorshift32.h"
#include "game/stg_game.h"
#include "game/stg_kind.h"
#include "game/stg_pool.h"
#include "test_stg_game.h"

#define SEED        0x5E7B0C01u

static uint32_t g_fail;

/* 游戏状态约 380 KB，放静态区。 */
static stg_game s_g;
static stg_ent  s_mem[256];
static stg_ent  s_ref[256];

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

/* 一行数值：tag 后跟若干个 u32。 */
static void info(const char *tag, const uint32_t *v, uint32_t n)
{
    char     line[200];
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

/* R 组只测自机与规则：初始化之后把事件表的游标放到表尾，关卡不出敌、不进 Boss（关卡在 S 组）。 */
static void game_init(stg_game *g)
{
    stg_game_init(g, SEED);
    g->ev_i = stg_stage_n_events;
}

/* 初始化并走完 120 帧标题：下一次 stg_game_step 是开局第一帧。 */
static void to_play(stg_game *g, uint32_t keys)
{
    uint32_t i;

    game_init(g);
    for (i = 0u; i < STG_TITLE_FRAMES; i++)
        stg_game_step(g, keys);
}

/* 在自机中心放一颗静止的敌弹（弹种 0，判定半径 4 像素）。 */
static void bullet_on_player(stg_game *g)
{
    (void)stg_pool_add(&g->f.pools.ebullet, g->f.px, g->f.py, 0, 0, 0u, 0u);
}

/* 池里没有标记移除的实体数。 */
static uint32_t live(const stg_pool *p)
{
    uint32_t i, k = 0u;
    stg_ent  e;

    for (i = 0u; stg_pool_get(p, i, &e) == 0; i++)
        if ((e.flags & STG_F_REMOVE) == 0u)
            k++;
    return k;
}

/* --- R1 移动 --------------------------------------------------------------- */

static void r1_move(void)
{
    static const uint32_t dir_keys[8] = {
        STG_KEY_UP, STG_KEY_DOWN, STG_KEY_LEFT, STG_KEY_RIGHT,
        STG_KEY_UP | STG_KEY_LEFT, STG_KEY_UP | STG_KEY_RIGHT,
        STG_KEY_DOWN | STG_KEY_LEFT, STG_KEY_DOWN | STG_KEY_RIGHT,
    };
    static const int32_t dir_x[8] = { 0, 0, -1, 1, -1, 1, -1, 1 };
    static const int32_t dir_y[8] = { -1, 1, 0, 0, -1, -1, 1, 1 };
    stg_game *g = &s_g;
    int       ok8 = 1, okopp = 1, okclamp = 1, oktitle;
    uint32_t  d, s, i;

    for (s = 0u; s < 2u; s++) {
        /* 期望：直行 v，斜行 v * 181 / 256 截断（v 为正） */
        const int32_t v  = (s == 0u) ? 1024 : 512;
        const int32_t vd = (v * 181) / 256;

        for (d = 0u; d < 8u; d++) {
            const int32_t  vv = (dir_x[d] != 0 && dir_y[d] != 0) ? vd : v;
            const uint32_t k = dir_keys[d] | ((s == 1u) ? STG_KEY_SLOW : 0u);

            to_play(g, 0u);
            stg_game_step(g, k);
            if (g->f.px != 200 * FX_ONE + dir_x[d] * vv || g->f.py != 400 * FX_ONE + dir_y[d] * vv
                || g->slow != s)
                ok8 = 0;
        }
    }
    ck("R1 8 directions x fast/slow: axis 1024/512, diagonal 724/362 per axis", ok8);

    to_play(g, 0u);
    stg_game_step(g, STG_KEY_UP | STG_KEY_DOWN | STG_KEY_LEFT);
    okopp &= (g->f.px == 200 * FX_ONE - 1024 && g->f.py == 400 * FX_ONE);
    stg_game_step(g, STG_KEY_LEFT | STG_KEY_RIGHT);
    okopp &= (g->f.px == 200 * FX_ONE - 1024 && g->f.py == 400 * FX_ONE);
    stg_game_step(g, STG_KEY_UP | STG_KEY_DOWN | STG_KEY_LEFT | STG_KEY_RIGHT);
    okopp &= (g->f.px == 200 * FX_ONE - 1024 && g->f.py == 400 * FX_ONE);
    ck("R1 opposite directions cancel on that axis (up+down+left = left)", okopp);

    to_play(g, 0u);
    for (i = 0u; i < 200u; i++) {
        stg_game_step(g, STG_KEY_UP | STG_KEY_LEFT);
        if (g->f.px < 16 * FX_ONE || g->f.py < 16 * FX_ONE)
            okclamp = 0;
    }
    okclamp &= (g->f.px == 16 * FX_ONE && g->f.py == 16 * FX_ONE);
    for (i = 0u; i < 400u; i++) {
        stg_game_step(g, STG_KEY_DOWN | STG_KEY_RIGHT | STG_KEY_SLOW);
        if (g->f.px > 384 * FX_ONE || g->f.py > 464 * FX_ONE)
            okclamp = 0;
    }
    okclamp &= (g->f.px == 384 * FX_ONE && g->f.py == 464 * FX_ONE);
    ck("R1 clamp to [16, 384] x [16, 464] px at both corners", okclamp);

    game_init(g);
    for (i = 0u; i < STG_TITLE_FRAMES; i++)
        stg_game_step(g, STG_KEY_RIGHT);
    oktitle = (g->phase == STG_PH_TITLE && g->f.px == 200 * FX_ONE);
    stg_game_step(g, STG_KEY_RIGHT);
    oktitle &= (g->phase == STG_PH_PLAY && g->f.px == 200 * FX_ONE + 1024 && (g->ev & STG_EV_START) != 0u);
    ck("R1 no movement during the 120 title frames, moves on the first play frame", oktitle);
}

/* --- R2 射击 --------------------------------------------------------------- */

static void r2_shoot(void)
{
    stg_game *g = &s_g;
    stg_ent   a, b;
    uint32_t  i, t, ok_title = 1u, ok_count = 1u, n_max = 0u;
    int       ok_first;

    game_init(g);
    for (i = 0u; i < STG_TITLE_FRAMES; i++) {
        stg_game_step(g, 0u);
        if (stg_pool_count(&g->f.pools.pbullet) != 0u)
            ok_title = 0u;
    }
    ck("R2 no shots during the title", ok_title != 0u);

    stg_game_step(g, 0u);
    ok_first = stg_pool_count(&g->f.pools.pbullet) == 2u
        && stg_pool_get(&g->f.pools.pbullet, 0u, &a) == 0 && stg_pool_get(&g->f.pools.pbullet, 1u, &b) == 0
        && a.x == 193 * FX_ONE && a.y == 384 * FX_ONE && b.x == 207 * FX_ONE && b.y == 384 * FX_ONE
        && a.vx == 0 && a.vy == -2816 && b.vx == 0 && b.vy == -2816
        && a.flags == STG_KIND_PSHOT && b.flags == STG_KIND_PSHOT;
    ck("R2 first play frame: 2 beams at (193, 384) and (207, 384), vy -2816, kind 12", ok_first);

    /* 开局第 t 帧（从 0 起）：第 f 帧（f 是 4 的倍数）发的弹已走 t - f 步，y = 384 - 11 (t - f)，
       y >= -12 像素还在池里（自机弹的边界外扩 12 像素）。 */
    for (t = 1u; t < 400u; t++) {
        uint32_t f, want = 0u;

        stg_game_step(g, 0u);
        for (f = 0u; f <= t; f += 4u)
            if (384 - 11 * (int32_t)(t - f) >= -12)
                want += 2u;
        if (stg_pool_count(&g->f.pools.pbullet) != want)
            ok_count = 0u;
        if (want > n_max)
            n_max = want;
    }
    ck("R2 400 frames: every 4 frames, count == independent y >= -12 px count", ok_count != 0u);
    info("R2 steady beams (max)", &n_max, 1u);
}

/* --- R3 受击与无敌 --------------------------------------------------------- */

static void r3_hit(void)
{
    stg_game *g = &s_g;
    uint32_t  i, gap = 0u, hidden = 0u, graze0;
    int       ok_lost;

    to_play(g, 0u);
    stg_game_step(g, 0u);
    g->bombs = 0u;
    bullet_on_player(g);
    stg_game_step(g, 0u);
    ok_lost = (g->ev & (STG_EV_HIT | STG_EV_LOST)) == (STG_EV_HIT | STG_EV_LOST)
        && (g->ev & STG_EV_BOMB) == 0u
        && g->lives == 2u && g->inv == STG_INV_HIT && g->bombs == STG_BOMBS_INIT && g->flash == 1u;
    ck("R3 hit without bombs: life 3 -> 2, flash, inv 120, bombs refilled to 3", ok_lost);

    /* 炸弹再清零（不让自动炸弹抵掉），自机上再放一颗静止的弹：无敌期间只算擦弹 */
    g->bombs = 0u;
    graze0 = g->graze;
    bullet_on_player(g);
    if (!stg_game_player_visible(g))
        hidden++;
    for (i = 1u; i <= 400u; i++) {
        stg_game_step(g, 0u);
        if (!stg_game_player_visible(g))
            hidden++;
        if ((g->ev & STG_EV_LOST) != 0u) {
            gap = i;
            break;
        }
    }
    ck("R3 invincible exactly 120 frames: next hit on the 121st frame", gap == 121u && g->lives == 1u);
    ck("R3 bullet resting on the player during invincibility: grazed once", g->graze == graze0 + 1u);
    ck("R3 blink: hidden on 60 frames of the invincibility (inv & 4)", hidden == 60u);
    {
        uint32_t v[2];

        v[0] = gap;
        v[1] = hidden;
        info("R3 gap hidden", v, 2u);
    }
}

/* --- R4 自动炸弹 ----------------------------------------------------------- */

static void r4_autobomb(void)
{
    stg_game *g = &s_g;
    stg_ent   e;
    uint32_t  i, gap = 0u;
    int       ok, ok_sp = 1;

    to_play(g, 0u);
    stg_game_step(g, 0u);
    for (i = 0u; i < 10u; i++)
        (void)stg_pool_add(&g->f.pools.ebullet, (20 + 30 * (int32_t)i) * FX_ONE, 50 * FX_ONE, 0, 0, 4u, 4u);
    (void)stg_pool_add(&g->f.pools.ebullet, 300 * FX_ONE, 60 * FX_ONE, 0, 0, 0u, STG_F_NOCANCEL);
    bullet_on_player(g);
    stg_game_step(g, 0u);
    ok = (g->ev & (STG_EV_HIT | STG_EV_AUTOBOMB | STG_EV_BOMB | STG_EV_LOST))
            == (STG_EV_HIT | STG_EV_AUTOBOMB | STG_EV_BOMB)
        && g->lives == 3u && g->bombs == 2u && g->inv == STG_INV_BOMB && g->flash == 0u
        && g->cancelled == 10u && live(&g->f.pools.ebullet) == 1u
        && stg_pool_count(&g->f.pools.particle) == 10u;
    ck("R4 auto bomb: no life lost, bombs 3 -> 2, 10 cancelled, NOCANCEL kept, 10 sparks, inv 180", ok);

    /* 火花：原位出生、帧号 16；之后每帧向上 1 像素、寿命减一，第 16 帧打标记，第 17 帧移除 */
    ok_sp &= (stg_pool_get(&g->f.pools.particle, 3u, &e) == 0 && e.x == 110 * FX_ONE && e.y == 50 * FX_ONE
              && e.sprite == 16u && e.flags == STG_KIND_SPARK && e.vy == -FX_ONE);
    stg_game_step(g, 0u);
    ok_sp &= (stg_pool_count(&g->f.pools.ebullet) == 1u);
    ok_sp &= (stg_pool_get(&g->f.pools.particle, 3u, &e) == 0 && e.y == 49 * FX_ONE && e.sprite == 15u);
    for (i = 2u; i <= 15u; i++) {
        stg_game_step(g, 0u);
        ok_sp &= (live(&g->f.pools.particle) == 10u);
    }
    stg_game_step(g, 0u);                           /* 第 16 帧：寿命到 0，打标记 */
    ok_sp &= (stg_pool_count(&g->f.pools.particle) == 10u && live(&g->f.pools.particle) == 0u);
    stg_game_step(g, 0u);                           /* 第 17 帧：压缩时移除 */
    ok_sp &= (stg_pool_count(&g->f.pools.particle) == 0u);
    ck("R4 sparks: born in place, up 1 px/frame, live 16 frames, removed on the 17th", ok_sp);

    /* 炸弹之后无敌 180 帧：自机上放一颗静止的弹，第 181 帧（从炸弹那一帧算）再次受击 */
    bullet_on_player(g);
    for (i = 18u; i <= 400u; i++) {
        stg_game_step(g, 0u);
        if ((g->ev & STG_EV_HIT) != 0u) {
            gap = i;
            break;
        }
    }
    ck("R4 after a bomb: invincible 180 frames, next hit on the 181st, auto bomb again",
       gap == 181u && g->bombs == 1u && g->lives == 3u && (g->ev & STG_EV_AUTOBOMB) != 0u);
}

/* --- R5 手动炸弹 ----------------------------------------------------------- */

static void r5_manual(void)
{
    stg_game *g = &s_g;
    uint32_t  i, n = 0u;
    int       ok;

    to_play(g, 0u);
    for (i = 0u; i < 10u; i++) {
        stg_game_step(g, STG_KEY_BOMB);
        n += ((g->ev & STG_EV_BOMB) != 0u) ? 1u : 0u;
    }
    ok = (n == 1u && g->bombs == 2u);
    stg_game_step(g, 0u);
    stg_game_step(g, STG_KEY_BOMB);
    ok &= ((g->ev & STG_EV_BOMB) != 0u && (g->ev & STG_EV_AUTOBOMB) == 0u && g->bombs == 1u);
    ck("R5 manual bomb on the press edge only: held 10 frames = 1 bomb, re-press = 1 more", ok);

    to_play(g, STG_KEY_BOMB);
    stg_game_step(g, STG_KEY_BOMB);
    ok = (g->phase == STG_PH_PLAY && g->bombs == 3u && (g->ev & STG_EV_BOMB) == 0u);
    to_play(g, 0u);
    stg_game_step(g, 0u);
    g->bombs = 0u;
    stg_game_step(g, STG_KEY_BOMB);
    ok &= (g->bombs == 0u && (g->ev & STG_EV_BOMB) == 0u && g->inv == 0u);
    ck("R5 key held through the title is not a press; no bomb when bombs == 0", ok);
}

/* --- R6 消弹与粒子溢出 ----------------------------------------------------- */

static void r6_cancel_many(void)
{
    stg_game *g = &s_g;
    uint32_t  i, v[3];
    int       ok;

    to_play(g, 0u);
    stg_game_step(g, 0u);
    for (i = 0u; i < 3000u; i++)
        (void)stg_pool_add(&g->f.pools.ebullet, (int32_t)(8u + (i % 50u) * 8u) * FX_ONE,
                           (int32_t)(8u + (i / 50u) * 4u) * FX_ONE, 0, 0, 0u, 0u);
    stg_game_step(g, STG_KEY_BOMB);
    ok = (g->cancelled == 3000u && stg_pool_count(&g->f.pools.particle) == STG_CAP_PARTICLE
          && stg_pool_overflow(&g->f.pools.particle) == 3000u - STG_CAP_PARTICLE
          && stg_pool_overflow(&g->f.pools.ebullet) == 0u);
    v[0] = g->cancelled;
    v[1] = stg_pool_count(&g->f.pools.particle);
    v[2] = stg_pool_overflow(&g->f.pools.particle);
    stg_game_step(g, 0u);
    ok &= (stg_pool_count(&g->f.pools.ebullet) == 0u);
    ck("R6 bomb on 3000 bullets: 3000 cancelled, 2048 sparks, particle overflow 952, ebullet overflow 0", ok);
    info("R6 cancelled sparks particle_overflow", v, 3u);
}

/* --- R7 残机用完 ----------------------------------------------------------- */

static void r7_over(void)
{
    stg_game *g = &s_g;
    uint32_t  i, gap = 0u;
    int       ok;

    to_play(g, 0u);
    stg_game_step(g, STG_KEY_LEFT);
    g->lives = 1u;
    g->bombs = 0u;
    g->score = 1234u;
    bullet_on_player(g);
    stg_game_step(g, STG_KEY_LEFT);
    ok = (g->ev & (STG_EV_LOST | STG_EV_OVER)) == (STG_EV_LOST | STG_EV_OVER)
        && g->phase == STG_PH_TITLE && g->lives == 3u && g->bombs == 3u && g->score == 0u
        && g->hiscore == 1234u && g->inv == 0u
        && stg_pool_count(&g->f.pools.ebullet) == 0u && stg_pool_count(&g->f.pools.pbullet) == 0u
        && g->f.px == 200 * FX_ONE && g->f.py == 400 * FX_ONE;
    ck("R7 last life lost: back to title, lives/bombs 3, score 0, HI-SCORE 1234 kept, pools empty", ok);

    for (i = 1u; i <= 300u; i++) {
        stg_game_step(g, 0u);
        if ((g->ev & STG_EV_START) != 0u) {
            gap = i;
            break;
        }
    }
    ck("R7 title shown 120 frames (game-over frame included), play starts on the next",
       gap == STG_TITLE_FRAMES && g->phase == STG_PH_PLAY && g->hiscore == 1234u);

    stg_game_init(g, SEED);
    ck("R7 stg_game_init clears HI-SCORE", g->hiscore == 0u && g->phase == STG_PH_TITLE);
}

/* --- R8 擦弹得分 ----------------------------------------------------------- */

static void r8_graze(void)
{
    stg_game *g = &s_g;
    int32_t   dx, dy;
    uint32_t  want = 0u, v[3], score1;
    int       ok;

    to_play(g, 0u);
    stg_game_step(g, 0u);
    stg_game_set_hits(g, 0);
    /* 自机周围 ±24 像素、步长 3 像素放一片静止弹（dy 是 4 的倍数的那几行 x 再错开 1/256 像素），弹种 0 与 4 交替。
       期望：d^2 < (16 px + r_b)^2 为擦弹，64 位整数另算。 */
    for (dy = -24; dy <= 24; dy += 3) {
        for (dx = -24; dx <= 24; dx += 3) {
            const uint32_t kind = (((dx + dy) & 1) != 0) ? 4u : 0u;
            const int64_t  r = 16 * FX_ONE + (int64_t)stg_kind_tab[kind].r_hit;
            const int64_t  qx = (int64_t)dx * FX_ONE + ((dy & 3) == 0 ? 1 : 0);
            const int64_t  qy = (int64_t)dy * FX_ONE;

            (void)stg_pool_add(&g->f.pools.ebullet, g->f.px + (int32_t)qx, g->f.py + (int32_t)qy,
                               0, 0, 0u, (uint16_t)kind);
            if (qx * qx + qy * qy < r * r)
                want++;
        }
    }
    stg_game_step(g, 0u);
    score1 = g->score;
    ok = (g->graze == want && g->score == want * STG_SCORE_GRAZE && g->lives == 3u && g->bombs == 3u
          && g->hiscore == g->score);
    stg_game_step(g, 0u);
    ok &= (g->graze == want && g->score == score1);
    ck("R8 hits off: graze count == independent 64-bit count, score == 10 x graze, no double count", ok);
    v[0] = stg_pool_count(&g->f.pools.ebullet);
    v[1] = want;
    v[2] = g->score;
    info("R8 bullets grazes score", v, 3u);
}

/* --- R9 自机弹对敌机 ------------------------------------------------------- */

typedef struct {
    uint32_t hits[4];
    uint16_t flags;
} tgt_res;

static tgt_res hit_one(const stg_target *tg, uint32_t n, int32_t x, int32_t y, uint16_t flags)
{
    stg_pool p;
    int32_t  r[STG_KIND_N_MAX];
    stg_ent  e;
    tgt_res  res;
    uint32_t i;

    stg_kind_radii(r);
    stg_pool_init(&p, s_mem, 256u);
    (void)stg_pool_add(&p, x, y, 0, -2816, 0u, flags);
    if ((flags & STG_F_REMOVE) != 0u)
        stg_pool_mark(&p, 0u);
    for (i = 0u; i < 4u; i++)
        res.hits[i] = 0u;
    (void)stg_pool_hit_targets(&p, r, tg, n, res.hits);
    (void)stg_pool_get(&p, 0u, &e);
    res.flags = e.flags;
    return res;
}

static void target(stg_target *t, int32_t x, int32_t y, int32_t r, int32_t alive)
{
    t->x     = x;
    t->y     = y;
    t->r     = r;
    t->alive = alive;
}

static void r9_targets(void)
{
    const int32_t  cx = 200 * FX_ONE, cy = 150 * FX_ONE;
    const uint16_t ps = (uint16_t)STG_KIND_PSHOT;
    stg_target     tg[4];
    tgt_res        a;
    int            tangent_ok = 1, closer_ok = 1, ok;
    int            d;

    /* 目标半径 10 像素、自机弹 3 像素：T = 13 像素 = 3328 */
    target(&tg[0], cx, cy, 10 * FX_ONE, 1);
    for (d = 0; d < 4; d++) {
        const int32_t sx = (d == 0) ? 1 : (d == 1) ? -1 : 0;
        const int32_t sy = (d == 2) ? 1 : (d == 3) ? -1 : 0;
        const int32_t t = 13 * FX_ONE;

        a = hit_one(tg, 1u, cx + sx * t, cy + sy * t, ps);
        tangent_ok &= (a.hits[0] == 0u && a.flags == ps);
        a = hit_one(tg, 1u, cx + sx * (t - 1), cy + sy * (t - 1), ps);
        closer_ok &= (a.hits[0] == 1u && (a.flags & STG_F_REMOVE) != 0u);
    }
    /* 勾股数：目标半径 1792（7 像素），T = 2560 = 5 x 512，(1536, 2048) 恰好相切 */
    target(&tg[1], cx, cy, 1792, 1);
    for (d = 0; d < 4; d++) {
        const int32_t sx = (d & 1) ? -1 : 1;
        const int32_t sy = (d & 2) ? -1 : 1;

        a = hit_one(&tg[1], 1u, cx + sx * 1536, cy + sy * 2048, ps);
        tangent_ok &= (a.hits[0] == 0u && a.flags == ps);
        a = hit_one(&tg[1], 1u, cx + sx * 1536, cy + sy * 2047, ps);
        closer_ok &= (a.hits[0] == 1u && (a.flags & STG_F_REMOVE) != 0u);
    }
    ck("R9 tangent (T = 3328 in 4 directions, 3-4-5 with T = 2560): no hit", tangent_ok);
    ck("R9 one step closer: hit, bullet marked", closer_ok);

    /* 两个目标重叠：只打第一个；第一个存活为 0 就打第二个；两个都为 0 穿过去 */
    target(&tg[0], cx, cy, 14 * FX_ONE, 1);
    target(&tg[1], cx + FX_ONE, cy, 32 * FX_ONE, 1);
    a = hit_one(tg, 2u, cx, cy, ps);
    ok = (a.hits[0] == 1u && a.hits[1] == 0u);
    tg[0].alive = 0;
    a = hit_one(tg, 2u, cx, cy, ps);
    ok &= (a.hits[0] == 0u && a.hits[1] == 1u);
    tg[1].alive = 0;
    a = hit_one(tg, 2u, cx, cy, ps);
    ok &= (a.hits[0] == 0u && a.hits[1] == 0u && a.flags == ps);
    ck("R9 one bullet hits only the first target; alive == 0 is passed through", ok);

    target(&tg[0], cx, cy, 14 * FX_ONE, 1);
    a = hit_one(tg, 1u, cx, cy, (uint16_t)(ps | STG_F_REMOVE));
    ck("R9 marked bullet is skipped", a.hits[0] == 0u);

    /* 随机样本：64 发 x 8 个目标 x 300 轮，对 64 位平方、不粗筛的朴素参考。
       一半轮次坐标落在整像素网格上，恰好相切大量出现；坐标覆盖整个出界边界。 */
    {
        xs32       rng;
        stg_pool   p, q;
        stg_target tt[8];
        int32_t    r[STG_KIND_N_MAX];
        uint32_t   hits[8], ref[8];
        uint32_t   round, i, t, all_ok = 1u, total = 0u, tangent = 0u;
        int32_t    r_t, alive;

        stg_kind_radii(r);
        xs32_seed(&rng, 0x5E7B0C09u);
        for (round = 0u; round < 300u; round++) {
            const int grid = (round & 1u) == 0u;

            stg_pool_init(&p, s_mem, 256u);
            stg_pool_init(&q, s_ref, 256u);
            for (t = 0u; t < 8u; t++) {
                int32_t x = (int32_t)(xs32_next(&rng) % 424u) - 12;
                int32_t y = (int32_t)(xs32_next(&rng) % 504u) - 12;

                if (!grid) {
                    x = x * FX_ONE + (int32_t)(xs32_next(&rng) & 255u);
                    y = y * FX_ONE + (int32_t)(xs32_next(&rng) & 255u);
                } else {
                    x *= FX_ONE;
                    y *= FX_ONE;
                }
                /* 取数放进局部量：函数实参的求值顺序不定，64 位与 -m32 可能不同 */
                r_t   = (int32_t)(4u + xs32_next(&rng) % 29u) * FX_ONE;
                alive = (xs32_next(&rng) % 5u) != 0u;
                target(&tt[t], x, y, r_t, alive);
                hits[t] = 0u;
                ref[t]  = 0u;
            }
            for (i = 0u; i < 64u; i++) {
                /* 四分之一的弹恰好与某个目标相切或再近 1/256 像素（轴向，或 T 是 5 的倍数时按 3-4-5），
                   四分之一落在某个目标附近，其余散在整个出界边界里 */
                const stg_target *near = &tt[xs32_next(&rng) & 7u];
                const uint32_t    mode = xs32_next(&rng) & 3u;
                int32_t           x, y;
                uint16_t          fl = ps;

                if (mode == 0u) {
                    const int32_t  tt_sum = r[STG_KIND_PSHOT] + near->r;
                    const uint32_t w = xs32_next(&rng);
                    int32_t        ox = tt_sum, oy = 0, sw;

                    if ((tt_sum % 5) == 0 && (w & 4u) != 0u) {
                        ox = 3 * (tt_sum / 5);
                        oy = 4 * (tt_sum / 5);
                    }
                    if ((w & 16u) != 0u)
                        ox--;               /* 再近 1/256 像素 */
                    if ((w & 8u) != 0u) {
                        sw = ox;
                        ox = oy;
                        oy = sw;
                    }
                    x = near->x + (((w & 1u) != 0u) ? -ox : ox);
                    y = near->y + (((w & 2u) != 0u) ? -oy : oy);
                } else if (mode == 1u) {
                    x = near->x + (int32_t)(xs32_next(&rng) % 81u) * (grid ? FX_ONE : 64) - 40 * (grid ? FX_ONE : 64);
                    y = near->y + (int32_t)(xs32_next(&rng) % 81u) * (grid ? FX_ONE : 64) - 40 * (grid ? FX_ONE : 64);
                } else {
                    x = ((int32_t)(xs32_next(&rng) % 424u) - 12) * FX_ONE;
                    y = ((int32_t)(xs32_next(&rng) % 504u) - 12) * FX_ONE;
                }
                if ((xs32_next(&rng) & 15u) == 0u)
                    fl = (uint16_t)(fl | STG_F_REMOVE);
                (void)stg_pool_add(&p, x, y, 0, 0, 0u, fl);
                (void)stg_pool_add(&q, x, y, 0, 0, 0u, fl);
                if ((fl & STG_F_REMOVE) != 0u) {
                    stg_pool_mark(&p, i);
                    stg_pool_mark(&q, i);
                }
            }
            (void)stg_pool_hit_targets(&p, r, tt, 8u, hits);

            /* 朴素参考：逐颗、按目标顺序，64 位平方，不粗筛 */
            for (i = 0u; i < 64u; i++) {
                stg_ent e;

                (void)stg_pool_get(&q, i, &e);
                if ((e.flags & STG_F_REMOVE) != 0u)
                    continue;
                for (t = 0u; t < 8u; t++) {
                    const int64_t dx = (int64_t)e.x - tt[t].x;
                    const int64_t dy = (int64_t)e.y - tt[t].y;
                    const int64_t s = (int64_t)r[STG_KIND_PSHOT] + tt[t].r;

                    if (tt[t].alive == 0)
                        continue;
                    if (dx * dx + dy * dy == s * s)
                        tangent++;
                    if (dx * dx + dy * dy < s * s) {
                        ref[t]++;
                        stg_pool_mark(&q, i);
                        break;
                    }
                }
            }
            for (t = 0u; t < 8u; t++) {
                if (hits[t] != ref[t])
                    all_ok = 0u;
                total += hits[t];
            }
            for (i = 0u; i < 64u; i++) {
                stg_ent e1, e2;

                (void)stg_pool_get(&p, i, &e1);
                (void)stg_pool_get(&q, i, &e2);
                if (e1.flags != e2.flags)
                    all_ok = 0u;
            }
        }
        ck("R9 300 random rounds vs 64-bit naive reference: hits per target and flags equal", all_ok != 0u);
        {
            uint32_t v[2];

            v[0] = total;
            v[1] = tangent;
            info("R9 totals hits tangent_pairs", v, 2u);
        }
    }
}

/* --- R10 粒子寿命与弹种位图 ------------------------------------------------ */

static void r10_age_kinds(void)
{
    stg_pool   p;
    stg_bounds b;
    stg_ent    e;
    int        ok;

    stg_pool_init(&p, s_mem, 256u);
    (void)stg_pool_add(&p, 0, 0, 0, 0, 3u, 13u);
    (void)stg_pool_add(&p, 0, 0, 0, 0, 1u, 13u);
    (void)stg_pool_add(&p, 0, 0, 0, 0, 0u, 13u);
    (void)stg_pool_add(&p, 0, 0, 0, 0, 5u, 13u);
    stg_pool_mark(&p, 3u);
    stg_pool_age(&p);
    ok = (stg_pool_get(&p, 0u, &e) == 0 && e.sprite == 2u && (e.flags & STG_F_REMOVE) == 0u);
    ok &= (stg_pool_get(&p, 1u, &e) == 0 && e.sprite == 0u && (e.flags & STG_F_REMOVE) != 0u);
    ok &= (stg_pool_get(&p, 2u, &e) == 0 && e.sprite == 0u && (e.flags & STG_F_REMOVE) != 0u);
    ok &= (stg_pool_get(&p, 3u, &e) == 0 && e.sprite == 5u);
    stg_bounds_field(&b);
    stg_pool_step(&p, &b, 0);
    ok &= (stg_pool_count(&p) == 1u);
    stg_pool_age(&p);
    stg_pool_age(&p);
    stg_pool_step(&p, &b, 0);
    ok &= (stg_pool_count(&p) == 0u);
    ck("R10 age: life 3/1/0 -> 2/marked/marked, marked one untouched, removed by the next step", ok);

    stg_pool_init(&p, s_mem, 256u);
    ok = (stg_pool_kinds(&p) == 0u);
    (void)stg_pool_add(&p, 10 * FX_ONE, 10 * FX_ONE, 0, 0, 0u, 0u);
    (void)stg_pool_add(&p, 10 * FX_ONE, 10 * FX_ONE, 0, -30 * FX_ONE, 0u, 4u);
    ok &= (stg_pool_kinds(&p) == 0x11u);
    stg_pool_step(&p, &b, 0);                   /* 弹种 4 出了上沿 */
    ok &= (stg_pool_kinds(&p) == 0x01u && stg_pool_count(&p) == 1u);
    (void)stg_pool_add(&p, 10 * FX_ONE, 10 * FX_ONE, 0, 0, 0u, 12u);
    ok &= (stg_pool_kinds(&p) == 0x1001u);
    stg_pool_mark(&p, 0u);
    stg_pool_mark(&p, 1u);
    stg_pool_step(&p, &b, 0);
    ok &= (stg_pool_kinds(&p) == 0u);
    ck("R10 kinds bitmask: add ORs in, step keeps only survivors' kinds, empty pool is 0", ok);
}

uint32_t test_stg_game_run(void)
{
    g_fail = 0u;
    r1_move();
    r2_shoot();
    r3_hit();
    r4_autobomb();
    r5_manual();
    r6_cancel_many();
    r7_over();
    r8_graze();
    r9_targets();
    r10_age_kinds();
    return g_fail;
}
