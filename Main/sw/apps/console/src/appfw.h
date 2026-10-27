/******************************************************************************
* @file appfw.h: 游戏机应用框架（SW10）
*
* @brief  「应用」= 可从菜单启动的一等内容（游戏、三维演示、系统信息……）。
*         契约与裸机模式框架（gfx2dGame/main.c 的 mode_enter/frame/leave）同构：
*           enter  登记表面/装载资源（0 成功）
*           frame  一场一帧：组帧+执行+呈现全部自己做（present_wait 即节拍）
*           leave  收尾（引擎排空、状态重置）
*         扩展 = appfw_apps 注册表加一项 + appfw_request 启动。
*
*         系统键位（应用不该占用）：
*           MODE1|MODE2|MODE3 三键同按 8 帧 = HOME：退回菜单（runner 检测，
*           应用拿到的 keys 原样含这些位，但自行处理会与 HOME 冲突）。
*         菜单键位（只在 shell 内）：上下移动、BOMB/MODE2 启动、数字键直达。
******************************************************************************/
#ifndef APPFW_H
#define APPFW_H

#include <stdint.h>

#include "core/logq.h"
#include "gfx/gfx.h"

#include "stg_runtime.h"

typedef struct appfw_ctx {
    stg_runtime *rt;        /* 共享运行时（boot 后有效） */
    uint32_t     seq;       /* 全局帧序号：目的面与命令表按奇偶轮换，跨应用连续 */
    int          shown;     /* 正在扫描的那一面；-1 未呈现过 */
    logq        *log;       /* 应用日志队列，runner 每帧按串口额度泵出 */
} appfw_ctx;

typedef struct appfw_app {
    const char *name;       /* ASCII 短名（判据行与菜单用） */
    const char *desc;       /* 菜单里的一行说明 */
    int  (*enter)(appfw_ctx *c);
    void (*leave)(appfw_ctx *c);
    void (*frame)(appfw_ctx *c, uint32_t keys, uint32_t app_frame);
} appfw_app;

/* 注册表：apps_registry.c 以链接期数组登记。count 出条数。 */
const appfw_app *const *appfw_apps(uint32_t *count);
const appfw_app        *appfw_app_at(uint32_t index);
const appfw_app        *appfw_find(const char *name);

/* 请求切到某应用（下一帧边界生效）；NULL 取消请求。 */
void                    appfw_request(const appfw_app *app);

/* 当前是不是菜单（monitor 只在安静时说话）。 */
int                     appfw_in_shell(void);

/* 一帧的合并按键（held|rising，语义同裸机每帧 board_keys() 的返回值）。 */
uint32_t                appfw_input_take(void);

/* runner 循环的忙耗时累计（取走并清零），供应用每秒统计行做
   「迭代周期 vs 忙耗时」对照：差值 = runner 被阻塞/抢占的时间。 */
void                    appfw_loop_cycles(uint32_t *sum, uint32_t *n);

/* 最近一次采样的按住位图（电平语义，不消耗按下沿）；
   供 r4_set_key_source 注入，替代 r4 在框架外直读 board_keys。 */
uint32_t                appfw_input_held(void);

/* 一行进日志队列（runner 泵出；boot 阶段的直打用 board_puts）。 */
void                    appfw_log(const char *s);

/* HOME 和弦事件位 */
#define APPFW_EV_HOME   (1u << 0)

/* 入口（main 调）：初始化并创建三个任务；之后 main 装 trap、起调度器。 */
void                    appfw_start(void);

#endif /* APPFW_H */
