/******************************************************************************
* @file stg_game.c: 游戏状态与一帧的顺序
*
* @brief  口径见 stg_game.h。全部是整数运算，没有浮点、没有 malloc、
*         没有文件级的可变状态。关卡的数据在 stg_stage.c 的常量表里，这里按表执行，没有按关卡写死的分支。
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

static void set_seg(stg_game *g, uint32_t seg)
{
    g->seg = seg;
    g->ev |= STG_EV_SEG;
}

/* 一局从标题开始：池清空、随机数重新播种、背景的星点重新生成、敌机与 Boss 清空、事件表回到表头、
   自机回出生点、残机与炸弹补满、得分与擦弹清零。HI-SCORE、钩子、上一帧的按键与帧数不动。 */
static void round_reset(stg_game *g)
{
    uint32_t i;

    stg_field_init(&g->f, g->seed);
    stg_enemies_init(&g->en);
    stg_bg_init(&g->bg, &g->f.rng);
    g->ss         = &stg_stage_classic;
    g->ex         = 0u;
    g->shake      = 0u;
    g->boss.on    = STG_BOSS_OFF;
    g->boss.t     = 0u;
    g->boss.n_em  = 0u;
    g->phase      = STG_PH_TITLE;
    g->phase_t    = 0u;
    g->ev_i       = 0u;
    g->slow       = 0u;
    g->shot_wait  = 0u;
    g->lives      = STG_LIVES_INIT;
    g->bombs      = STG_BOMBS_INIT;
    g->inv        = 0u;
    g->bomb_req   = 0u;
    g->press      = 0u;
    g->score      = 0u;
    g->graze      = 0u;
    g->cancelled  = 0u;
    for (i = 0u; i < STG_ETYPE_N; i++)
        g->kills[i] = 0u;
    set_seg(g, STG_GSEG_TITLE);
}

void stg_game_init(stg_game *g, uint32_t seed)
{
    g->seed       = seed;
    g->frame      = 0u;
    g->keys_prev  = 0u;
    g->flash      = 0u;
    g->hits_on    = 1u;
    g->shoot_on   = 1u;
    g->press_kind = 0u;
    g->knob       = 0u;
    g->hiscore    = 0u;
    g->ev         = 0u;
    pb_bounds(&g->pb_bounds);
    round_reset(g);
    g->ev = 0u;
}

void stg_game_set_hits(stg_game *g, int on)
{
    g->hits_on = (on != 0) ? 1u : 0u;
}

void stg_game_set_knob(stg_game *g, uint32_t knob)
{
    g->knob = knob;
}

const char *stg_game_seg_name(uint32_t seg)
{
    if (seg == STG_GSEG_TITLE)
        return "READY";
    if (seg == STG_GSEG_STAGE)
        return "STAGE";
    if (seg == STG_GSEG_BOSS)
        return "BOSS";
    if (seg >= STG_GSEG_SPELL1 && seg < STG_GSEG_SPELL1 + STG_CARD_N)
        return stg_cards[seg - STG_GSEG_SPELL1].name;
    if (seg == STG_GSEG_CLEAR)
        return "CLEAR";
    return "?";
}

