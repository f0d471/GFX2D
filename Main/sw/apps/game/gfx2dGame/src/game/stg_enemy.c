/******************************************************************************
* @file stg_enemy.c: 敌机的槽位表
*
* @brief  口径见 stg_enemy.h 与 sw 第 06 章 3.11 节。全部是整数运算，没有浮点、没有 malloc、
*         没有文件级的可变状态。每帧扫 64 个槽，不在弹的逐颗循环里。
******************************************************************************/

#include "stg_enemy.h"

#include "stg_kind.h"

/* 机种表（2.4 节）：DART 血量 3、半径 10 像素、100 分；CROSS 血量 12、半径 14 像素、300 分。
   画图的第一帧：ships.png 4 列，第 0 格是自机，DART 占 4~7，CROSS 占 8~11（2.7 节）。 */
const stg_etype stg_etype_tab[STG_ETYPE_N] = {
    [STG_ETYPE_DART]  = { 10 * FX_ONE,  3, 100u, 4u },
    [STG_ETYPE_CROSS] = { 14 * FX_ONE, 12, 300u, 8u },
};

void stg_enemies_init(stg_enemies *s)
{
    uint32_t i;

    for (i = 0u; i < STG_ENEMY_SLOTS; i++)
        s->e[i].alive = 0u;
    s->spawn_overflow = 0u;
}

int stg_enemy_spawn(stg_enemies *s, uint32_t type, int32_t x, int32_t y,
                    const stg_move *mv, uint32_t mv_n,
                    const stg_emit_rec *const *recs, uint32_t n_em, uint32_t delay)
{
    stg_enemy *e = 0;
    uint32_t   i;

    if (type >= STG_ETYPE_N || mv == 0 || mv_n == 0u || n_em > STG_ENEMY_EM_MAX)
        return -1;
    for (i = 0u; i < STG_ENEMY_SLOTS; i++) {
        if (s->e[i].alive == 0u) {
            e = &s->e[i];
            break;
        }
    }
    if (e == 0) {
        s->spawn_overflow++;
        return -1;
    }
    for (i = 0u; i < n_em; i++) {
        if (stg_emitter_init(&e->em[i], recs[i], x, y) != 0)
            return -1;
        e->em[i].wait = delay;
    }
    e->alive   = 1u;
    e->type    = type;
    e->x       = x;
    e->y       = y;
    e->hp      = stg_etype_tab[type].hp;
    e->mv      = mv;
    e->mv_n    = mv_n;
    e->mv_i    = 0u;
    e->vx      = mv[0].dvx;
    e->vy      = mv[0].dvy;
    e->mv_left = mv[0].frames;
    e->n_em    = n_em;
    e->age     = 0u;
    return (int)(e - s->e);
}

/* 走一步：位置加速度；这一步的帧数走完就进入下一步（加上它的速度增量），最后一步走完保持速度。 */
static void enemy_move(stg_enemy *e)
{
    e->x += e->vx;
    e->y += e->vy;
    if (e->mv_left != 0u) {
        e->mv_left--;
        if (e->mv_left == 0u && e->mv_i + 1u < e->mv_n) {
            e->mv_i++;
            e->vx += e->mv[e->mv_i].dvx;
            e->vy += e->mv[e->mv_i].dvy;
            e->mv_left = e->mv[e->mv_i].frames;
        }
    }
}

static int enemy_out(const stg_enemy *e)
{
    const int32_t lo = -STG_ENEMY_MARGIN_PX * FX_ONE;

    return e->x < lo || e->x >= (STG_FIELD_W + STG_ENEMY_MARGIN_PX) * FX_ONE
        || e->y < lo || e->y >= (STG_FIELD_H + STG_ENEMY_MARGIN_PX) * FX_ONE;
}

uint32_t stg_enemies_step(stg_enemies *s, stg_pool *ebullet, const stg_emit_ctx *ctx)
{
    uint32_t i, j, out = 0u;

    for (i = 0u; i < STG_ENEMY_SLOTS; i++) {
        stg_enemy *e = &s->e[i];

        if (e->alive == 0u)
            continue;
        for (j = 0u; j < e->n_em; j++) {
            e->em[j].x = e->x;
            e->em[j].y = e->y;
            (void)stg_emitter_tick(&e->em[j], ebullet, ctx);
        }
        enemy_move(e);
        e->age++;
        if (enemy_out(e)) {
            e->alive = 0u;
            out++;
        }
    }
    return out;
}

void stg_enemies_targets(const stg_enemies *s, stg_target *tg)
{
    uint32_t i;

    for (i = 0u; i < STG_ENEMY_SLOTS; i++) {
        const stg_enemy *e = &s->e[i];

        tg[i].x     = e->x;
        tg[i].y     = e->y;
        tg[i].r     = stg_etype_tab[e->type < STG_ETYPE_N ? e->type : 0u].r_hit;
        tg[i].alive = (int32_t)e->alive;
    }
}

uint32_t stg_enemies_settle(stg_enemies *s, const uint32_t *hits, stg_pool *particle,
                            uint32_t *score, uint32_t kills[STG_ETYPE_N])
{
    uint32_t i, k, dead = 0u;

    for (i = 0u; i < STG_ENEMY_SLOTS; i++) {
        stg_enemy *e = &s->e[i];

        if (e->alive == 0u || hits[i] == 0u)
            continue;
        e->hp -= (int32_t)hits[i];
        if (e->hp > 0)
            continue;
        e->alive = 0u;
        *score += stg_etype_tab[e->type].score;
        kills[e->type]++;
        dead++;
        for (k = 0u; k < STG_KILL_SPARKS; k++) {
            int32_t vx, vy;

            fx_vel(STG_KILL_SPARK_V, (uint16_t)(k * (65536u / STG_KILL_SPARKS)), &vx, &vy);
            (void)stg_pool_add(particle, e->x, e->y, vx, vy, (uint16_t)STG_KILL_SPARK_LIFE,
                               (uint16_t)STG_KIND_SPARK);
        }
    }
    return dead;
}

uint32_t stg_enemies_alive(const stg_enemies *s)
{
    uint32_t i, n = 0u;

    for (i = 0u; i < STG_ENEMY_SLOTS; i++)
        n += s->e[i].alive;
    return n;
}
