/******************************************************************************
* @file stg_game.h: 游戏状态与一帧的顺序
*
* @brief  游戏的全部状态在 stg_game 一个结构体里（四个池、随机数、
*         自机、残机与炸弹、无敌、得分、敌机槽位表、事件表与符卡表的游标、Boss、门控、背景），
*         没有模块级的可变全局；进游戏模式时整体初始化。
*
*         一帧的顺序（stg_game_step，是契约，录像回放的 CRC 与自动驾驶的确定性都靠它）：
*           1. 输入：阶段到点就切换（标题满 120 帧开局、CLEAR 满 180 帧回标题）；
*              对立方向当作该轴不动，炸弹键取按下沿（与上一帧的位图比）；
*           2. 四个池各一遍（敌弹、粒子用 10 像素外扩的边界，自机弹用 12 像素的），粒子池再走一遍寿命；
*              背景推进一帧；
*           3. 自机：移动，然后自动射击（新弹追加在池尾，出生那一帧不移动）；
*           4. 事件表：取出本帧到期的事件执行；Boss 在场时推进符卡表里按时间的切换
*              （进场满 74 帧、间隙满、超时）；
*           5. 敌机按槽位顺序：发射器到点齐射（在它当前的位置），再走一步，再判出界；
*              Boss 排在所有槽位之后：符卡进行中时发射器齐射（终符由门控决定），再移动；
*           6. 碰撞：先自机弹对敌机与 Boss，再自机判定点对敌弹；
*           7. 结算：敌机扣血与死亡（得分、火花），Boss 扣血、符卡在血量耗尽时结束；
*              无敌倒计时先减一，再处理本帧的受击与炸弹；HI-SCORE 取 max；局的结束；
*              Boss 与阶段内的帧数加一。
*         第 k 帧的状态指第 k 帧第 7 步之后的状态。标题与 CLEAR 期间第 2 步照走，第 3~6 步不走。
*
*         受击、炸弹与无敌：
*           受击且还有炸弹    改成放炸弹、不扣残机（自动炸弹，所有平台都生效）；
*           受击且没有炸弹    残机减一、闪白一帧、无敌 120 帧、炸弹补回 3；残机到 0 回标题；
*           手动炸弹          按下沿且还有炸弹；与受击同一帧只放一次；
*           炸弹              可消除的敌弹打移除标记、原位生成火花，无敌延长到至少 180 帧，不伤 Boss。
*         无敌倒计时在受击与炸弹之前减一，所以设下之后恰好 120（或 180）帧不受击。
*         无敌就是把受击开关关掉：受击开关 = 无敌为 0 且统计埋点的受击钩子打开，擦弹照算。
*
*         统计埋点与演示模式的钩子：stg_game_press_begin 把游戏放进终符的压力形态，
*         stg_game_set_knob 设门控的旋钮，stg_game_set_hits 是受击钩子。
*
*         按键位与 board.h 的 BOARD_KEY_* 同值；src/game/ 不含 board.h，入口里用 _Static_assert 卡住。
*
*         注错开关（主机测试用，见 check_sw7_game.sh）：
*           INJECT_GAME_NO_AUTOBOMB    受击时不看炸弹，直接扣残机
*           INJECT_GAME_NO_INVINCIBLE  受击开关不看无敌倒计时
*           INJECT_CARD_TIMEOUT_GT     符卡超时判成 t > timeout（晚一帧）
*           INJECT_GATE_NO_ACC         门控的累加器不跨帧，每帧的余数丢掉
******************************************************************************/
#ifndef STG_GAME_H
#define STG_GAME_H

#include <stdint.h>

#include "core/fixmath.h"

#include "stg_enemy.h"
#include "stg_field.h"
#include "stg_pool.h"
#include "stg_stage.h"

/* 按键位（与 board.h 同值）。 */
#define STG_KEY_UP          (1u << 0)
#define STG_KEY_DOWN        (1u << 1)
#define STG_KEY_LEFT        (1u << 2)
#define STG_KEY_RIGHT       (1u << 3)
#define STG_KEY_BOMB        (1u << 4)
#define STG_KEY_SLOW        (1u << 5)

/* 进游戏模式时的种子。 */
#define STG_GAME_SEED       0x5E7B0C01u

/* 阶段。 */
#define STG_PH_TITLE        0u
#define STG_PH_PLAY         1u
#define STG_PH_CLEAR        2u

