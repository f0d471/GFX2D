/******************************************************************************
* @file stg_stage.c: 关卡的常量表与背景
*
* @brief  口径见 stg_stage.h。表里的数就是关卡的定义；
*         测试按 stg_stage.h 的常量另算各段的开始帧，不读这几张表。
*         弹种号照 stg_kind 的弹种表：0~3 圆弹、4~7 小圆弹、8~11 米弹，颜色号 0~3 依次是青、品红、琥珀、绿。
******************************************************************************/

#include "stg_stage.h"

#include "core/fixmath.h"

#define PX(v)   ((v) * FX_ONE)

/* --- 敌机的发射记录与发射器组 ---------------------------------------------- */
/*                                  count dir           kind angle0 step  spin   intv rep rsvd speed sstep rx  rs */
static const stg_emit_rec em_dart_aim   = { 1u, STG_DIR_AIM,  10u, 0u,     0u,    0u, 50u, 0u, 0u,  640,  0,  0,  0 };
static const stg_emit_rec em_dart_pair  = { 2u, STG_DIR_ABS,   5u, 15384u, 2000u, 0u, 40u, 0u, 0u,  512,  0,  0,  0 };
static const stg_emit_rec em_cross_ring = {16u, STG_DIR_ABS,   1u, 0u,     4096u, 0u, 45u, 0u, 0u,  384,  0,  0,  0 };
static const stg_emit_rec em_cross_fan  = { 5u, STG_DIR_AIM,  11u, 61440u, 2048u, 0u, 30u, 0u, 0u,  576,  0,  0,  0 };

static const stg_emit_rec *const grp_dart_aim[]   = { &em_dart_aim };
static const stg_emit_rec *const grp_dart_pair[]  = { &em_dart_pair };
static const stg_emit_rec *const grp_cross_ring[] = { &em_cross_ring };
static const stg_emit_rec *const grp_cross_fan[]  = { &em_cross_fan };

const stg_em_group stg_em_groups[] = {
    { grp_dart_aim,   1u },     /* 0 */
    { grp_dart_pair,  1u },     /* 1 */
    { grp_cross_ring, 1u },     /* 2 */
    { grp_cross_fan,  1u },     /* 3 */
};
const uint32_t stg_n_em_groups = (uint32_t)(sizeof stg_em_groups / sizeof stg_em_groups[0]);

/* --- 移动事件表 ------------------------------------------------------------ */

static const stg_move mv_turn_r[]  = { { 0, 512, 80u }, {  384, -384, 1u } };
static const stg_move mv_turn_l[]  = { { 0, 512, 80u }, { -384, -384, 1u } };
static const stg_move mv_stop[]    = { { 0, 384, 100u }, { 0, -384, 240u }, { 0, -256, 1u } };
static const stg_move mv_down[]    = { { 0, 640, 1u } };
static const stg_move mv_diag_r[]  = { {  384, 256, 1u } };
static const stg_move mv_diag_l[]  = { { -384, 256, 1u } };

const stg_move_tab stg_moves[] = {
    { mv_turn_r, 2u },          /* 0 下行后折向右下 */
    { mv_turn_l, 2u },          /* 1 下行后折向左下 */
    { mv_stop,   3u },          /* 2 下行、停 240 帧、上行离场 */
    { mv_down,   1u },          /* 3 直下 */
    { mv_diag_r, 1u },          /* 4 从左侧斜向右下 */
    { mv_diag_l, 1u },          /* 5 从右侧斜向左下 */
};
const uint32_t stg_n_moves = (uint32_t)(sizeof stg_moves / sizeof stg_moves[0]);

/* --- 杂兵段的事件表（2.5 节「关卡的表」） -------------------------------------- */

#define SPAWN(t, et, x, y, mv, grp, d)  { (t), STG_EVT_SPAWN, (et), (x), (y), (mv), (grp), (d) }
#define DART   STG_ETYPE_DART
#define CROSS  STG_ETYPE_CROSS

