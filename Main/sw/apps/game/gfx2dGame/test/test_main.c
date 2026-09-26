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
*
*         三种用法：
*           不带参数          各组单项检查，末尾 fail／done
*           pat               五种弹型加合并段的轨迹（SW7 第 3 节「弹型」），与期望文件比
*           dump <段> <帧>    某一段某一帧之后的全部敌弹，轨迹不一致时找第一颗不同的弹
******************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/crc32.h"
#include "core/format.h"
#include "test_stg_auto.h"
#include "test_stg_chord.h"
#include "test_stg_collide.h"
#include "test_stg_draw.h"
#include "test_stg_emit.h"
#include "test_stg_game.h"
#include "test_stg_pool.h"
#include "test_stg_press.h"
#include "test_stg_stage.h"
#include "test_stg_view.h"

int main(int argc, char **argv)
{
    uint32_t fail = 0u;
    char     line[48];
    core_out o;

    crc32_init();

    if (argc == 2 && strcmp(argv[1], "pat") == 0) {
        stg_pat_trace();
        return 0;
    }
    if (argc == 4 && strcmp(argv[1], "dump") == 0)
        return (stg_pat_dump((uint32_t)strtoul(argv[2], 0, 10),
                             (uint32_t)strtoul(argv[3], 0, 10)) == 0) ? 0 : 2;
    if (argc != 1) {
        fputs("usage: stgTest [pat | dump <seg> <frame>]\n", stderr);
        return 2;
    }

    fputs("stgTest pool\n", stdout);
    fail += test_stg_pool_run();
    fputs("stgTest emit\n", stdout);
    fail += test_stg_emit_run();
    fputs("stgTest collide\n", stdout);
    fail += test_stg_collide_run();
    fputs("stgTest draw\n", stdout);
    fail += test_stg_draw_run();
    fputs("stgTest press\n", stdout);
    fail += test_stg_press_run();
    fputs("stgTest game\n", stdout);
    fail += test_stg_game_run();
    fputs("stgTest stage\n", stdout);
    fail += test_stg_stage_run();
    fputs("stgTest view\n", stdout);
    fail += test_stg_view_run();
    fputs("stgTest chord\n", stdout);
    fail += test_stg_chord_run();
    fputs("stgTest auto\n", stdout);
    fail += test_stg_auto_run();

    core_out_init(&o, line, (uint32_t)sizeof line);
    core_out_str(&o, "fail ");
    core_out_u32(&o, fail);
    core_out_str(&o, "\n");
    fputs(line, stdout);
    fputs("done\n", stdout);
    return (fail == 0u) ? 0 : 1;
}