/* 段号（HUD 的段名、判据日志的 seg 行）。 */
#define STG_GSEG_TITLE      0u
#define STG_GSEG_STAGE      1u
#define STG_GSEG_BOSS       2u          /* Boss 进场 */
#define STG_GSEG_SPELL1     3u          /* 3~6：四张符卡 */
#define STG_GSEG_CLEAR      7u
#define STG_GSEG_N          8u

/* Boss 的状态。 */
#define STG_BOSS_OFF        0u
#define STG_BOSS_ENTER      1u
#define STG_BOSS_GAP        2u
#define STG_BOSS_CARD       3u

/* 自机的移动：Q.8 每帧；斜向每轴乘 181/256。 */
#define STG_SPEED_FAST      1024
#define STG_SPEED_SLOW      512
#define STG_DIAG_MUL        181
#define STG_PLAYER_X_MIN    (16 * FX_ONE)
#define STG_PLAYER_X_MAX    (384 * FX_ONE)
#define STG_PLAYER_Y_MIN    (16 * FX_ONE)
#define STG_PLAYER_Y_MAX    (464 * FX_ONE)

/* 自动射击：每 4 帧两束，出生在 (±7, -16) 像素，竖直向上 11 像素/帧。 */
#define STG_SHOT_INTERVAL   4u
#define STG_SHOT_DX         (7 * FX_ONE)
#define STG_SHOT_DY         (-16 * FX_ONE)
#define STG_SHOT_VY         (-11 * FX_ONE)
#define STG_PB_MARGIN_PX    12

/* 残机、炸弹与无敌。 */
#define STG_LIVES_INIT      3u
#define STG_BOMBS_INIT      3u
#define STG_INV_HIT         120u
#define STG_INV_BOMB        180u

/* 火花：原位、向上 1 像素/帧、寿命 16 帧。 */
#define STG_SPARK_VY        (-FX_ONE)
#define STG_SPARK_LIFE      16u

/* 展示段与得分。 */
#define STG_TITLE_FRAMES    120u
#define STG_CLEAR_FRAMES    180u
#define STG_SCORE_GRAZE     10u

/* 目标：64 个槽位加 Boss。 */
#define STG_TARGETS         (STG_ENEMY_SLOTS + 1u)

/* 一帧里发生了什么（stg_game.ev 的位），给测试、判据日志与 HUD。 */
#define STG_EV_HIT          (1u << 0)   /* 被敌弹击中（无论是否由炸弹抵掉） */
#define STG_EV_LOST         (1u << 1)   /* 扣了一个残机 */
#define STG_EV_BOMB         (1u << 2)   /* 放了一次炸弹 */
#define STG_EV_AUTOBOMB     (1u << 3)   /* 这次炸弹是受击触发的 */
#define STG_EV_OVER         (1u << 4)   /* 残机用完，回到标题 */
#define STG_EV_START        (1u << 5)   /* 标题结束，开局 */
#define STG_EV_SEG          (1u << 6)   /* 进入了新的一段（seg 变了） */
#define STG_EV_SPAWN        (1u << 7)   /* 执行了出敌事件 */
#define STG_EV_CARD_KILL    (1u << 8)   /* 符卡在血量耗尽时结束 */
#define STG_EV_CARD_TIME    (1u << 9)   /* 符卡超时结束 */

typedef struct {
    uint32_t    on;             /* STG_BOSS_* */
    uint32_t    t;              /* 本状态已经走过的帧数（第 7 步末尾加一） */
    uint32_t    card;           /* 当前（或间隙之后的）符卡号，0~3 */
    int32_t     x, y;           /* 中心，Q.8 */
    uint16_t    a;              /* 游动的角度，BAM */
    uint16_t    rsvd;
    int32_t     hp;
    uint32_t    n_em;
    stg_emitter em[STG_CARD_EM_MAX];
    uint32_t    gate_acc;       /* 门控：累加器 */
    uint32_t    gate_tok;       /* 门控：剩余令牌 */
    uint32_t    gate_rr;        /* 门控：轮到第几个发射器 */
    uint32_t    gate_fired;     /* 门控：本张符卡累计发出的弹数（含被池拒绝的） */
    uint32_t    gate_k;         /* 门控：本帧的旋钮 */
} stg_boss;