/* HUD 用的段名：EX 路线显示 EX 前缀与 EX 的符卡名；经典路线与上面的判据口径逐字相同。 */
const char *stg_game_seg_name_g(const stg_game *g, uint32_t seg)
{
    if (g->ex == 0u)
        return stg_game_seg_name(seg);
    if (seg == STG_GSEG_STAGE)
        return "EX STAGE";
    if (seg == STG_GSEG_BOSS)
        return "EX BOSS";
    if (seg >= STG_GSEG_SPELL1 && seg < STG_GSEG_SPELL1 + STG_CARD_N)
        return stg_stage_ex.cards[seg - STG_GSEG_SPELL1].name;
    if (seg == STG_GSEG_CLEAR)
        return "EX CLEAR";
    return stg_game_seg_name(seg);
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

/* 消弹：可消除的敌弹打移除标记，原位生成火花（粒子池满了计粒子池的溢出数）。
   炸弹、清弹事件与符卡结束都走这里。 */
static void clear_bullets(stg_game *g)
{
    g->cancelled += stg_pool_cancel(&g->f.pools.ebullet, &g->f.pools.particle, 0, STG_SPARK_VY,
                                    (uint16_t)STG_SPARK_LIFE, (uint16_t)STG_KIND_SPARK);
}

/* --- Boss 与符卡 ------------------------------------------------------------ */

static void boss_begin(stg_game *g)
{
    stg_boss *b = &g->boss;

    b->on   = STG_BOSS_ENTER;
    b->t    = 0u;
    b->card = 0u;
    b->x    = STG_BOSS_X0;
    b->y    = STG_BOSS_Y0;
    b->a    = 0u;
    b->hp   = 0;
    b->n_em = 0u;
    set_seg(g, STG_GSEG_BOSS);
}

/* 发射器的位置：锚点是 Boss 时为 Boss 中心加偏移，绝对时就是表里的位置。 */
static void card_em_pos(const stg_boss *b, const stg_card_em *ce, stg_emitter *em)
{
    if (ce->anchor == STG_ANCHOR_BOSS) {
        em->x = b->x + ce->x;
        em->y = b->y + ce->y;
    } else {
        em->x = ce->x;
        em->y = ce->y;
    }
}

/* 符卡开始：血量、发射器（计时从这一帧起）、门控清零。第一张的段在这里开始，
   其余几张的段从上一张结束（间隙开头）算起。 */
static void card_begin(stg_game *g)
{
    stg_boss       *b = &g->boss;
    const stg_card *c = &g->ss->cards[b->card];
    uint32_t        j;

    b->on   = STG_BOSS_CARD;
    b->t    = 0u;
    b->hp   = c->hp;
    b->n_em = 0u;
    for (j = 0u; j < c->n_em && j < STG_CARD_EM_MAX; j++) {
        if (stg_emitter_init(&b->em[j], c->em[j].rec, 0, 0) != 0)
            break;
        card_em_pos(b, &c->em[j], &b->em[j]);
        b->n_em++;
    }
    b->gate_acc   = 0u;
    b->gate_tok   = 0u;
    b->gate_rr    = 0u;
    b->gate_fired = 0u;
    b->gate_k     = 0u;
    if (b->card == 0u)
        set_seg(g, STG_GSEG_SPELL1);
}

/* 符卡结束：消弹；血量耗尽时奖励 10000 x 序号，最后一张再加 50000；进下一张的间隙或 CLEAR。 */
static void card_end(stg_game *g, int by_hp)
{
    stg_boss *b = &g->boss;

    clear_bullets(g);
    if (by_hp) {
        g->score += STG_CARD_BONUS * (b->card + 1u);
        if (b->card + 1u >= STG_CARD_N)
            g->score += STG_FINAL_BONUS;
        g->ev |= STG_EV_CARD_KILL;
    } else {
        g->ev |= STG_EV_CARD_TIME;
    }
    b->n_em = 0u;
    b->t    = 0u;
    if (b->card + 1u >= STG_CARD_N) {
        b->on      = STG_BOSS_OFF;
        g->phase   = STG_PH_CLEAR;
        g->phase_t = 0u;
        set_seg(g, STG_GSEG_CLEAR);
    } else {
        b->card++;
        b->on = STG_BOSS_GAP;
        set_seg(g, STG_GSEG_SPELL1 + b->card);
    }
}

/* 第 4 步后半：按时间的切换（进场满 74 帧、间隙满、超时）。压力形态下符卡不结束。 */
static void boss_timing(stg_game *g)
{
    stg_boss *b = &g->boss;

    if (b->on == STG_BOSS_ENTER) {
        if (b->t >= STG_BOSS_ENTER_FRAMES)
            card_begin(g);
    } else if (b->on == STG_BOSS_GAP) {
        if (b->t >= g->ss->cards[b->card].gap)
            card_begin(g);
    } else if (b->on == STG_BOSS_CARD) {
#ifdef INJECT_CARD_TIMEOUT_GT
        if (!g->press && b->t > g->ss->cards[b->card].timeout)  /* 注错：超时晚一帧 */
#else
        if (!g->press && b->t >= g->ss->cards[b->card].timeout)
#endif
            card_end(g, 0);
    }
}

/* 终符的门控：每帧 acc += K，每满 125 发一枚令牌；令牌上限是这一帧新增的加 48；
   令牌够下一个发射器一次齐射的发数就让它齐射，发射器按槽内顺序轮流（轮转指针跨帧保留）。 */
static void gate(stg_game *g, const stg_emit_ctx *ctx)
{
    stg_boss *b = &g->boss;
    uint32_t  add = 0u;

    b->gate_k = g->press ? g->knob : STG_GATE_K0 + ((b->t * 3u) >> 2);
    b->gate_acc += b->gate_k;
#ifdef INJECT_GATE_NO_ACC
    b->gate_acc = b->gate_k;                    /* 注错：累加器不跨帧，余数丢掉 */
#endif
    while (b->gate_acc >= STG_GATE_L) {
        b->gate_acc -= STG_GATE_L;
        add++;
    }
#ifdef INJECT_GATE_NO_ACC
    b->gate_acc = 0u;
#endif
    b->gate_tok += add;
    if (b->gate_tok > add + STG_GATE_CARRY)
        b->gate_tok = add + STG_GATE_CARRY;
    while (b->n_em != 0u) {
        stg_emitter   *em = &b->em[b->gate_rr];
        const uint32_t cnt = em->rec->count;
        const uint32_t kind = g->press ? g->press_kind : em->rec->kind;

        if (cnt == 0u || b->gate_tok < cnt)
            break;
        b->gate_fired += stg_emitter_fire(em, &g->f.pools.ebullet, ctx, kind);
        b->gate_tok   -= cnt;
        b->gate_rr     = (b->gate_rr + 1u < b->n_em) ? b->gate_rr + 1u : 0u;
    }
}

/* 第 5 步的 Boss：符卡进行中时发射器在当前位置齐射，再移动。 */
static void boss_step(stg_game *g, const stg_emit_ctx *ctx)
{
    stg_boss *b = &g->boss;
    uint32_t  j;

    if (b->on == STG_BOSS_OFF)
        return;
    if (b->on == STG_BOSS_CARD) {
        const stg_card *c = &g->ss->cards[b->card];

        for (j = 0u; j < b->n_em; j++)
            card_em_pos(b, &c->em[j], &b->em[j]);
        if (c->gated) {
            gate(g, ctx);
        } else {
            for (j = 0u; j < b->n_em; j++)
                (void)stg_emitter_tick(&b->em[j], &g->f.pools.ebullet, ctx);
        }
    }
    if (b->on == STG_BOSS_ENTER) {
        b->y += STG_BOSS_VY_ENTER;
    } else if (!g->press) {
        b->a = (uint16_t)(b->a + STG_BOSS_DA);
        b->x = STG_BOSS_X0 + ((STG_BOSS_AX_PX * fx_sin(b->a)) >> 6);
        b->y = STG_BOSS_HOME_Y + ((STG_BOSS_AY_PX * fx_sin((uint16_t)(2u * b->a))) >> 6);
    }
}

/* --- 事件表 ----------------------------------------------------------------- */

/* 第 4 步前半：取出本帧到期的事件（帧号不大于开局以来的帧数）按表的顺序执行。 */
static void stage_events(stg_game *g)
{
    while (g->ev_i < g->ss->n_events && g->ss->events[g->ev_i].t <= g->phase_t) {
        const stg_event *e = &g->ss->events[g->ev_i];

        g->ev_i++;
        if (e->type == STG_EVT_SPAWN) {
            if (e->move < g->ss->n_moves && e->group < g->ss->n_groups) {
                const stg_move_tab *mv = &g->ss->moves[e->move];
                const stg_em_group *gr = &g->ss->groups[e->group];

                (void)stg_enemy_spawn(&g->en, e->etype, (int32_t)e->x * FX_ONE, (int32_t)e->y * FX_ONE,
                                      mv->steps, mv->n, gr->recs, gr->n, e->delay);
                g->ev |= STG_EV_SPAWN;
            }
        } else if (e->type == STG_EVT_CLEAR) {
            clear_bullets(g);
        } else if (e->type == STG_EVT_BOSS) {
            boss_begin(g);
        }
    }
}

/* --- 碰撞与结算 ------------------------------------------------------------- */

/* 第 6 步前半：自机弹对敌机与 Boss。目标按槽位顺序，Boss 排在最后；
   Boss 只在符卡进行中且不在压力形态时受伤。 */
static void shots_collide(stg_game *g)
{
    stg_target *tb = &g->tg[STG_ENEMY_SLOTS];

    stg_enemies_targets(&g->en, g->tg);
    tb->x     = g->boss.x;
    tb->y     = g->boss.y;
    tb->r     = STG_BOSS_R_HIT;
    tb->alive = (g->boss.on == STG_BOSS_CARD && !g->press) ? 1 : 0;
    (void)stg_pool_hit_targets(&g->f.pools.pbullet, g->f.r_kind, g->tg, STG_TARGETS, g->hits);
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

/* 炸弹：消弹、无敌延长到至少 180 帧。 */
static void bomb(stg_game *g)
{
    g->bombs--;
    clear_bullets(g);
    if (g->inv < STG_INV_BOMB)
        g->inv = STG_INV_BOMB;
    g->ev |= STG_EV_BOMB;
}

/* 第 7 步前半：敌机扣血与死亡，Boss 扣血、符卡在血量耗尽时结束。 */
static void settle_targets(stg_game *g)
{
    stg_boss *b = &g->boss;

    (void)stg_enemies_settle(&g->en, g->hits, &g->f.pools.particle, &g->score, g->kills);
    if (b->on == STG_BOSS_CARD && g->hits[STG_ENEMY_SLOTS] != 0u) {
        b->hp -= (int32_t)g->hits[STG_ENEMY_SLOTS];
        if (b->hp <= 0)
            card_end(g, 1);
    }
}

void stg_game_step(stg_game *g, uint32_t keys)
{
    uint32_t pressed, i;
    int      hit = 0;

    /* 1. 输入与阶段切换（压力形态不认按键） */
    if (g->press)
        keys = 0u;
    pressed      = keys & ~g->keys_prev;
    g->ev        = 0u;
    g->flash     = 0u;
    g->cancelled = 0u;
    g->keys_prev = keys;
    if (g->phase == STG_PH_TITLE && g->phase_t >= STG_TITLE_FRAMES) {
        /* EX 路线的入口：过渡帧上四个方向键同按。判据按键脚本与通关录像搜索器
           永远不产生这种输入（四向同按 = 原地不动），经典路线不受影响。
           GFX2D_START_EX 是演示/截帧钩子：编译期直接走 EX，不需要按键。 */
#ifdef GFX2D_START_EX
        if (1) {
#else
        if ((keys & (STG_KEY_UP | STG_KEY_DOWN | STG_KEY_LEFT | STG_KEY_RIGHT)) ==
            (STG_KEY_UP | STG_KEY_DOWN | STG_KEY_LEFT | STG_KEY_RIGHT)) {
#endif
            g->ss = &stg_stage_ex;
            g->ex = 1u;
        }
        g->phase   = STG_PH_PLAY;
        g->phase_t = 0u;
        g->ev     |= STG_EV_START;
        set_seg(g, STG_GSEG_STAGE);
    } else if (g->phase == STG_PH_CLEAR && g->phase_t >= STG_CLEAR_FRAMES) {
        round_reset(g);
    }
    g->slow     = (g->phase == STG_PH_PLAY && (keys & STG_KEY_SLOW) != 0u) ? 1u : 0u;
    g->bomb_req = (g->phase == STG_PH_PLAY && (pressed & STG_KEY_BOMB) != 0u && g->bombs != 0u) ? 1u : 0u;

    /* 2. 四个池各一遍，粒子池再走一遍寿命；背景 */
    stg_pool_step(&g->f.pools.ebullet, &g->f.bounds, &g->f.last);
    stg_pool_step(&g->f.pools.pbullet, &g->pb_bounds, 0);
    stg_pool_step(&g->f.pools.enemy, &g->f.bounds, 0);
    stg_pool_step(&g->f.pools.particle, &g->f.bounds, 0);
    stg_pool_age(&g->f.pools.particle);
    stg_bg_step(&g->bg);

    for (i = 0u; i < STG_TARGETS; i++)
        g->hits[i] = 0u;
    if (g->phase == STG_PH_PLAY) {
        stg_emit_ctx ctx;

        /* 3. 自机 */
        if (!g->press)
            player_move(g, keys);
        if (g->shoot_on)
            player_shoot(g);

        /* 4. 事件表与符卡表的按时间切换 */
        stage_events(g);
        boss_timing(g);

        /* 5. 敌机按槽位顺序，Boss 在最后 */
        ctx.px  = g->f.px;
        ctx.py  = g->f.py;
        ctx.rng = &g->f.rng;
        (void)stg_enemies_step(&g->en, &g->f.pools.ebullet, &ctx);
        boss_step(g, &ctx);

        /* 6. 碰撞 */
        shots_collide(g);
        hit = player_collide(g);
    }

    /* 7. 结算：敌机与 Boss；无敌倒计时先减一，再处理本帧的受击与炸弹 */
    settle_targets(g);
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

    /* 背景震动的幅度：受击/炸弹/破符给拍，逐帧衰减。只由 ev 位派生、不消耗随机数，
       不影响任何结算；绘制层读 shake 与 frame 算偏移（画法在 stg_draw.c）。 */
    if ((g->ev & STG_EV_LOST) != 0u)
        g->shake = 12u;
    else if ((g->ev & (STG_EV_HIT | STG_EV_BOMB | STG_EV_CARD_KILL)) != 0u)
        g->shake = (g->shake < 8u) ? 8u : g->shake;
    else if (g->shake != 0u)
        g->shake--;

    /* HI-SCORE 在局的结束之前取 max：结束那一帧的得分也算 */
    if (g->score > g->hiscore)
        g->hiscore = g->score;
    if (g->phase == STG_PH_PLAY && g->lives == 0u) {
        round_reset(g);
        g->ev |= STG_EV_OVER;
    }
    if (g->boss.on != STG_BOSS_OFF)
        g->boss.t++;
    g->phase_t++;
    g->frame++;
}

void stg_game_press_begin(stg_game *g, uint32_t kind)
{
    stg_boss *b = &g->boss;

    stg_game_init(g, STG_GAME_SEED);
    g->press      = 1u;
    g->press_kind = kind & STG_F_KIND_MASK;
    g->knob       = 0u;
    g->shoot_on   = 0u;
    g->hits_on    = 0u;
    g->phase      = STG_PH_PLAY;
    g->phase_t    = 0u;
    g->ev_i       = g->ss->n_events;
    b->card = STG_CARD_N - 1u;
    b->x    = STG_BOSS_X0;
    b->y    = STG_BOSS_HOME_Y;
    b->a    = 0u;
    card_begin(g);
    set_seg(g, STG_GSEG_SPELL1 + b->card);
    g->ev = 0u;
}
