/******************************************************************************
*
* @file main.c: keyTest
*
* @brief  SoC IP 定型之后，确认板上四个按键 KEY0~3 经 GPIO1 读得到。
*
*         顶层把 key_n[3:0] 接到 GPIO1 的读口 [3:0]，[7:4] 接 0。按键低有效：
*         不按为 1，按下为 0。本程序分两段：
*
*         1. 空闲读数。启动时要求没有按任何键，GPIO1 读口应为 0x0f。
*            读到别的值说明管脚、上下拉或接线不对，这一项计入 fail。
*         2. 按键观测。在 WINDOW_S 秒内每毫秒采样一次，读口变化就打一行，
*            并记下每个键有没有出现过「按下」与「松开」。
*            结束时四个键都要各出现过一次按下与松开，缺哪个就计 fail。
*            这一段需要人在窗口内把四个键逐个按一下。
*
*         GPIO1 的基址来自 sdk 的 soc.h（SYSTEM_GPIO_1_IO_CTRL，0xF801_6000）。
*         若 SoC 没有开 GPIO1，这个宏不存在，程序编不过，而不是运行时读到 0。
*
******************************************************************************/

#include <stdint.h>
#include "bsp.h"
#include "gpio.h"

#ifndef SYSTEM_GPIO_1_IO_CTRL
#error "soc.h 里没有 GPIO1：SoC IP 没有开 GPIO1"
#endif

#define GPIO1       SYSTEM_GPIO_1_IO_CTRL
#define KEY_MASK    0x0Fu
#define IDLE        0x0Fu
#define WINDOW_S    30

static uint32_t read_keys(void)
{
    return gpio_getInput(GPIO1) & 0xFFu;
}

void main(void)
{
    int fail = 0;
    uint32_t seen_down = 0, seen_up = 0;

    bsp_printf("keyTest\r\n");

    uint32_t v = read_keys();
    int ok = (v == IDLE);
    fail += !ok;
    bsp_printf("idle expect=0x0f got=0x%x %s\r\n", (unsigned)v, ok ? "ok" : "FAIL");

    bsp_printf("press KEY0~3 one by one within %d s\r\n", WINDOW_S);
    uint32_t last = v;
    for (uint32_t ms = 0; ms < WINDOW_S * 1000u; ms++) {
        bsp_uDelay(1000);
        uint32_t now = read_keys();
        if (now == last)
            continue;
        uint32_t changed = (now ^ last) & KEY_MASK;
        for (int k = 0; k < 4; k++) {
            if (!(changed & (1u << k)))
                continue;
            int down = !(now & (1u << k));
            if (down)
                seen_down |= 1u << k;
            else
                seen_up |= 1u << k;
            bsp_printf("t=%d ms KEY%d %s raw=0x%x\r\n", (int)ms, k, down ? "down" : "up", (unsigned)now);
        }
        if ((now ^ last) & ~KEY_MASK) {
            fail++;
            bsp_printf("t=%d ms high bits changed raw=0x%x FAIL\r\n", (int)ms, (unsigned)now);
        }
        last = now;
    }

    for (int k = 0; k < 4; k++) {
        int d = !!(seen_down & (1u << k)), u = !!(seen_up & (1u << k));
        int good = d && u;
        fail += !good;
        bsp_printf("KEY%d down=%d up=%d %s\r\n", k, d, u, good ? "ok" : "FAIL");
    }
    bsp_printf("fail %d\r\n", fail);
    bsp_printf("done\r\n");
}