const stg_event stg_stage_events[] = {
    /* 第 1 波：左上五架 DART，下行后折向右下，单发狙 */
    SPAWN(  60, DART,  100, -24, 0, 0, 30), SPAWN(  84, DART,  100, -24, 0, 0, 30),
    SPAWN( 108, DART,  100, -24, 0, 0, 30), SPAWN( 132, DART,  100, -24, 0, 0, 30),
    SPAWN( 156, DART,  100, -24, 0, 0, 30),
    /* 第 2 波：右上五架，镜像 */
    SPAWN( 240, DART,  300, -24, 1, 0, 30), SPAWN( 264, DART,  300, -24, 1, 0, 30),
    SPAWN( 288, DART,  300, -24, 1, 0, 30), SPAWN( 312, DART,  300, -24, 1, 0, 30),
    SPAWN( 336, DART,  300, -24, 1, 0, 30),
    /* 第 3 波：中央一架 CROSS，停下来放圆环 */
    SPAWN( 480, CROSS, 200, -24, 2, 2, 90),
    /* 第 4 波：两侧成对直下的 DART，两发小弹 */
    SPAWN( 780, DART,   60, -24, 3, 1, 20), SPAWN( 780, DART,  340, -24, 3, 1, 20),
    SPAWN( 840, DART,   60, -24, 3, 1, 20), SPAWN( 840, DART,  340, -24, 3, 1, 20),
    SPAWN( 900, DART,   60, -24, 3, 1, 20), SPAWN( 900, DART,  340, -24, 3, 1, 20),
    /* 第 5 波：两架 CROSS，停下来放自机狙扇 */
    SPAWN(1020, CROSS, 120, -24, 2, 3, 90), SPAWN(1020, CROSS, 280, -24, 2, 3, 90),
    /* 第 6 波：左右两侧交替斜飞的 DART */
    SPAWN(1320, DART,  -20,  60, 4, 0, 20), SPAWN(1350, DART,  420,  60, 5, 0, 20),
    SPAWN(1380, DART,  -20,  60, 4, 0, 20), SPAWN(1410, DART,  420,  60, 5, 0, 20),
    SPAWN(1440, DART,  -20,  60, 4, 0, 20), SPAWN(1470, DART,  420,  60, 5, 0, 20),
    SPAWN(1500, DART,  -20,  60, 4, 0, 20),
    { 1680u, STG_EVT_CLEAR, 0u, 0, 0, 0u, 0u, 0u },
    { 1740u, STG_EVT_BOSS,  0u, 0, 0, 0u, 0u, 0u },
};
const uint32_t stg_stage_n_events = (uint32_t)(sizeof stg_stage_events / sizeof stg_stage_events[0]);

/* --- 符卡的发射记录 -------------------------------------------------------- */
/*                                   count dir           kind angle0 step   spin    intv rep rsvd speed sstep rx     rs */
static const stg_emit_rec c1_ring   = {20u, STG_DIR_SEQ,   0u, 0u,     3277u,  1638u,  36u, 0u, 0u,  448,  0,  0,     0 };
static const stg_emit_rec c1_aim    = { 3u, STG_DIR_AIM,  10u, 63488u, 2048u,  0u,    40u, 0u, 0u,  896,  0,  0,     0 };
static const stg_emit_rec c2_spir_a = { 4u, STG_DIR_SEQ,   5u, 0u,     16384u, 1300u,   3u, 0u, 0u,  512,  0,  0,     0 };
static const stg_emit_rec c2_spir_b = { 4u, STG_DIR_SEQ,   4u, 8192u,  16384u, 64436u,  5u, 0u, 0u,  384,  0,  0,     0 };
static const stg_emit_rec c3_fan    = { 9u, STG_DIR_ABS,  11u, 12384u, 1000u,  0u,    20u, 0u, 0u,  640, 48,  0,     0 };
static const stg_emit_rec c3_rain   = { 3u, STG_DIR_ABS,   4u, 16384u, 0u,     0u,     5u, 0u, 0u,  384,  0,  48640, 384 };
static const stg_emit_rec c3_aim    = { 1u, STG_DIR_AIM,   2u, 0u,     0u,     0u,    60u, 0u, 0u, 1024,  0,  0,     0 };
/* 终符：六个发射器受门控，interval 不起作用（写 1 让记录合法）；最大的一次齐射 24 发 */
static const stg_emit_rec c4_ring   = {24u, STG_DIR_ABS,   0u, 0u,     2731u,  0u,     1u, 0u, 0u,  384,  0,  0,     0 };
static const stg_emit_rec c4_aim    = { 5u, STG_DIR_AIM,  10u, 61440u, 2048u,  0u,     1u, 0u, 0u,  768,  0,  0,     0 };
static const stg_emit_rec c4_spir_a = { 3u, STG_DIR_SEQ,   5u, 0u,     21845u, 1200u,  1u, 0u, 0u,  512,  0,  0,     0 };
static const stg_emit_rec c4_spir_b = { 3u, STG_DIR_SEQ,   7u, 0u,     21845u, 64336u, 1u, 0u, 0u,  576,  0,  0,     0 };
static const stg_emit_rec c4_fan    = { 7u, STG_DIR_ABS,  11u, 13384u, 1000u,  0u,     1u, 0u, 0u,  640, 64,  0,     0 };
static const stg_emit_rec c4_rain   = { 4u, STG_DIR_ABS,   4u, 16384u, 0u,     0u,     1u, 0u, 0u,  512,  0,  48640, 512 };

