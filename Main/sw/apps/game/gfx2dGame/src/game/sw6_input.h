/******************************************************************************
* @file sw6_input.h: 本帧的输入位图与按下沿
*
* @brief  两个输入源，判据与演示各用一个（spec 2.0 第 3 条）：
*           - 实时：`board_keys()`，每帧只调一次；
*           - 录像：`replay_open()` + `replay_key_at()`，种子取录像头里的那个字段。
*         模式框架（SW8 2.1 节）另有第三种「由调用方喂」：框架每帧只读一次按键（或用合成脚本），
*         处理和弦之后用 sw6_input_feed() 交给模式 1，本层不再调 `board_keys()`。
*         判据模式**不读实时按键**——键盘状态进了画面，两次运行就不可能逐帧相同。
*
*         按下沿由本层判：当前位图有、上一帧没有。上一帧初值 0，
*         所以录像第一帧的按键就算一次按下沿（生成器为此把每步写成
*         「按 1 帧、松 1 帧」，一个沿对应一次步进）。
******************************************************************************/
#ifndef SW6_INPUT_H
#define SW6_INPUT_H

#include <stdint.h>

#include "core/replay.h"

typedef struct {
    int          replay;    /* 0 = 实时，1 = 录像，2 = 由调用方喂 */
    replay_view  view;
    uint32_t     seed;
    uint32_t     keys;      /* 本帧位图 */
    uint32_t     prev;      /* 上一帧位图，按下沿用 */
    uint32_t     fed;       /* 由调用方喂时，下一次 sw6_input_next 取的位图 */
} sw6_input;

/* 实时输入。seed 只用于报给上层（判据模式不读它）。 */
void     sw6_input_live(sw6_input *in, uint32_t seed);

/* 由调用方喂的输入（模式框架用）：每帧在 sw6_input_next 之前调 sw6_input_feed。 */
void     sw6_input_fed(sw6_input *in, uint32_t seed);
void     sw6_input_feed(sw6_input *in, uint32_t keys);

/* 录像输入。成功返回 0 并把帧数写进 *out_frames；失败返回 REPLAY_E_*。 */
int      sw6_input_replay(sw6_input *in, const uint8_t *buf, uint32_t len,
                          uint32_t *out_frames);

/* 取第 frame 帧的位图（每帧只调一次）并更新上一帧。越界返回 0。 */
uint32_t sw6_input_next(sw6_input *in, uint32_t frame);

/* 本帧相对上一帧的按下沿。必须在 sw6_input_next 之后调用。 */
int      sw6_input_pressed(const sw6_input *in, uint32_t bit);

#endif /* SW6_INPUT_H */
