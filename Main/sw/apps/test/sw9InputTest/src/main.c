/******************************************************************************
* @file main.c: SW9 串口输入、短按与失联松键的板上诊断
******************************************************************************/

#include <stdint.h>
#include "board.h"
#include "core/format.h"

int main(void)
{
    uint32_t f, last = 0u;
    board_init();
    if (board_irq_init() != 0)
        board_puts("irq init err\r\n");
    board_puts("sw9InputTest\r\n");
    for (f = 0u; f < 240u; f++) {
        const uint32_t keys = board_keys();
        if (keys != last) {
            char buf[80];
            core_out out;
            core_out_init(&out, buf, (uint32_t)sizeof buf);
            core_out_str(&out, "keys frame ");
            core_out_u32(&out, f);
            core_out_str(&out, " value ");
            core_out_hex32(&out, keys);
            core_out_str(&out, " pressed ");
            core_out_hex32(&out, keys & ~last);
            core_out_str(&out, "\r\n");
            board_puts(buf);
            last = keys;
        }
        board_wait_frame();
    }
    board_puts("done\r\n");
    return 0;
}