#define AT_BOSS(r)      { &(r), STG_ANCHOR_BOSS, 0, 0 }
#define AT_ABS(r, x, y) { &(r), STG_ANCHOR_ABS, PX(x), PX(y) }

const stg_card stg_cards[STG_CARD_N] = {
    { "SPELL 1 RING+AIM", 300, 1800u,  0u, 0u, 2u,
      { AT_BOSS(c1_ring), AT_BOSS(c1_aim) } },
    { "SPELL 2 SPIRAL",   350, 1800u, 90u, 0u, 2u,
      { AT_BOSS(c2_spir_a), AT_BOSS(c2_spir_b) } },
    { "SPELL 3 FAN+RAIN", 400, 1800u, 90u, 0u, 3u,
      { AT_BOSS(c3_fan), AT_ABS(c3_rain, 200, 0), AT_BOSS(c3_aim) } },
    { "FINAL OVERLOAD",   600, 2400u, 90u, 1u, 6u,
      { AT_BOSS(c4_ring), AT_BOSS(c4_aim), AT_BOSS(c4_spir_a), AT_BOSS(c4_spir_b),
        AT_BOSS(c4_fan), AT_ABS(c4_rain, 200, 0) } },
};

/* --- EX 关卡（stg_stage.c 中的 EX 路线定义） ----------------------------------
   只换常量表，不动执行逻辑：同一套发射原语与机种，波次更密、排布不同。
   入口是「四个方向键同按」（判据按键脚本与录像搜索器不产生这种输入）。 */

/* EX 的发射记录：密度与速度整体上探，弹型仍取既有的五种。 */
static const stg_emit_rec ex_dart_fast   = { 2u, STG_DIR_AIM,  10u, 63488u, 1536u, 0u, 34u, 0u, 0u,  768,  0,  0,  0 };
static const stg_emit_rec ex_dart_ring   = {12u, STG_DIR_ABS,   6u, 2731u,  5461u, 0u, 36u, 0u, 0u,  512,  0,  0,  0 };
static const stg_emit_rec ex_cross_fan2  = { 7u, STG_DIR_AIM,  11u, 59392u, 2048u, 0u, 24u, 0u, 0u,  640,  0,  0,  0 };
static const stg_emit_rec ex_cross_ring  = {20u, STG_DIR_ABS,   1u, 0u,     3277u, 0u, 40u, 0u, 0u,  448,  0,  0,  0 };

static const stg_emit_rec *const exgrp_dart_fast[]  = { &ex_dart_fast };
static const stg_emit_rec *const exgrp_dart_ring[]  = { &ex_dart_ring };
static const stg_emit_rec *const exgrp_cross_fan2[] = { &ex_cross_fan2 };
static const stg_emit_rec *const exgrp_cross_ring[] = { &ex_cross_ring };

