/******************************************************************************
* @file sw6_input.c: 本帧的输入位图与按下沿
*
* @brief  实现见 sw6_input.h 的文件头。平台服务只经 board.h 取，分层不变。
******************************************************************************/

#include <stddef.h>

#include "board.h"

#include "sw6_input.h"

void sw6_input_live(sw6_input *in, uint32_t seed)
{
    in->replay = 0;
    in->seed   = seed;
    in->keys   = 0u;
    in->prev   = 0u;
    in->fed    = 0u;
}

void sw6_input_fed(sw6_input *in, uint32_t seed)
{
    sw6_input_live(in, seed);
    in->replay = 2;
}

void sw6_input_feed(sw6_input *in, uint32_t keys)
{
    in->fed = keys;
}

int sw6_input_replay(sw6_input *in, const uint8_t *buf, uint32_t len,
                     uint32_t *out_frames)
{
    int rc = replay_open(buf, len, &in->view);

    if (rc != 0)
        return rc;
    in->replay = 1;
    in->seed   = in->view.seed;
    in->keys   = 0u;
    in->prev   = 0u;
    in->fed    = 0u;
    if (out_frames != NULL)
        *out_frames = in->view.frames;
    return 0;
}

uint32_t sw6_input_next(sw6_input *in, uint32_t frame)
{
    uint32_t keys = 0u;

    if (in->replay == 1) {
        if (replay_key_at(&in->view, frame, &keys) != 0)
            keys = 0u;              /* 越界：录像头写的帧数与调用方不一致 */
    } else if (in->replay == 2) {
        keys = in->fed;
    } else {
        keys = board_keys();        /* 每帧只取一次 */
    }
    in->prev = in->keys;
    in->keys = keys;
    return keys;
}

int sw6_input_pressed(const sw6_input *in, uint32_t bit)
{
    return ((in->keys & bit) != 0u) && ((in->prev & bit) == 0u);
}
