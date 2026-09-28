/******************************************************************************
* @file stg_script.h: 游戏判据入口的按键脚本
*
* @brief  sw 第 06 章 3.13 节（游戏的判据入口）。判据不读实时按键，输入是这份固定的脚本：
*         xorshift32 按固定种子，每 30 帧取一个数换一次方向（9 种之一：不动或八个方向），
*         同一个数的另一段决定这 30 帧是否按住低速（四分之一的机会）；第 900 帧按一次炸弹。
*         同一个帧号序列两次给出同一串按键位图，与平台无关。
******************************************************************************/
#ifndef STG_SCRIPT_H
#define STG_SCRIPT_H

#include <stdint.h>

#include "core/xorshift32.h"

#define STG_SCRIPT_SEED     0x5E7B0C05u
#define STG_SCRIPT_PERIOD   30u
#define STG_SCRIPT_BOMB_AT  900u

typedef struct {
    xs32     rng;
    uint32_t keys;          /* 这 30 帧的方向与低速 */
} stg_script;

void     stg_script_init(stg_script *s);

/* 第 frame 帧的按键位图（STG_KEY_*）。帧号须从 0 起逐帧递增地调用。 */
uint32_t stg_script_keys(stg_script *s, uint32_t frame);

#endif /* STG_SCRIPT_H */
