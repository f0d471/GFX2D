/******************************************************************************
* @file test_main.c: 游戏代码（src/game/stg_*）的主机侧测试
*
* @brief  游戏代码是应用的一部分，`lib/test` 的编译只有 -Isw/lib，看不到它，
*         所以测试放在应用目录下（SW7 2.2 节与 4.5 节第 5 条）。
*
*         由 Main/tools/check/check_sw7_game.sh 编译：选项与 mk/host.mk 的 test 目标相同，
*         64 位挂 ASan/UBSan、-m32 各一份，两份输出必须逐字节相同。
*         只链 lib/ 与被测的 stg_*.c，**不链 bsp**：-m32 下没有 32 位 SDL，
*         而且不链 bsp 本身就证明了游戏逻辑不依赖平台。
******************************************************************************/

#include <stdio.h>

#include "core/format.h"
#include "test_stg_pool.h"

int main(void)
{
    uint32_t fail = 0u;
    char     line[48];
    core_out o;

    fputs("stgTest pool\n", stdout);
    fail += test_stg_pool_run();

    core_out_init(&o, line, (uint32_t)sizeof line);
    core_out_str(&o, "fail ");
    core_out_u32(&o, fail);
    core_out_str(&o, "\n");
    fputs(line, stdout);
    fputs("done\n", stdout);
    return (fail == 0u) ? 0 : 1;
}
