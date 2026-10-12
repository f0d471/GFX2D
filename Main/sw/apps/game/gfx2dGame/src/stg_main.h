/******************************************************************************
* @file stg_main.h: 弹幕场的画面入口
*
* @brief  编译开关 EXTRA_CFLAGS=-DSTG_FIELD=<n> 选入口，不给时 gfx2dGame 仍是基础 Demo：
*           1  演示：五种弹型各 300 帧轮流，再是合并段 600 帧，循环；PC 上开窗口
*           2  判据：不读按键，五段各 120 帧、合并段 240 帧，逐帧一行 f，末尾 fail／done
*           3  N_max 搜索：模拟时钟，只做普查、不执行命令表
*           4  游戏的判据入口：按键脚本跑完一局，逐帧一行 g（整帧 CRC 与计数），末尾 fail／done
*         -DSTG_FRAMES=N 限总帧数（板上用）。
*
*         main.c 做完前三步启动（分区窗口、素材包、表面槽，不做预合成）之后把这些交过来；
*         表面槽用游戏那一套（stg_slots.h）。场或游戏的状态结构体是本入口文件里唯一的一份。
******************************************************************************/
#ifndef STG_MAIN_H
#define STG_MAIN_H

#include <stdint.h>

#include "gfx/gfx.h"

#ifndef SW9_PERF
#define SW9_PERF 0
#endif

#if defined(STG_FIELD)
#define STG_FIELD_MODE (STG_FIELD)
#else
#define STG_FIELD_MODE 0
#endif

typedef struct {
    const gfx_surface_table *tab;
    const gfx_mem           *mem;
    exec_sw_state           *es;
    exec_sw_error           *err;
    gfx_cmd                 *cmds[2];       /* 两张命令表，按全局帧序号的奇偶轮换 */
    uint32_t                 cap[2];
    int                      hw;
} stg_main_ctx;

/* fail0 是启动阶段已经记下的失败数（素材包对账不符等）。返回 0 表示 fail 0。 */
int stg_main(const stg_main_ctx *ctx, uint32_t fail0);

#endif /* STG_MAIN_H */
