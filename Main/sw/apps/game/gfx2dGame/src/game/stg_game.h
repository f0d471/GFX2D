/******************************************************************************
* @file stg_game.h: 游戏状态与一帧的顺序
*
* @brief  SW7 2.4 节 C 批的定法。游戏的全部状态在 stg_game 一个结构体里（四个池、随机数、
*         自机、残机与炸弹、无敌、得分），没有模块级的可变全局；进游戏模式时整体初始化（2.8 节）。
*
*         一帧的顺序（stg_game_step，是契约，录像回放的 CRC 与自动驾驶的确定性都靠它）：
*           1. 输入：阶段到点就切换（标题满 120 帧开局、CLEAR 满 180 帧回标题）；
*              对立方向当作该轴不动，炸弹键取按下沿（与上一帧的位图比）；
*           2. 四个池各一遍（敌弹、粒子用 10 像素外扩的边界，自机弹用 12 像素的），粒子池再走一遍寿命；
*           3. 自机：移动，然后自动射击（新弹追加在池尾，出生那一帧不移动）；
*           4. 事件表与符卡表（C 批 C2）；
*           5. 敌机与 Boss 的发射器与移动（C 批 C2）；
*           6. 碰撞：先自机弹对敌机与 Boss（C2），再自机判定点对敌弹；受击记下来，第 7 步处理；
*           7. 结算：敌机死亡与符卡切换（C2）；无敌倒计时先减一，再处理本帧的受击与炸弹；局的结束；
*              HI-SCORE 取 max；阶段内帧号加一。
*         第 k 帧的状态指第 k 帧第 7 步之后的状态。标题与 CLEAR 期间第 2 步照走，第 3~6 步不走。
*
*         受击、炸弹与无敌（2.4 节）：
*           受击且还有炸弹    改成放炸弹、不扣残机（自动炸弹，所有平台都生效）；
*           受击且没有炸弹    残机减一、闪白一帧、无敌 120 帧、炸弹补回 3；残机到 0 回标题；
*           手动炸弹          按下沿且还有炸弹；与受击同一帧只放一次；
*           炸弹              可消除的敌弹打移除标记、原位生成火花，无敌延长到至少 180 帧。
*         无敌倒计时在受击与炸弹之前减一，所以设下之后恰好 120（或 180）帧不受击。
*         无敌就是把受击开关关掉：受击开关 = 无敌为 0 且 SW8 的受击钩子打开，擦弹照算。
*
*         按键位与 board.h 的 BOARD_KEY_* 同值；src/game/ 不含 board.h，入口里用 _Static_assert 卡住。
*
*         注错开关（主机测试用，见 check_sw7_game.sh）：
*           INJECT_GAME_NO_AUTOBOMB    受击时不看炸弹，直接扣残机
*           INJECT_GAME_NO_INVINCIBLE  受击开关不看无敌倒计时
******************************************************************************/
#ifndef STG_GAME_H
#define STG_GAME_H

#include <stdint.h>

#include "core/fixmath.h"

#include "stg_field.h"
#include "stg_pool.h"

/* 按键位（与 board.h 同值）。 */
#define STG_KEY_UP          (1u << 0)
#define STG_KEY_DOWN        (1u << 1)
#define STG_KEY_LEFT        (1u << 2)
#define STG_KEY_RIGHT       (1u << 3)
#define STG_KEY_BOMB        (1u << 4)
#define STG_KEY_SLOW        (1u << 5)

/* 阶段。 */
#define STG_PH_TITLE        0u
#define STG_PH_PLAY         1u
#define STG_PH_CLEAR        2u

/* 自机的移动（2.4 节）：Q.8 每帧；斜向每轴乘 181/256。 */
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

/* 一帧里发生了什么（stg_game.ev 的位），给测试、判据日志与 HUD。 */
#define STG_EV_HIT          (1u << 0)   /* 被敌弹击中（无论是否由炸弹抵掉） */
#define STG_EV_LOST         (1u << 1)   /* 扣了一个残机 */
#define STG_EV_BOMB         (1u << 2)   /* 放了一次炸弹 */
#define STG_EV_AUTOBOMB     (1u << 3)   /* 这次炸弹是受击触发的 */
#define STG_EV_OVER         (1u << 4)   /* 残机用完，回到标题 */
#define STG_EV_START        (1u << 5)   /* 标题结束，开局 */

typedef struct {
    stg_field  f;               /* 四个池、随机数、自机中心与探针、按弹种的判定半径 */
    stg_bounds pb_bounds;       /* 自机弹池的出界边界（外扩 12 像素） */
    uint32_t   seed;            /* 每局开头用它重新播种 */
    uint32_t   frame;           /* 进入游戏模式以来走过的帧数 */
    uint32_t   phase;           /* STG_PH_* */
    uint32_t   phase_t;         /* 本阶段已经走过的帧数（第 7 步末尾加一） */
    uint32_t   keys_prev;       /* 上一帧的按键位图（取炸弹键的按下沿） */
    uint32_t   slow;            /* 本帧低速（显示判定点） */
    uint32_t   shot_wait;       /* 自动射击的倒数计数 */
    uint32_t   lives, bombs;
    uint32_t   inv;             /* 无敌剩余帧数 */
    uint32_t   bomb_req;        /* 本帧要放炸弹（手动按下沿或受击） */
    uint32_t   flash;           /* 本帧组帧时叠一条闪白 */
    uint32_t   hits_on;         /* SW8 的受击钩子 */
    uint32_t   score, hiscore;
    uint32_t   graze;           /* 本局累计擦弹 */
    uint32_t   cancelled;       /* 本帧炸弹消掉的敌弹数 */
    uint32_t   ev;              /* 本帧的 STG_EV_* */
} stg_game;

/* 进游戏模式：HI-SCORE 清零、受击钩子打开、第一局从标题开始。 */
void stg_game_init(stg_game *g, uint32_t seed);

/* 一帧的第 1~7 步。keys 是经过 SW8 框架之后的按键位图（STG_KEY_*）。 */
void stg_game_step(stg_game *g, uint32_t keys);

/* SW8 的受击钩子：0 关掉受击，非 0 打开。与无敌是「与」的关系。 */
void stg_game_set_hits(stg_game *g, int on);

/* 自机这一帧画不画：无敌期间每 4 帧隐去 4 帧（inv 的第 2 位为 1 时隐去）。 */
static inline int stg_game_player_visible(const stg_game *g)
{
    return g->phase == STG_PH_PLAY && (g->inv & 4u) == 0u;
}

#endif /* STG_GAME_H */