typedef struct {
    stg_field   f;              /* 四个池、随机数、自机中心与探针、按弹种的判定半径 */
    stg_bounds  pb_bounds;      /* 自机弹池的出界边界（外扩 12 像素） */
    stg_enemies en;             /* 敌机槽位表 */
    stg_boss    boss;
    stg_bg      bg;
    const stg_stage_set *ss;    /* 本局用的关卡集（经典或 EX），标题→开局的过渡帧上定 */
    uint32_t    ex;             /* 1 = EX 路线（四方向同按进入；只换表，规则不变） */
    uint32_t    seed;           /* 每局开头用它重新播种 */
    uint32_t    frame;          /* 进入游戏模式以来走过的帧数 */
    uint32_t    phase;          /* STG_PH_* */
    uint32_t    phase_t;        /* 本阶段已经走过的帧数（第 7 步末尾加一）；开局之后就是事件表的帧号 */
    uint32_t    seg;            /* STG_GSEG_* */
    uint32_t    ev_i;           /* 事件表的下一条 */
    uint32_t    keys_prev;      /* 上一帧的按键位图（取炸弹键的按下沿） */
    uint32_t    slow;           /* 本帧低速（显示判定点） */
    uint32_t    shot_wait;      /* 自动射击的倒数计数 */
    uint32_t    shoot_on;       /* 自动射击开关（压力形态关掉；测试也用） */
    uint32_t    lives, bombs;
    uint32_t    inv;            /* 无敌剩余帧数 */
    uint32_t    bomb_req;       /* 本帧要放炸弹（手动按下沿或受击） */
    uint32_t    flash;          /* 本帧组帧时叠一条闪白 */
    uint32_t    hits_on;        /* SW8 的受击钩子 */
    uint32_t    press;          /* 终符的压力形态（SW8 模式 3） */
    uint32_t    press_kind;     /* 压力形态下终符敌弹换成的弹种 */
    uint32_t    knob;           /* 压力形态下门控的旋钮 K */
    uint32_t    shake;          /* 背景震动剩余帧数（第 7 步由 ev 派生，绘制层读） */
    uint32_t    score, hiscore;
    uint32_t    graze;          /* 本局累计擦弹 */
    uint32_t    kills[STG_ETYPE_N];     /* 本局按机种累计的击毁数 */
    uint32_t    cancelled;      /* 本帧消掉的敌弹数（炸弹、清弹、符卡结束） */
    uint32_t    ev;             /* 本帧的 STG_EV_* */
    stg_target  tg[STG_TARGETS];    /* 本帧的目标圆（第 6 步写）：槽位 0~63 与 Boss */
    uint32_t    hits[STG_TARGETS];  /* 本帧每个目标的命中数（第 6 步写、第 7 步读） */
} stg_game;

/* 进游戏模式：HI-SCORE 清零、受击钩子打开、第一局从标题开始。 */
void stg_game_init(stg_game *g, uint32_t seed);

/* 一帧的第 1~7 步。keys 是经过模式框架处理之后的按键位图（STG_KEY_*）。 */
void stg_game_step(stg_game *g, uint32_t keys);

/* 统计埋点的受击钩子：0 关掉受击，非 0 打开。与无敌是「与」的关系。 */
void stg_game_set_hits(stg_game *g, int on);

/* 压力形态（演示模式的 N_max 搜索用）：以 STG_GAME_SEED 重新进游戏，直接放进终符：Boss 停在 (200, 100)、不受伤、
   符卡不结束；自机停在出生点、不射击、不认按键；受击关掉；终符的敌弹全部换成 kind（只取低 4 位）；
   旋钮为 0（不发弹），由 stg_game_set_knob 设。 */
void stg_game_press_begin(stg_game *g, uint32_t kind);

/* 门控的旋钮 K（每 125 帧允许发出的弹数），只在压力形态下起作用。 */
void stg_game_set_knob(stg_game *g, uint32_t knob);

/* 段名：READY、STAGE、BOSS、四张符卡名、CLEAR；越界返回 "?"。
   判据日志与旧调用用这一个（经典口径）；HUD 用下面带状态的（EX 路线显示 EX 名）。 */
const char *stg_game_seg_name(uint32_t seg);
const char *stg_game_seg_name_g(const stg_game *g, uint32_t seg);

/* 自机这一帧画不画：开局之后，无敌期间每 4 帧隐去 4 帧（inv 的第 2 位为 1 时隐去）。 */
static inline int stg_game_player_visible(const stg_game *g)
{
    return g->phase == STG_PH_PLAY && (g->inv & 4u) == 0u;
}

#endif /* STG_GAME_H */
