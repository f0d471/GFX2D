/******************************************************************************
* @file check_sw8_tx.c: board_tx_room() 的 PC 实现的主机侧探针
*
* @brief  SW8 2.4 节。bsp/host 要 SDL，编不进 lib/test，所以单独做一支探针，
*         链的是**真实的** sw/bsp/host/board.c 与 sw/bsp/memmap.c，测的就是那段代码。
*         由 check_sw8_stats.sh 编译并跑两遍：
*
*           不设 GFX2D_TX_BUDGET   X1  board_tx_room() 返回 0x7FFFFFFF，board_puts() 不改变它
*           GFX2D_TX_BUDGET=192    X2  对齐到场起始之后额度是 192；board_puts 100 字节剩 92、再 50 剩 42；
*                                      再 60 超额：额度归零、超额一次 18 字节
*                                  X3  等一场之后补满到 192；再等三场仍是 192（不累加）
*                                  X4  退出时 stderr 打出 "over 1 times 18 bytes"（由脚本核对）
*
*         X2 要求几次调用落在同一场里：先 board_wait_frame() 对齐到场起始，
*         做完再看场号有没有变；变了（恰好跨过场边界）就整组重来，最多三次。
*
*         注错 -DINJECT_HOST_TX_NO_REFILL 让 X3 见红。
*         退出码 0 表示通过；末尾打 fail 与 done。编译与运行见 Main/tools/check/README.md。
******************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"

static int g_fail;

static void ck(const char *tag, int ok)
{
    fprintf(stdout, "%s %s\n", tag, ok ? "ok" : "FAIL");
    fflush(stdout);
    if (!ok)
        g_fail++;
}

/* 打一行恰好 n 字节（含换行）的日志 */
static void put_n(uint32_t n)
{
    static char line[256];

    memset(line, '.', n);
    line[n - 1u] = '\n';
    line[n] = '\0';
    board_puts(line);
}

int main(void)
{
    const char *env = getenv("GFX2D_TX_BUDGET");

    board_init();

    if (env == NULL || env[0] == '\0') {
        uint32_t r0 = board_tx_room();

        put_n(100u);
        ck("X1 no GFX2D_TX_BUDGET: room 0x7FFFFFFF before and after a 100-byte puts",
           r0 == BOARD_TX_ROOM_UNLIMITED && board_tx_room() == BOARD_TX_ROOM_UNLIMITED);
    } else {
        uint32_t tries, r0 = 0u, r1 = 0u, r2 = 0u, r3 = 0u, i;
        int      same = 0;

        for (tries = 0u; tries < 3u && !same; tries++) {
            uint32_t f0;

            board_wait_frame();
            f0 = board_frame_count();
            r0 = board_tx_room();
            put_n(100u);
            r1 = board_tx_room();
            put_n(50u);
            r2 = board_tx_room();
            put_n(60u);
            r3 = board_tx_room();
            same = (board_frame_count() == f0);
        }
        ck("X2 same field: room 192 -> 100B -> 92 -> 50B -> 42 -> 60B -> 0",
           same && r0 == 192u && r1 == 92u && r2 == 42u && r3 == 0u);
        /* 脚本据此核对退出时的超额汇总：只试了一次时必须恰好 "over 1 times 18 bytes" */
        fprintf(stdout, "X2 tries %u\n", (unsigned)tries);

        board_wait_frame();
        r0 = board_tx_room();
        for (i = 0u; i < 3u; i++)
            board_wait_frame();
        r1 = board_tx_room();
        ck("X3 next field refills to 192; three more fields still 192 (no accumulation)",
           r0 == 192u && r1 == 192u);
    }

    fprintf(stdout, "fail %d\ndone\n", g_fail);
    return (g_fail == 0) ? 0 : 1;
}
