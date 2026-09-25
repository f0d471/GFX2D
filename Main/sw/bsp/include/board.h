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

/* 现在还能不阻塞地交给 board_puts() 多少字节（SW8 2.4 节）。
   板上的 board_puts() 忙等发送 FIFO，115200 波特每帧只发得出约 192 字节，一次写多了会卡住当帧；
   应用把日志先放进 lib/core/logq，每帧只取这么多字节写出去。

   板上：发送 FIFO 的空位数（UART 状态寄存器的 [23:16]）。
   PC：默认返回 BOARD_TX_ROOM_UNLIMITED。设了环境变量 GFX2D_TX_BUDGET=N（1~65535）时模拟串口，
       **按场计、不按墙钟**：board_frame_count() 一变就把额度补满到 N（不累加，FIFO 的容量是固定的），
       board_puts() 从额度里扣。超出额度的字节照样打出来（PC 上不丢日志），另计超额次数与字节数，
       进程退出时在 stderr 打一行汇总。 */
#define BOARD_TX_ROOM_UNLIMITED 0x7FFFFFFFu

uint32_t   board_tx_room(void);

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
   换算行首。两个平台都只返回指针，画完要 board_present 才上屏。 */
uint32_t  *board_fb(int index);

/* 呈现一帧：把 index 这一面设成扫描输出的目标并请求切换，**等切换完成后返回**。
   返回之后另一面已经不在扫描，调用方可以安全地往它画下一帧。
   板上走扫描输出的 BACK_BASE/SWAP_REQ（切换在场起始生效，所以不会撕裂），
   PC 上上传纹理并刷新窗口。
   比特流里没有扫描输出时板上退回空操作，老程序照常跑完。 */
void       board_present(int index);

/* 等下一个场同步。板上轮询扫描输出的 FRAME_COUNT（与 EN 无关，没 present 过也能用），
   PC 上等到下一个 60Hz 场边界。两边都有轮询上界，不会停在这里不返回。
   board_should_quit() 置位时立即返回。 */
void       board_wait_frame(void);

/* --- 帧节拍的两个计数 --------------------------------------------------- */
/* 两个平台都真实存在的两个量，掉帧数由应用做减法：Δ场 − Δ切换。

   都按 32 位回绕，与 board_cycles 同一约定：取两次读数相减。
   **减法要先用 uint32_t 减完，再往更宽的类型上累加**：

       uint32_t d = (uint32_t)(f1 - f0) - (uint32_t)(s1 - s0);   // 这一段错过的场数
       total += d;                                              // 累加放宽类型里

   先提升再减会把一次回绕放大成天文数字（SW8 要按秒累加掉帧数，踩到一次整列作废）。

   板上：FRAME_COUNT 由像素域时序发生器在每一个场起始加一，与 EN 无关、任何时刻可读；
         SWAP_COUNT 在完成的切换上加一。比特流里没有扫描输出时两个都恒为 0。
   PC：场数由 board_init 起算的 60Hz 时间轴给（同样与有没有 present 无关），
       切换数是成功 present 过的次数。

   口径与判据见 Engineering/TechReports/sw/03-PC平台层与帧节拍.md 3.4 节。 */
uint32_t   board_frame_count(void);
uint32_t   board_swap_count(void);

/* 宿主要求结束（PC：窗口关闭按钮；板上恒为 0）。
   置位之后 board_present 与 board_wait_frame 都立即返回，不再等场边界，
   否则关窗之后每一帧照样各等掉一场。退出不是游戏输入，**不进按键位图**：
   进了位图就会被录进录像，回放时凭空多出一个按键，破坏确定性。 */
int        board_should_quit(void);

/* --- 素材包装载 --------------------------------------------------------- */
/* Flash 素材分区（设计文档 2.2 节）：起点 0x400000、物理上限 4 MiB。
   这一组常量属于板级层：lib/core/assetpak 不认识分区，
   它只接受调用方算好的 assetpak_limits。 */
#define BOARD_ASSET_FLASH_OFFSET 0x00400000u
#define BOARD_ASSET_MAX_BYTES    (4u * 1024u * 1024u)

#define BOARD_ASSET_OK           0
#define BOARD_ASSET_E_RANGE     (-1)    /* 请求越出素材分区容量 */
#define BOARD_ASSET_E_IO        (-2)    /* 读失败（文件打不开、SPI 无响应） */
#define BOARD_ASSET_E_PATH      (-3)    /* PC：路径取不到 */

/* 从素材分区读 bytes 个字节到 dst。offset **相对分区起点**，不是 Flash 物理地址：
   板上把它加到 BOARD_ASSET_FLASH_OFFSET 上，PC 上就是 assets.pak 的文件偏移。
   板上走 SPI Flash（单线/双线由本层选），PC 走 GFX2D_ASSETS_PAK 指的文件。
   返回 0 或上面的负错误码。

   本函数只管搬运，**不解析**：素材包的结构与校验规则在 lib/core/assetpak 里，
   由 assetpak_load() 调用它。这样 bsp 不依赖 lib、lib 不依赖 bsp，
   仍然只有应用把两者接起来（SW1 2.1 节的分层）。 */
int board_asset_read(uint32_t offset, void *dst, uint32_t bytes);

/* --- 缓存 --------------------------------------------------------------- */
/* 按 E3 规则包成带 "memory" clobber 的函数：CPU 数据缓存会遮蔽外部主机
   （引擎、扫描输出）写入 DDR 的结果，共享区读回之前必须失效一次。
   见 Engineering/TechReports/platform/05-CPU数据缓存与第二个主机写入的一致性.md。
   PC 上都是空操作。 */
void       board_dcache_invalidate_all(void);
void       board_dcache_invalidate_range(const void *addr, uint32_t bytes);

#endif /* GFX2D_BOARD_H */
