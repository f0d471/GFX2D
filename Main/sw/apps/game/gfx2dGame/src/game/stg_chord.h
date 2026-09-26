/******************************************************************************
* @file stg_chord.h: 模式框架的和弦检测
*
* @brief  SW8 2.1 节。板上只有四个方向键，没有数字键与 Tab，用对立方向的和弦当系统键：
*           上 + 下同时按住满 8 帧   触发一次「下一个模式」
*           左 + 右同时按住满 8 帧   触发一次「切后端」
*         触发之后要这一组的两个键都松开才会再次触发（按住不连发）。按帧计（框架每推进一次算一帧），
*         不按场，所以录像回放是确定的。游戏那一侧把对立方向当作该轴不动（SW7 2.4 节），两者不冲突，
*         和弦期间的按键照常交给当前模式。
*
*         纯函数加一个小状态结构体，不链 bsp，app 测试里 64/32 比对。
*
*         注错开关（app 测试用，见 check_sw8_stats.sh）：
*           INJECT_CHORD_RETRIGGER  触发之后不等松开，按住满 8 帧就再触发一次
******************************************************************************/
#ifndef STG_CHORD_H
#define STG_CHORD_H

#include <stdint.h>

#define STG_CHORD_HOLD      8u          /* 按住满几帧触发 */

/* 事件位 */
#define STG_CHORD_NEXT_MODE (1u << 0)
#define STG_CHORD_SWAP      (1u << 1)

typedef struct {
    uint32_t ud_n, lr_n;    /* 两组和弦各按住了几帧 */
    uint32_t ud_armed;      /* 1：可以触发；0：触发过，等两个键都松开 */
    uint32_t lr_armed;
} stg_chord;

void     stg_chord_init(stg_chord *c);

/* 推进一帧。up/down/left/right 是方向键的位（与按键位图同一套），返回本帧触发的 STG_CHORD_* 事件。 */
uint32_t stg_chord_step(stg_chord *c, uint32_t keys, uint32_t up, uint32_t down,
                        uint32_t left, uint32_t right);

#endif /* STG_CHORD_H */
