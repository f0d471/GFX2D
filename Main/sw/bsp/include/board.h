/******************************************************************************
* @file board.h: 项目板级接口
*
* @brief  `sw/bsp/` 是叠在只读的 `sdk/` 之上的项目板级层，让同一份 `sw/lib/`
*         与应用代码在两个平台上编译：`bsp/ti60`（Ti60F225 板）与
*         `bsp/host`（WSL 里的 SDL2，PC 在这套结构里就是另一块板子）。
*
*         分层由各层的头文件路径保证，不靠约定（见 sw/mk/target.mk 与
*         sw/mk/host.mk）：编 `lib/` 时只有 `-Isw/lib`，看不到本文件；
*         编应用时只有 `lib` 与 `bsp/include`，看不到 sdk。所以应用里出现
*         `#include <soc.h>` 或直接调 sdk 驱动会编不过。
*
*         这里只放与硬件有关、两边都能实现的服务。应用层的事不进这里：
*         素材按编号查找是 `lib/core` 解析素材包，录像一段按键缓冲是
*         `lib/core` 处理，文件读写才归 `bsp/host`。
*
*         接口的取舍见 Engineering/Todo/sw/SW1-软件平台分层与双目标构建.md 2.2 节。
******************************************************************************/
#ifndef GFX2D_BOARD_H
#define GFX2D_BOARD_H

#include <stddef.h>
#include <stdint.h>
#include "memmap.h"

/* 板级帧节拍与计时基准：core_clk = 100MHz，1 个 cycle = 10ns。
   PC 上按 SDL_GetPerformanceCounter 折算成同一单位，只作显示，
   不作性能结论（SW1-SW9 总览约束 5）。 */
#define BOARD_HZ            100000000u

#define BOARD_FB_COUNT      2

typedef struct {
    void    *ptr;       /* 区域起始指针，两个平台都按同一偏移取 */
    uint32_t size;      /* 区域字节数，取自 memmap.h 的分区表 */
} board_mem;

/* --- 初始化 ------------------------------------------------------------- */
/* 板上：UART、CLINT；PC：SDL 初始化并分配模拟 DDR 分区。 */
void       board_init(void);

/* --- 内存区 ------------------------------------------------------------- */
/* 应用只经这里拿到 DDR 分区，不出现物理地址，PC 才能用一块内存模拟。 */
board_mem  board_region(board_region_id id);

/* --- 计时 --------------------------------------------------------------- */
/* 返回自 board_init 起累加的 CPU 周期数，按 **32 位回绕**：板上是 CLINT 的低 32 位，
   PC 上是性能计数器折算后同样截成 32 位。100MHz 下 2^32 个周期 = 42.9 秒，
   两个平台的回绕行为一致，判据才能在两边通用。

   用法是取两次读数相减得到区间长度，相减在模 2^32 下成立，
   所以一次区间必须在 42.9 秒以内；要统计更长的总时长，
   逐段累加区间（例如每帧累加），不要拿两个相距很远的读数相减。 */
uint32_t   board_cycles(void);

/* --- 日志 --------------------------------------------------------------- */
/* 走 UART（板上）或 stdout（PC），两边逐字节相同。应用里不出现 printf。 */
void       board_puts(const char *s);

/* --- 输入 --------------------------------------------------------------- */
/* 返回本帧的按键位图。位分配是两个实现必须一致的约定：板上 KEY0~3 与串口
   （SW9 接），PC 上是 SDL 键盘。本条（SW1）板上恒为 0。 */
#define BOARD_KEY_UP        (1u << 0)
#define BOARD_KEY_DOWN      (1u << 1)
#define BOARD_KEY_LEFT      (1u << 2)
#define BOARD_KEY_RIGHT     (1u << 3)
#define BOARD_KEY_BOMB      (1u << 4)   /* PC: 空格 */
#define BOARD_KEY_SLOW      (1u << 5)   /* PC: 左 Shift */
#define BOARD_KEY_SWAP      (1u << 6)   /* 软硬件渲染对照，PC: Tab */
#define BOARD_KEY_MODE1     (1u << 7)
#define BOARD_KEY_MODE2     (1u << 8)
#define BOARD_KEY_MODE3     (1u << 9)

uint32_t   board_keys(void);

/* --- 显示 --------------------------------------------------------------- */
/* 返回帧缓冲 index（0/1）的像素指针；行跨固定 4096 字节，用 FB_ROW_PIXELS
   换算行首。本条板上只返回指针不上屏（等 display 系列），PC 上开窗显示。 */
uint32_t  *board_fb(int index);

/* 呈现一帧。本条板上是空操作，PC 上上传纹理并刷新窗口。 */
void       board_present(int index);

/* 等下一个场同步。本条板上是空操作，PC 上按 60Hz 节拍。 */
void       board_wait_frame(void);

/* --- 缓存 --------------------------------------------------------------- */
/* 按 E3 规则包成带 "memory" clobber 的函数：CPU 数据缓存会遮蔽外部主机
   （引擎、扫描输出）写入 DDR 的结果，共享区读回之前必须失效一次。
   见 Engineering/TechReports/platform/05-CPU数据缓存与第二个主机写入的一致性.md。
   PC 上都是空操作。 */
void       board_dcache_invalidate_all(void);
void       board_dcache_invalidate_range(const void *addr, uint32_t bytes);

#endif /* GFX2D_BOARD_H */