const stg_em_group stg_ex_em_groups[] = {
    { exgrp_dart_fast,  1u },     /* 0 */
    { exgrp_dart_ring,  1u },     /* 1 */
    { exgrp_cross_fan2, 1u },     /* 2 */
    { exgrp_cross_ring, 1u },     /* 3 */
};
const uint32_t stg_n_ex_em_groups = (uint32_t)(sizeof stg_ex_em_groups / sizeof stg_ex_em_groups[0]);

/* EX 的移动表：经典六条照抄 + 两条高速斜飞。 */
static const stg_move mv_dash_r[] = { {  768, 256, 1u } };
static const stg_move mv_dash_l[] = { { -768, 256, 1u } };

static const stg_move ex_mv_turn_r[]  = { { 0, 512, 80u }, {  384, -384, 1u } };
static const stg_move ex_mv_turn_l[]  = { { 0, 512, 80u }, { -384, -384, 1u } };
static const stg_move ex_mv_stop[]    = { { 0, 384, 100u }, { 0, -384, 240u }, { 0, -256, 1u } };
static const stg_move ex_mv_down[]    = { { 0, 640, 1u } };
static const stg_move ex_mv_diag_r[]  = { {  384, 256, 1u } };
static const stg_move ex_mv_diag_l[]  = { { -384, 256, 1u } };

const stg_move_tab stg_ex_moves[] = {
    { ex_mv_turn_r, 2u },       /* 0 下行后折向右下 */
    { ex_mv_turn_l, 2u },       /* 1 下行后折向左下 */
    { ex_mv_stop,   3u },       /* 2 下行、停、上行离场 */
    { ex_mv_down,   1u },       /* 3 直下 */
    { ex_mv_diag_r, 1u },       /* 4 从左侧斜向右下 */
    { ex_mv_diag_l, 1u },       /* 5 从右侧斜向左下 */
    { mv_dash_r,    1u },       /* 6 高速斜向右下 */
    { mv_dash_l,    1u },       /* 7 高速斜向左下 */
};
const uint32_t stg_n_ex_moves = (uint32_t)(sizeof stg_ex_moves / sizeof stg_ex_moves[0]);

const stg_event stg_ex_events[] = {
    /* 第 1 波：左右同时切入的高速 DART（新移动表 6/7） */
    SPAWN(  50, DART,  -20,  60, 6, 0, 20), SPAWN(  50, DART,  420,  60, 7, 0, 20),
    SPAWN(  90, DART,  -20,  60, 6, 0, 20), SPAWN(  90, DART,  420,  60, 7, 0, 20),
    SPAWN( 130, DART,  -20,  60, 6, 0, 20), SPAWN( 130, DART,  420,  60, 7, 0, 20),
    /* 第 2 波：三列直下 DART，快狙双发 */
    SPAWN( 300, DART,   80, -24, 3, 0, 20), SPAWN( 300, DART,  200, -24, 3, 0, 20),
    SPAWN( 300, DART,  320, -24, 3, 0, 20), SPAWN( 360, DART,   80, -24, 3, 0, 20),
    SPAWN( 360, DART,  200, -24, 3, 0, 20), SPAWN( 360, DART,  320, -24, 3, 0, 20),
    /* 第 3 波：中央 CROSS 放 12 向旋转环，两侧 DART 折返策应 */
    SPAWN( 560, CROSS, 200, -24, 2, 3, 60),
    SPAWN( 620, DART,  100, -24, 0, 1, 30), SPAWN( 620, DART,  300, -24, 1, 1, 30),
    /* 第 4 波：CROSS 双停位对狙扇 + 斜飞 DART 穿场 */
    SPAWN( 880, CROSS, 110, -24, 2, 2, 80), SPAWN( 880, CROSS, 290, -24, 2, 2, 80),
    SPAWN( 960, DART,  -20, 120, 6, 0, 20), SPAWN(1000, DART,  420, 120, 7, 0, 20),
    /* 第 5 波：三列直下 + 环十字压场 */
    SPAWN(1180, DART,   60, -24, 3, 1, 16), SPAWN(1180, DART,  200, -24, 3, 1, 16),
    SPAWN(1180, DART,  340, -24, 3, 1, 16), SPAWN(1240, CROSS, 200, -24, 2, 3, 60),
    /* 第 6 波：左右交替折返的连续快波 */
    SPAWN(1420, DART,  100, -24, 0, 0, 16), SPAWN(1444, DART,  300, -24, 1, 0, 16),
    SPAWN(1468, DART,  100, -24, 0, 0, 16), SPAWN(1492, DART,  300, -24, 1, 0, 16),
    SPAWN(1516, DART,  100, -24, 0, 0, 16), SPAWN(1540, DART,  300, -24, 1, 0, 16),
    SPAWN(1564, DART,  100, -24, 0, 0, 16), SPAWN(1588, DART,  300, -24, 1, 0, 16),
    /* 第 7 波：收官混编 */
    SPAWN(1720, CROSS, 140, -24, 2, 3, 60), SPAWN(1720, DART,  260, -24, 3, 1, 20),
    SPAWN(1780, DART,  260, -24, 3, 1, 20), SPAWN(1840, CROSS, 260, -24, 2, 2, 80),
    SPAWN(1840, DART,  140, -24, 3, 1, 20), SPAWN(1900, DART,  140, -24, 3, 1, 20),
    { 2060u, STG_EVT_CLEAR, 0u, 0, 0, 0u, 0u, 0u },
    { 2120u, STG_EVT_BOSS,  0u, 0, 0, 0u, 0u, 0u },
};
const uint32_t stg_ex_n_events = (uint32_t)(sizeof stg_ex_events / sizeof stg_ex_events[0]);

