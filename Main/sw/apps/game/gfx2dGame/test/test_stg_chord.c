/******************************************************************************
* @file test_stg_chord.c: 模式框架的和弦检测（SW8 2.1 节）的主机侧测试
*
* @brief  H 组，判据见 SW8 第 3 节「系统键和弦」一行：
*           H1  上 + 下按住 7 帧不触发、第 8 帧触发一次「下一个模式」，按住不放 200 帧不再触发
*           H2  只松开一个键再按回去不重新武装；两个都松开之后再按住 8 帧才再触发一次
*           H3  左 + 右同样，触发的是「切后端」；两组和弦同时按住，第 8 帧两个事件同时出
*           H4  按住 4 帧、松开、再按住 4 帧：计数清零，不触发；只按上或只按下永不触发；
*               炸弹、低速等其它键不影响
*         注错 CHORD_RETRIGGER（不等松开就再触发）必须让 H1 的「不再触发」那一行见红。
******************************************************************************/

#include <stdint.h>
#include <stdio.h>

#include "core/format.h"
#include "game/stg_chord.h"
#include "game/stg_game.h"
#include "test_stg_chord.h"

#define U   STG_KEY_UP
#define D   STG_KEY_DOWN
#define L   STG_KEY_LEFT
#define R   STG_KEY_RIGHT

static uint32_t g_fail;

static void ck(const char *tag, int ok)
{
    char     line[200];
    core_out o;

    core_out_init(&o, line, (uint32_t)sizeof line);
    core_out_str(&o, tag);
    core_out_str(&o, ok ? " ok\n" : " FAIL\n");
    fputs(line, stdout);
    fflush(stdout);
    if (!ok)
        g_fail++;
}

/* 按住 keys n 帧，返回这段时间里各事件的次数与最后一次事件出现在第几帧（从 1 数） */
typedef struct {
    uint32_t next, swap, last_next, last_swap;
} run_res;

static run_res hold(stg_chord *c, uint32_t keys, uint32_t n)
{
    run_res  r = { 0u, 0u, 0u, 0u };
    uint32_t i;

    for (i = 1u; i <= n; i++) {
        const uint32_t ev = stg_chord_step(c, keys, U, D, L, R);

        if (ev & STG_CHORD_NEXT_MODE) {
            r.next++;
            r.last_next = i;
        }
        if (ev & STG_CHORD_SWAP) {
            r.swap++;
            r.last_swap = i;
        }
    }
    return r;
}

uint32_t test_stg_chord_run(void)
{
    stg_chord c;
    run_res   r;
    int       ok;

    g_fail = 0u;

    stg_chord_init(&c);
    r = hold(&c, U | D, 7u);
    ok = (r.next == 0u);
    r = hold(&c, U | D, 1u);
    ok &= (r.next == 1u && r.swap == 0u);
    ck("H1 up+down: 7 frames no trigger, the 8th frame triggers NEXT_MODE once", ok);
    r = hold(&c, U | D, 200u);
    ck("H1 held 200 more frames: no retrigger", r.next == 0u);

    r = hold(&c, U, 3u);                        /* 只松开下 */
    ok = (r.next == 0u);
    r = hold(&c, U | D, 20u);
    ok &= (r.next == 0u);
    r = hold(&c, 0u, 1u);                       /* 两个都松开 */
    r = hold(&c, U | D, 8u);
    ok &= (r.next == 1u && r.last_next == 8u);
    ck("H2 releasing one key does not re-arm; after both released, 8 frames trigger again", ok);

    stg_chord_init(&c);
    r = hold(&c, L | R, 8u);
    ok = (r.swap == 1u && r.last_swap == 8u && r.next == 0u);
    stg_chord_init(&c);
    r = hold(&c, U | D | L | R, 8u);
    ok &= (r.next == 1u && r.swap == 1u && r.last_next == 8u && r.last_swap == 8u);
    ck("H3 left+right triggers SWAP on the 8th frame; both chords together fire both events", ok);

    stg_chord_init(&c);
    r = hold(&c, U | D, 4u);
    ok = (r.next == 0u);
    r = hold(&c, 0u, 1u);
    r = hold(&c, U | D, 4u);
    ok &= (r.next == 0u);
    r = hold(&c, U, 50u);
    ok &= (r.next == 0u);
    r = hold(&c, D | STG_KEY_BOMB, 50u);
    ok &= (r.next == 0u);
    r = hold(&c, 0u, 1u);
    r = hold(&c, U | D | STG_KEY_BOMB | STG_KEY_SLOW, 8u);
    ok &= (r.next == 1u && r.last_next == 8u);
    ck("H4 4+4 frames with a gap: no trigger; single keys never; other keys do not interfere", ok);
    return g_fail;
}
