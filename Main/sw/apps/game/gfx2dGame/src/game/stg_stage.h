/******************************************************************************
* @file stg_stage.h: 关卡 —— 杂兵段的事件表、Boss 的符卡表与背景
*
* @brief  sw 第 06 章 3.11 节（C 批）的定法。关卡是两张常量表，游戏逻辑按表执行，没有按关卡写死的分支：
*           杂兵段  按帧号排序的事件表 stg_stage_events：{帧号, 类型, 参数}，类型有出敌、清弹、进 Boss；
*                   帧号从标题结束那一帧（开局第 0 帧）算起；最后一条是「进 Boss」，之后事件表不再推进。
*           Boss 段 符卡表 stg_cards：每张给名字、血量、超时帧数、开场间隙帧数、是否受门控、1~6 个发射器
*                   （发射记录、锚点、偏移）。锚点是 Boss（位置随 Boss 走）或绝对位置（随机雨从上沿落下）。
*         事件与符卡怎么执行在 stg_game.c（一帧的第 4、5、7 步）。这里只有常量表与背景。
*
*         背景（2.5 节，参数照决策页效果图）：整区 FILL 铺底色；横线 15 条、每 32 像素一条，
*         每帧下移 230/256 像素；竖线 13 条在 x = 8 + 32k；每第 4 条亮一些；星点 60 个，
*         开局时由游戏的 xorshift32 生成，出了下沿回到上沿外。这里只推进状态，画法在组帧（2.6 节）。
******************************************************************************/
#ifndef STG_STAGE_H
#define STG_STAGE_H

#include <stdint.h>

#include "core/xorshift32.h"

#include "stg_emit.h"
#include "stg_enemy.h"

/* 事件类型 */
#define STG_EVT_SPAWN       0u
#define STG_EVT_CLEAR       1u          /* 清弹：可消除的敌弹照炸弹的做法变成火花 */
#define STG_EVT_BOSS        2u

typedef struct {
    uint16_t t;             /* 帧号（开局第 0 帧起） */
    uint8_t  type;          /* STG_EVT_* */
    uint8_t  etype;         /* 出敌：机种 */
    int16_t  x, y;          /* 出敌：出生点（像素） */
    uint8_t  move;          /* 出敌：移动事件表号 */
    uint8_t  group;         /* 出敌：发射器组号 */
    uint16_t delay;         /* 出敌：第一次齐射推迟几帧 */
} stg_event;

_Static_assert(sizeof(stg_event) == 12u, "stg_event 必须是 12 字节");

/* 发射器组：一架敌机挂的 1~4 条发射记录。 */
typedef struct {
    const stg_emit_rec *const *recs;
    uint32_t                   n;
} stg_em_group;

extern const stg_event    stg_stage_events[];
extern const uint32_t     stg_stage_n_events;
extern const stg_move_tab stg_moves[];
extern const uint32_t     stg_n_moves;
extern const stg_em_group stg_em_groups[];
extern const uint32_t     stg_n_em_groups;

/* 符卡 */
#define STG_CARD_N          4u
#define STG_CARD_EM_MAX     6u
#define STG_ANCHOR_BOSS     0u
#define STG_ANCHOR_ABS      1u

typedef struct {
    const stg_emit_rec *rec;
    uint32_t            anchor;     /* STG_ANCHOR_* */
    int32_t             x, y;       /* 锚点是 Boss 时为偏移，绝对时为位置；Q.8 */
} stg_card_em;

typedef struct {
    const char  *name;              /* HUD 显示的符卡名 */
    int32_t      hp;
    uint32_t     timeout;           /* 开始之后几帧超时 */
    uint32_t     gap;               /* 开场间隙（第一张为 0：Boss 进场就是它的间隙） */
    uint32_t     gated;             /* 1：发射器不按自己的计时，由门控决定何时齐射 */
    uint32_t     n_em;
    stg_card_em  em[STG_CARD_EM_MAX];
} stg_card;

extern const stg_card stg_cards[STG_CARD_N];

/* Boss（2.5 节）：从 (200, -48) 以 2 像素/帧下移 74 帧到 (200, 100)，之后在
   (200 + 70 sin a, 100 + 12 sin 2a) 附近游动，a 每帧加 91 BAM。判定半径 32 像素。 */
#define STG_BOSS_X0         (200 * 256)
#define STG_BOSS_Y0         (-48 * 256)
#define STG_BOSS_VY_ENTER   (2 * 256)
#define STG_BOSS_ENTER_FRAMES 74u
#define STG_BOSS_HOME_Y     (100 * 256)
#define STG_BOSS_AX_PX      70
#define STG_BOSS_AY_PX      12
#define STG_BOSS_DA         91u
#define STG_BOSS_R_HIT      (32 * 256)

/* 门控（2.5 节）：K 是每 125 帧允许发出的弹数；令牌上限是这一帧新增的加 48。
   游戏里 K = 600 + (t * 3) >> 2，t 是符卡开始后的帧数。 */
#define STG_GATE_L          125u
#define STG_GATE_CARRY      48u
#define STG_GATE_K0         600u

/* 得分（2.4 节） */
#define STG_CARD_BONUS      10000u      /* 乘符卡序号 1~4 */
#define STG_FINAL_BONUS     50000u

/* 背景 */
#define STG_BG_HLINES       15u
#define STG_BG_VLINES       13u
#define STG_BG_STARS        60u
#define STG_BG_SPACING_PX   32
#define STG_BG_SCROLL       230u        /* Q.8 每帧 */
#define STG_BG_VX0_PX       8
#define STG_BG_BRIGHT       0x0013394Eu
#define STG_BG_DIM          0x00091723u

typedef struct {
    int32_t  x, y;          /* 左上角，Q.8 */
    int32_t  vy;            /* Q.8/帧 */
    uint32_t kind;          /* 0 2x2 亮、1 2x2 暗、2 1x1 亮、3 1x1 暗（stars.png 的格号） */
} stg_star;

typedef struct {
    uint32_t scroll;        /* 横线下移了多少，Q.8，[0, 32 像素) */
    uint32_t phase;         /* 亮线的相位：横线每下移满 32 像素加一，取低 2 位 */
    stg_star star[STG_BG_STARS];
} stg_bg;

/* 开局：横线回到原位，星点从 rng 取数（每个依次取 x、y、速度、种类）。 */
void stg_bg_init(stg_bg *b, xs32 *rng);

/* 一帧：横线下移，星点下移，出了下沿回到上沿外。 */
void stg_bg_step(stg_bg *b);

/* 第 k 条横线的 y（像素）与是否亮；第 k 条竖线的 x 与是否亮。 */
int32_t stg_bg_hline_y(const stg_bg *b, uint32_t k);
int     stg_bg_hline_bright(const stg_bg *b, uint32_t k);
int32_t stg_bg_vline_x(uint32_t k);
int     stg_bg_vline_bright(uint32_t k);

#endif /* STG_STAGE_H */