/* EX 的符卡：同构四张，弹幕编排换风格（旋转对称/双侧夹击/混合雨/超载+）。 */
static const stg_emit_rec x1_lotus   = {10u, STG_DIR_SEQ,   0u, 0u,     6554u, 1092u, 14u, 0u, 0u,  430,  0,  0, 0 };
static const stg_emit_rec x1_aim     = { 3u, STG_DIR_AIM,  10u, 63488u, 2048u, 0u,    36u, 0u, 0u,  832,  0,  0, 0 };
static const stg_emit_rec x2_fan_l   = { 7u, STG_DIR_AIM,  11u, 59392u, 1707u, 0u,    26u, 0u, 0u,  704,  0,  0, 0 };
static const stg_emit_rec x2_fan_r   = { 7u, STG_DIR_AIM,  11u, 6144u,  1707u, 0u,    26u, 0u, 0u,  704,  0,  0, 0 };
static const stg_emit_rec x2_ring    = {10u, STG_DIR_SEQ,   6u, 8192u,  6554u, 328u,  36u, 0u, 0u,  448,  0,  0, 0 };
static const stg_emit_rec x3_rain_a  = { 4u, STG_DIR_ABS,   4u, 16384u, 0u,    0u,     4u, 0u, 0u,  448,  0, 32768, 640 };
static const stg_emit_rec x3_rain_b  = { 3u, STG_DIR_ABS,   8u, 16384u, 0u,    0u,     5u, 0u, 0u,  512,  0, 24576, 512 };
static const stg_emit_rec x3_spir    = { 4u, STG_DIR_SEQ,   5u, 0u,    14564u, 1092u,  4u, 0u, 0u,  576,  0,  0, 0 };
static const stg_emit_rec x4_ring    = {26u, STG_DIR_ABS,   0u, 0u,     2521u, 0u,     1u, 0u, 0u,  448,  0,  0, 0 };
static const stg_emit_rec x4_aim     = { 6u, STG_DIR_AIM,  10u, 60672u, 2048u, 0u,     1u, 0u, 0u,  832,  0,  0, 0 };
static const stg_emit_rec x4_spir_a  = { 3u, STG_DIR_SEQ,   5u, 0u,    21845u, 1092u,  1u, 0u, 0u,  576,  0,  0, 0 };
static const stg_emit_rec x4_spir_b  = { 3u, STG_DIR_SEQ,   7u, 0u,    21845u, 64244u, 1u, 0u, 0u,  640,  0,  0, 0 };
static const stg_emit_rec x4_fan     = { 9u, STG_DIR_ABS,  11u, 12384u, 1000u, 0u,     1u, 0u, 0u,  704, 80,  0, 0 };
static const stg_emit_rec x4_rain    = { 5u, STG_DIR_ABS,   4u, 16384u, 0u,    0u,     1u, 0u, 0u,  576,  0, 40960, 576 };

