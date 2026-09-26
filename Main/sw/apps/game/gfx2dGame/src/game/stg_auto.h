/******************************************************************************
* @file stg_auto.h: 自动驾驶
*
* @brief  SW8 2.6 节（K3 定案：做）。两件：
*           - 接管：框架连续 STG_AUTO_IDLE（600）帧按键位图为 0 就接管，任何一帧有按键就立即交还、
*             这一帧用人的按键（stg_auto_take）。按帧计、不按场：同一份输入在 PC 与板上接管的帧相同。
*           - 选方向：九个候选按固定顺序（不动、上、下、左、右、左上、右上、左下、右下），每个候选是
*             「自机按高速走 STG_AUTO_LOOK（12）帧之后的位置」（夹在移动范围内）。危险度由 stg_pool_danger
*             一遍扫敌弹池给出（每颗外推 12 帧，离自机任一轴超过 88 像素的跳过，d² < 40² 时加 40² − d²），
*             再加偏好项：离「Boss 的 x（没有 Boss 时 200）、y = 400」的曼哈顿距离乘 STG_AUTO_PREF_W。
*             取两者之和最小的候选，**平局取顺序在前的**；输出那个方向的按键位图，走游戏原有的输入路径。
*         纯整数、确定性，不改游戏状态：同一局面给出同一个按键位图。不按低速、不按炸弹（受击时自动炸弹照常）。
*         只在模式 2（游戏）与游戏的判据入口（-DSTG_JUDGE_AUTO=1）里用。
*
*         注错开关（app 测试与 check_sw8_stats.sh 用）：
*           INJECT_AUTO_ORDER  候选顺序倒过来（平局的取法变了）
******************************************************************************/
#ifndef STG_AUTO_H
#define STG_AUTO_H

#include <stdint.h>

#include "stg_game.h"

#define STG_AUTO_IDLE       600u        /* 连续多少帧无按键就接管 */
#define STG_AUTO_LOOK       12          /* 外推帧数 */
#define STG_AUTO_R_PX       40          /* 危险半径，像素 */
#define STG_AUTO_SPREAD_PX  48          /* 候选离自机的最大轴向距离：高速 4 像素/帧 × 12 帧 */
#define STG_AUTO_REACH_PX   (STG_AUTO_SPREAD_PX + STG_AUTO_R_PX)    /* 粗筛：88 像素 */
#define STG_AUTO_PREF_W     2u          /* 偏好项每像素的权重 */
#define STG_AUTO_HOME_X     200         /* 没有 Boss 时偏好的 x，像素 */
#define STG_AUTO_HOME_Y     400         /* 偏好的 y，像素 */
#define STG_AUTO_N          9u          /* 候选数 */

/* 接管：keys 是框架这一帧的按键位图。有按键时计数清零、返回 0（用人的按键）；
   无按键时，已经连续 STG_AUTO_IDLE 帧无按键就返回 1（自动驾驶接管），否则计数加一、返回 0。 */
int      stg_auto_take(uint32_t *idle, uint32_t keys);

/* 这一帧的按键位图（STG_KEY_* 的方向位，不动时为 0）。risk 不为 NULL 时写九个候选的总分
   （危险度加偏好，按固定顺序），给测试看。 */
uint32_t stg_auto_keys(const stg_game *g, uint32_t *risk);

/* 第 i 个候选（固定顺序）的按键位图，越界返回 0。 */
uint32_t stg_auto_cand_keys(uint32_t i);

#endif /* STG_AUTO_H */
