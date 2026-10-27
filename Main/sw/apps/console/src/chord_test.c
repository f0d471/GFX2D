/******************************************************************************
* @file test_chord.c: HOME 和弦逻辑的宿主单测（OSAL，SW14）
*
* @brief  纯逻辑穷举：触发帧数、复位、re-arm、额外键位、边界。构建：
*           cd sw/apps/console/test && make
*         产物 build_host/test_chord，末行 fail N（0 = 全过）。
******************************************************************************/

#include <stdio.h>

#include "chord.h"

static uint32_t s_fail;

#define CHECK(cond, name)                                                    \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL %s\n", name);                                       \
            s_fail++;                                                        \
        } else {                                                             \
            printf("ok %s\n", name);                                         \
        }                                                                    \
    } while (0)

int main(void)
{
    chord c;
    uint32_t i, ev;

    /* 1. 恰好第 8 帧触发；第 7 帧不触发 */
    chord_init(&c, 0x380u, 8u);
    ev = 0u;
    for (i = 0u; i < 7u; i++)
        ev |= chord_step(&c, 0x380u);
    CHECK(ev == 0u, "hold7-no-fire");
    ev = chord_step(&c, 0x380u);
    CHECK(ev == CHORD_EV_FIRE, "hold8-fires");

    /* 2. 触发后保持按住不重复触发 */
    ev = chord_step(&c, 0x380u);
    CHECK(ev == 0u, "held-after-fire-no-refire");

    /* 3. 松开重新武装；再按 8 帧再次触发 */
    (void)chord_step(&c, 0u);
    ev = 0u;
    for (i = 0u; i < 8u; i++)
        ev |= chord_step(&c, 0x380u);
    CHECK(ev == CHORD_EV_FIRE, "rearm-after-release");

    /* 4. 中途断开复位计数 */
    chord_init(&c, 0x380u, 8u);
    for (i = 0u; i < 5u; i++)
        (void)chord_step(&c, 0x380u);
    (void)chord_step(&c, 0x200u);            /* 断开一角 */
    ev = 0u;
    for (i = 0u; i < 7u; i++)
        ev |= chord_step(&c, 0x380u);        /* 只有 7 帧连续 */
    CHECK(ev == 0u, "break-resets-count");
    ev = chord_step(&c, 0x380u);
    CHECK(ev == CHORD_EV_FIRE, "8th-after-break-fires");

    /* 5. 组合之外的多余键位不影响计数 */
    chord_init(&c, 0x380u, 8u);
    ev = 0u;
    for (i = 0u; i < 8u; i++)
        ev |= chord_step(&c, 0x380u | 0x10u /*BOMB*/);
    CHECK(ev == CHORD_EV_FIRE, "extra-keys-count");

    /* 6. 单键长按不触发（MODE2 一路按住 100 帧） */
    chord_init(&c, 0x380u, 8u);
    ev = 0u;
    for (i = 0u; i < 100u; i++)
        ev |= chord_step(&c, 0x100u);
    CHECK(ev == 0u, "single-key-no-fire");

    /* 7. 交错帧数下的 armed 语义：触发后立刻再计也不重复，必须先断开 */
    chord_init(&c, 0x380u, 3u);
    (void)chord_step(&c, 0x380u);
    (void)chord_step(&c, 0x380u);
    ev = chord_step(&c, 0x380u);
    CHECK(ev == CHORD_EV_FIRE, "hold3-fires");
    ev = chord_step(&c, 0x381u);             /* 仍含组合但 armed=0 */
    CHECK(ev == 0u, "no-refire-while-held");
    (void)chord_step(&c, 0u);
    ev = chord_step(&c, 0x380u | 0x02u);
    ev |= chord_step(&c, 0x380u);
    ev |= chord_step(&c, 0x380u);
    CHECK(ev == CHORD_EV_FIRE, "rearm-with-hold3");

    printf("fail %u\n", (unsigned)s_fail);
    return (s_fail == 0u) ? 0 : 1;
}