const stg_card stg_ex_cards[STG_CARD_N] = {
    { "EX1 LOTUS RING",   380, 1800u,  0u, 0u, 2u,
      { AT_BOSS(x1_lotus), AT_BOSS(x1_aim) } },
    { "EX2 TWIN FAN",     420, 1800u, 90u, 0u, 3u,
      { AT_ABS(x2_fan_l,  60, 40), AT_ABS(x2_fan_r, 340, 40), AT_BOSS(x2_ring) } },
    { "EX3 STORM RAIN",   460, 1800u, 90u, 0u, 4u,
      { AT_ABS(x3_rain_a, 100, 0), AT_ABS(x3_rain_b, 300, 0), AT_BOSS(x3_spir), AT_BOSS(x1_aim) } },
    { "EX4 OVERLOAD+",    700, 2400u, 90u, 1u, 6u,
      { AT_BOSS(x4_ring), AT_BOSS(x4_aim), AT_BOSS(x4_spir_a), AT_BOSS(x4_spir_b),
        AT_BOSS(x4_fan), AT_ABS(x4_rain, 200, 0) } },
};

/* --- 关卡集 ------------------------------------------------------------------- */

const stg_stage_set stg_stage_classic = {
    stg_stage_events, (uint32_t)(sizeof stg_stage_events / sizeof stg_stage_events[0]),
    stg_moves,        stg_n_moves,
    stg_em_groups,    stg_n_em_groups,
    stg_cards,
};

const stg_stage_set stg_stage_ex = {
    stg_ex_events, stg_ex_n_events,
    stg_ex_moves,  stg_n_ex_moves,
    stg_ex_em_groups, stg_n_ex_em_groups,
    stg_ex_cards,
};

/* --- 背景 ------------------------------------------------------------------ */

void stg_bg_init(stg_bg *b, xs32 *rng)
{
    uint32_t i;

    b->scroll = 0u;
    b->phase  = 0u;
    for (i = 0u; i < STG_BG_STARS; i++) {
        stg_star *s = &b->star[i];

        s->x    = (int32_t)(xs32_next(rng) % 398u) * FX_ONE;           /* 2x2 落在 0..399 之内 */
        s->y    = (int32_t)(xs32_next(rng) % 480u) * FX_ONE;
        s->vy   = 307 + (int32_t)(xs32_next(rng) % 615u);               /* 1.2~3.6 像素/帧 */
        s->kind = xs32_next(rng) & 3u;
    }
}

void stg_bg_step(stg_bg *b)
{
    uint32_t i;

    b->scroll += STG_BG_SCROLL;
    if (b->scroll >= (uint32_t)STG_BG_SPACING_PX * FX_ONE) {
        b->scroll -= (uint32_t)STG_BG_SPACING_PX * FX_ONE;
        b->phase   = (b->phase + 1u) & 3u;
    }
    for (i = 0u; i < STG_BG_STARS; i++) {
        stg_star *s = &b->star[i];

        s->y += s->vy;
        if (s->y >= STG_FIELD_H * FX_ONE)
            s->y -= (STG_FIELD_H + 2) * FX_ONE;             /* 回到上沿外 */
    }
}

int32_t stg_bg_hline_y(const stg_bg *b, uint32_t k)
{
    return (int32_t)(b->scroll >> 8) + (int32_t)k * STG_BG_SPACING_PX;
}

/* 横线下移满 32 像素时第 k 条变成第 k+1 条，亮线的相位跟着转 */
int stg_bg_hline_bright(const stg_bg *b, uint32_t k)
{
    return ((k - b->phase) & 3u) == 0u;
}

int32_t stg_bg_vline_x(uint32_t k)
{
    return STG_BG_VX0_PX + (int32_t)k * STG_BG_SPACING_PX;
}

int stg_bg_vline_bright(uint32_t k)
{
    return (k & 3u) == 0u;
}
