/******************************************************************************
* @file stg_modes.h: 模式框架里的模式 2（游戏）与模式 3（压力测试）
*
* @brief  SW8 2.1、2.3 节。两个模式共用一份游戏状态（stg_game，本文件里唯一的一份）：
*         离开模式 2、3 与进入它们时都整体重置（池清空、HI-SCORE 清零），所以切进来之后的第一帧
*         与冷启动直接进这个模式的第一帧相同。
*
*           模式 2  游戏：按键交给 stg_game_step，整帧组帧（含 HUD）、执行、呈现。连续 600 帧无按键时
*                   自动驾驶接管（stg_auto，SW8 2.6 节），任何按键立即交还；进入模式 2 时计数清零。
*           模式 3  压力测试：stg_game_press_begin 把游戏放进终符的压力形态，lib/core/nmax 的旋钮就是
*                   终符门控的 K；16x16 圆弹（弹种 0）搜一遍、8x8 小圆弹（弹种 4）搜一遍，各打 nmax cfg、
*                   逐档 nmax probe 与 nmax result；搜完停在 8x8 结果那一档继续跑。SETTLE_MIN 取 300 帧
*                   （SW8 2.3 节「终符的 SETTLE_MIN」），其余照 B 批。
*                   时钟：-DGFX2D_PRESS_CLOCK=0（缺省）用模拟时钟，e 与帧时间由本帧整帧的普查按 B 批的代价模型算；
*                   =1 用真实的场数增量与 board_cycles()，给 SW9 上板用。HUD 的段名那一行显示搜索的档或结果，标 SIM 或 REAL。
*                   模拟时钟下 HUD 的 SW FPS 一律 --：代价含 HUD，实测值的位数随墙钟变会让 N_max 随机器变。
*                   -DGFX2D_PRESS_KNOB=<K>（K 不为 0）时不搜索：16x16 圆弹、旋钮固定为 K 一直跑，进入时打一行
*                     press fixed knob <K> kind 16x16
*                   HUD 段名那一行显示 SIM K <K>（或 REAL K <K>）。给场景 S1 规模的统计与截帧用（SW7 第 3 节「规模」、
*                   SW8 第 3 节「第一版统计」）；缺省 0，照常搜索。
*
*         每个模式一帧：推进、组帧、普查自查、执行、整帧 CRC、呈现（dst 由框架按全局帧序号给）。
*         表面槽用游戏那一套（stg_slots.h），进入时由框架登记。
*
*         注错开关（检查脚本用，见 check_sw8_stats.sh）：
*           INJECT_MODE_KEEP_POOL  重置游戏状态时漏清敌弹池（敌弹的存活数原样留下）
******************************************************************************/
#ifndef STG_MODES_H
#define STG_MODES_H

#include <stdint.h>

#include "core/logq.h"
#include "gfx/gfx.h"

#include "game/stg_draw.h"

#include "stg_main.h"

#ifndef GFX2D_PRESS_CLOCK
#define GFX2D_PRESS_CLOCK   0
#endif

#ifndef GFX2D_PRESS_KNOB
#define GFX2D_PRESS_KNOB    0           /* 非 0：模式 3 不搜索，旋钮固定为它 */
#endif

#define STG_MODES_SETTLE_MIN    300u    /* 终符的 SETTLE_MIN（SW8 2.3 节） */

typedef struct {
    uint32_t        crc;        /* 整帧 640x480 的 CRC32 */
    stg_frame_stats st;         /* 组帧的计数 */
    uint32_t        list_idx;   /* 这一帧用的是哪一张命令表（0、1） */
    uint32_t        used;       /* 命令表的条数 */
    exec_sw_stats   xs;         /* 执行器的统计 */
    uint32_t        cyc_logic, cyc_build, cyc_exec;     /* 推进、组帧、执行三段的周期数 */
    uint32_t        t_present;  /* 发出呈现请求前一刻的 board_cycles()：帧时间的终点（SW8 2.2 节，不含等场） */
    uint32_t        cyc_auto;   /* 自动驾驶（接管判断与选方向）的周期数；模式 3 为 0 */
    uint32_t        auto_on;    /* 这一帧由自动驾驶给按键 */
} stg_mode_out;

/* 游戏的组帧配置：表面槽照 stg_slots.h，字库几何照 assets.h。STG_FIELD 入口也用它。 */
void     stg_modes_draw_cfg(stg_draw_cfg *c, int dst);

/* 整帧 640x480 的 CRC32（逐行取 2560 字节）。 */
uint32_t stg_modes_frame_crc(const uint32_t *fb);

/* 游戏状态整体重置（离开与进入模式 2、3 时调）。 */
void     stg_modes_reset(void);

/* 四个池（敌弹、自机弹、敌机、粒子）的存活数。 */
void     stg_modes_pools(uint32_t pool[4]);

/* 自查失败的累计次数（组帧被拒、普查对不上、执行出错、搜索结果不是 OK）。 */
uint32_t stg_modes_fail(void);

/* 模式 3 的两遍搜索都结束了返回 1（-DGFX2D_PRESS_EXIT=1 时框架据此结束，给检查脚本用）。 */
int      stg_modes_press_done(void);

/* 敌弹池的溢出计数（统计埋点的 overflow 列取它的增量）。 */
uint32_t stg_modes_overflow(void);

/* 日志行（nmax 的几种行、err 行）写进哪里：q 为 NULL 时直接 board_puts。 */
void     stg_modes_set_log(logq *q);

void     stg_mode_game_enter(void);
void     stg_mode_game_frame(const stg_main_ctx *ctx, int dst, uint32_t keys,
                             const stg_hud_live *live, stg_mode_out *o);

void     stg_mode_press_enter(void);
void     stg_mode_press_frame(const stg_main_ctx *ctx, int dst, const stg_hud_live *live,
                              stg_mode_out *o);

#endif /* STG_MODES_H */
