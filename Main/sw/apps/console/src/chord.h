/******************************************************************************
* @file chord.h: HOME 和弦检测（纯逻辑，无平台依赖——host 单测直接编它）
*
* @brief  三模式键同按 N 帧触发一次的 armed 计帧器，语义与裸机 stg_chord
*         一致：组合键不齐即复位计数并重新武装；触发后保持按住不重复触发，
*         松开才重新武装。OSAL 的宿主侧：appfw.c 用 FRAME 常数实例化，
*         host 单测用小常数穷举。
******************************************************************************/
#ifndef CHORD_H
#define CHORD_H

#include <stdint.h>

#define CHORD_EV_FIRE (1u << 0)

typedef struct {
    uint32_t combo;      /* 触发组合（位或） */
    uint32_t hold;       /* 需连续保持的帧数 */
    uint32_t n;          /* 连续计数 */
    int      armed;      /* 触发后保持按住不重触发；松开重新武装 */
} chord;

void     chord_init(chord *c, uint32_t combo, uint32_t hold);
uint32_t chord_step(chord *c, uint32_t keys);

#endif /* CHORD_H */
