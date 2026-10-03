/******************************************************************************
* @file board.c: 板级接口的 Ti60F225 实现
*
* @brief  这一层是叠在只读的 `sdk/` 之上的项目板级层：sdk 是 SoC IP 的生成物，
*         重新生成 IP 时整个目录会被覆盖，所以项目要往 sdk 加的东西一律放这里，
*         由本层转出去（SW1 spec 2.1 节）。
*
*         SW1 提供 UART、CLINT、分区与缓存服务；SW9 补 GPIO/串口输入、
*         显示中断节拍与异步呈现，引擎驱动由 engine 系列提供。
*
*         这里 include 的 sdk 头文件在 mk/target.mk 里按系统头（-isystem）引入：
*         它们是上游代码，里面定义了用不到的 static 函数，
*         不该由它们把项目的「零告警」判据打断。
******************************************************************************/

#include <stdint.h>

#include "board.h"
#include "dcache_line.h"

#include "bsp.h"            /* sdk：uart_write、clint_getTimeLow、BSP_* 基址 */
#include "vexriscv.h"       /* sdk：data_cache_invalidate_all / _address */

/* 上游 spiFlash.h 里有两个 static 只有声明、没有定义
   （spiFlash_init_mode 与 spiFlash_enable_quad_access，它们的定义在
   `#if defined(MX25_FLASH)` / `GD25_FLASH` 分支里，本构型一个都不走），
   GCC 于是报 -Wunused-function「声明了 static 却从未定义」。
   这条诊断在 TU 结束时按当时的 pragma 状态发出，所以只包住 include 没用，
   必须对整个 TU 关掉。sdk 是只读的，不能去补它的头文件；
   而本文件的几个 static 都被用到，这一条豁免的代价可以接受。
   只关这一条，别的告警照旧当错误。 */
#pragma GCC diagnostic ignored "-Wunused-function"

#include "spiFlash.h"       /* sdk：spiFlash_init/wake/exit4ByteAddr/f2m */
#include "gpio.h"
#include "display_regs.h"

/* 素材分区用的 SPI 口与片选。取值与 bootloader 一致（bootloaderConfig.h），
   但在这里自己定义，不包含 bootloader 的配置头——那是它自己的东西。 */
#define ASSET_SPI     SYSTEM_SPI_0_IO_CTRL
#define ASSET_SPI_CS  0u

/* 数据缓存是 1 路直接映射、64 行 x 64 字节（cacheMechTest 的 T4 逐位实测），
   失效按行做，跨行的区间要逐行失效。 */
#define DCACHE_LINE_BYTES   64u

/* 素材分区的 SPI 读法。0 = 单线（与 bootloader 现状一致，`spiFlash_f2m`），
   1 = 双线（`spiFlash_f2m_dual`，板上 data_0/1 已接好）。
   默认取单线：它是当前唯一经过完整启动验证的路径；双线能不能用、值不值得用
   由 E6 的实测决定，那时只改这一个宏。 */
#ifndef BOARD_ASSET_SPI_DUAL
#define BOARD_ASSET_SPI_DUAL 0
#endif

void board_init(void)
{
    bsp_init();         /* UART 115200 8N1 */
}

board_mem board_region(board_region_id id)
{
    board_mem m;

    if (id >= REGION_COUNT) {
        m.ptr  = NULL;
        m.size = 0u;
        return m;
    }
    /* DDR 基址是 0：CPU 的 DDR 窗口从 0x0000_1000 起，而分区起点都 >= 0x0040_0000，
       且这个范围内 CPU 地址就等于控制器地址（E2 实测），所以分区表的偏移可以直接当
       CPU 地址用。memmap.h 里不写这个前提，它只属于本层。 */
    m.ptr  = (void *)(uintptr_t)g_mem_regions[id].offset;
    m.size = g_mem_regions[id].size;
    return m;
}

uint32_t board_cycles(void)
{
    return (uint32_t)clint_getTimeLow(BSP_CLINT);   /* 1 tick = 10ns */
}

/* --- 素材分区（sw 第 04 章 3.8 节）----------------------------------------------- */
/* 每次读都自己初始化一遍 SPI 与 Flash，不假设 bootloader 跑完之后
   外设还保持着它留下的状态。驱动的调用顺序与 bootloaderConfig.h 一致。 */
static void asset_spi_begin(void)
{
    spiFlash_init(ASSET_SPI, ASSET_SPI_CS);
    spiFlash_wake(ASSET_SPI, ASSET_SPI_CS);
    spiFlash_exit4ByteAddr(ASSET_SPI, ASSET_SPI_CS);
}

int board_asset_read(uint32_t offset, void *dst, uint32_t bytes)
{
    if (dst == NULL)
        return BOARD_ASSET_E_RANGE;
    if (bytes == 0u)
        return BOARD_ASSET_OK;
    /* 越界在这里拦掉：不拿一个算错的长度去读 Flash 或者写 DDR。 */
    if (offset > BOARD_ASSET_MAX_BYTES || bytes > BOARD_ASSET_MAX_BYTES - offset)
        return BOARD_ASSET_E_RANGE;

    asset_spi_begin();
#if BOARD_ASSET_SPI_DUAL
    spiFlash_f2m_dual(ASSET_SPI, ASSET_SPI_CS, BOARD_ASSET_FLASH_OFFSET + offset,
                      (uint32_t)(uintptr_t)dst, bytes);
#else
    spiFlash_f2m(ASSET_SPI, ASSET_SPI_CS, BOARD_ASSET_FLASH_OFFSET + offset,
                 (uint32_t)(uintptr_t)dst, bytes);
#endif
    return BOARD_ASSET_OK;
}

void board_puts(const char *s)
{
    while (*s != '\0')
        uart_write(BSP_UART_TERMINAL, *s++);
}

/* sdk 的 uart_writeAvailability() 读 UART 状态寄存器的 [23:16]，就是发送 FIFO 的空位数；
   写不超过这么多字节，上面的 uart_write() 就不会在忙等里停下（sw 第 07 章 3.3 节）。 */
uint32_t board_tx_room(void)
{
    return (uint32_t)uart_writeAvailability(BSP_UART_TERMINAL);
}

uint32_t board_keys(void)
{
    static uint32_t remote_held, last_valid;
    static uint8_t  header;
    static int      have_header, have_valid;
    const uint32_t now = board_cycles();
    const uint32_t gpio = (~gpio_getInput(SYSTEM_GPIO_1_IO_CTRL)) & 0xFu;
    uint32_t physical = 0u, rising = 0u;

    /* D3：只在确认非空后调用 sdk 的阻塞 uart_read；一帧内读空 FIFO。
       两包之间即使先按下再松开，rising 仍把按下沿交给这一帧。 */
    while (uart_readOccupancy(BSP_UART_TERMINAL) != 0u) {
        const uint8_t b = (uint8_t)uart_read(BSP_UART_TERMINAL);
        uint32_t key, check;

        if ((b & 0x80u) != 0u) {
            header = b;
            have_header = 1;
            continue;
        }
        if (!have_header)
            continue;
        have_header = 0;
        key = ((uint32_t)(header & 0x7Fu)) | (((uint32_t)b & 7u) << 7);
        check = (key ^ (key >> 4) ^ (key >> 8)) & 0xFu;
        if (((uint32_t)b >> 3) != check)
            continue;
        rising |= key & ~remote_held;
        remote_held = key;
        last_valid = now;
        have_valid = 1;
    }
    if (have_valid && (uint32_t)(now - last_valid) > BOARD_HZ * 3u / 10u) {
        remote_held = 0u;
        have_valid = 0;
    }
    if ((gpio & 1u) != 0u) physical |= BOARD_KEY_LEFT;
    if ((gpio & 2u) != 0u) physical |= BOARD_KEY_DOWN;
    if ((gpio & 4u) != 0u) physical |= BOARD_KEY_UP;
    if ((gpio & 8u) != 0u) physical |= BOARD_KEY_RIGHT;
    return physical | remote_held | rising;
}

uint32_t *board_fb(int index)
{
    if (index < 0 || index >= BOARD_FB_COUNT)
        return NULL;
    return (uint32_t *)(uintptr_t)g_mem_regions[REGION_FB0 + index].offset;
}

/* --- 扫描输出（display）------------------------------------------------- */
/* 寄存器表见 `rtl/display/display_fetch.v` 文件头与
   `../Engineering/Todo/hw/扫描输出.md` 2.2 节。窗口是 APB Slave 0 的 `0xE000~0xEFFF`。
   这些地址只出现在本文件：应用经 board_fb / board_present / board_wait_frame 用它，
   不碰寄存器。 */
/* 轮询上界。两件事都以场为节拍（60Hz，一场 16.7ms），给到 100ms 是六倍余量。
   不设上界的轮询在本仓踩过：比特流里没有扫描输出、或者取数没起来时，
   程序会停在这里不再输出，现象像死机而不是像失败。 */
#define DISP_POLL_LIMIT     (BOARD_HZ / 10u)

static uint32_t disp_rd(uint32_t addr)
{
    return *(volatile uint32_t *)(uintptr_t)addr;
}

static void disp_wr(uint32_t addr, uint32_t value)
{
    *(volatile uint32_t *)(uintptr_t)addr = value;
}

/* 比特流里有没有扫描输出。没有就退回空操作，老程序照常跑完。 */
static int disp_present_in_bitstream(void)
{
    return disp_rd(DISP_ID) == DISP_ID_VALUE;
}

static int s_disp_started;      /* 第一次 board_present 时打开取数并切到帧缓冲 */
static uint32_t s_present_event; /* 请求时的场起始中断序号 */
static uint32_t s_present_ticket;
static uint32_t s_present_field; /* 写 SWAP_REQ 之前读的 FRAME_COUNT */
static uint32_t s_present_retries;
static int s_present_pending;

/* SWAP_REQ 的写恰好落在场起始那一拍时，硬件让场起始的清零优先（display 第 01 章 4.1 节），
   这次请求被吞掉，SWAP_COUNT 再也不会变。场号先于请求读，正常的请求最迟在其后第二个
   场起始生效、并在该场排空复位时计数；到第三个场起始仍没换，只能是被吞了，重写一次。
   引擎忙时恰好约一场的负载会让请求成串贴着场起始写，这个窗口才真的会碰上。 */
static void present_retry_lost(void)
{
    if (disp_rd(DISP_FRAME_COUNT) - s_present_field < 3u)
        return;
    if (disp_rd(DISP_SWAP_COUNT) != s_present_ticket)
        return;
    s_present_field = disp_rd(DISP_FRAME_COUNT);
    disp_wr(DISP_SWAP_REQ, 1u);
    s_present_retries++;
}

int board_present_request(int index, uint32_t *ticket)
{
    uint32_t fb;
    uint32_t swaps;

    if (index < 0 || index >= BOARD_FB_COUNT || ticket == NULL || s_present_pending)
        return -1;
    if (!disp_present_in_bitstream())
        return -1;

    /* 写方向不需要先刷缓存：本构型没有写缓冲，CPU 的写返回时已经落到 DDR
       （platform 第 05 章 T3、T6）。要失效的是「读回外部主机写的东西」那个方向。 */
    fb = g_mem_regions[REGION_FB0 + index].offset;
    disp_wr(DISP_BACK_BASE, fb);

    if (!s_disp_started) {
        /* CLEAR 与使能一起写：core 复位不会复位像素域（它的复位来自 pll_locked），
           上一次会话留下的 SRC=1 会让像素侧在取数没开时空读一整场，
           把欠载计数灌满（见 display 条目 2026-09-21 的开发日志）。
           取数开始之前清一次，统计量才是本次运行的。 */
        disp_wr(DISP_CTRL, DISP_CTRL_EN | DISP_CTRL_SRC_FB | DISP_CTRL_CLEAR);
        s_disp_started = 1;
    }

    /* 切换在下一个场起始生效，一场的 480 行都来自同一个基址，所以不会撕裂。
       等它真的换过去再返回：返回之后调用方要画的是另一面，
       那一面必须已经不在扫描了。 */
    swaps = disp_rd(DISP_SWAP_COUNT);
    s_present_event = board_irq_frame_events();
    s_present_field = disp_rd(DISP_FRAME_COUNT);
    disp_wr(DISP_SWAP_REQ, 1u);
#ifdef INJECT_SW9_DROP_SWAP
    {
        /* 注错：每 64 次撤销一次，等同请求被场起始吞掉，验证 present_retry_lost */
        static uint32_t n;
        if ((n++ & 63u) == 0u)
            disp_wr(DISP_SWAP_REQ, 0u);
    }
#endif
    s_present_ticket = swaps;
    s_present_pending = 1;
    *ticket = swaps;
    return 0;
}

int board_present_finished(uint32_t ticket)
{
    int done;

    if (!disp_present_in_bitstream())
        return 0;
    if (s_present_pending && ticket == s_present_ticket)
        present_retry_lost();
    done = disp_rd(DISP_SWAP_COUNT) != ticket
           && (!board_irq_enabled() || board_irq_frame_events() != s_present_event);
    if (done && s_present_pending && ticket == s_present_ticket)
        s_present_pending = 0;
    return done;
}

int board_present_wait(uint32_t ticket)
{
    const uint32_t t0 = board_cycles();

    while (!board_present_finished(ticket)) {
        if ((uint32_t)(board_cycles() - t0) > DISP_POLL_LIMIT) {
            s_present_pending = 0;   /* 留着它，之后每次请求都会被拒收 */
            return -1;
        }
    }
    return 0;
}

uint32_t board_present_retries(void)
{
    return s_present_retries;
}

void board_present(int index)
{
    uint32_t ticket;

    if (board_present_request(index, &ticket) == 0)
        (void)board_present_wait(ticket);
}

void board_wait_frame(void)
{
    uint32_t f0;
    uint32_t t0;

    if (!disp_present_in_bitstream())
        return;

    /* FRAME_COUNT 数的是场起始，与 EN 无关：像素侧的时序发生器一直在跑，
       所以还没 present 过也能用它对齐到场节拍。 */
    f0 = disp_rd(DISP_FRAME_COUNT);
    t0 = board_cycles();
    if (board_irq_enabled()) {
        const uint32_t ev = board_irq_frame_events();
        while (board_irq_frame_events() == ev || disp_rd(DISP_FRAME_COUNT) == f0) {
            if ((uint32_t)(board_cycles() - t0) > DISP_POLL_LIMIT)
                return;
        }
        return;
    }
    while (disp_rd(DISP_FRAME_COUNT) == f0) {
        if ((board_cycles() - t0) > DISP_POLL_LIMIT)
            return;
    }
}

/* 两个计数直接读寄存器：它们是同一条通路的真实状态，本层不另做影子计数
   （做了反而会与硬件漂移）。比特流里没有扫描输出时读回来没有意义，恒返回 0，
   与 board_present 退回空操作是同一件事。 */
uint32_t board_frame_count(void)
{
    if (!disp_present_in_bitstream())
        return 0u;
    return disp_rd(DISP_FRAME_COUNT);
}

uint32_t board_swap_count(void)
{
    if (!disp_present_in_bitstream())
        return 0u;
    return disp_rd(DISP_SWAP_COUNT);
}

int board_display_diag(uint32_t *frame, uint32_t *fifo_min,
                       uint32_t *zero_hits, uint32_t *under_beats)
{
    if (!disp_present_in_bitstream() || frame == NULL || fifo_min == NULL ||
        zero_hits == NULL || under_beats == NULL)
        return -1;
    *frame = disp_rd(DISP_SNAP_FRAME);
    *fifo_min = disp_rd(DISP_SNAP_MIN);
    *zero_hits = disp_rd(DISP_ZERO_HITS);
    *under_beats = disp_rd(DISP_UNDER_BEATS);
    return 0;
}

/* 板上交互演示不限时；结束由复位、断电或调试器控制。 */
int board_should_quit(void)
{
    return 0;
}

/* 按 E3 规则包成带 "memory" clobber 的函数（platform 第 05 章）：
   sdk 的宏没有 clobber，紧邻的空 asm 屏障保证编译器不会把相邻的访存
   重排到失效之前——失效的作用是让后面的读真的去 DDR 取数。 */
void board_dcache_invalidate_all(void)
{
    data_cache_invalidate_all();
    __asm__ volatile ("" ::: "memory");
}

void board_dcache_invalidate_range(const void *addr, uint32_t bytes)
{
    dcache_span span;
    uint32_t    line;

    if (bytes == 0u)
        return;
    if (addr == NULL)
        return;         /* 指针为 0 说明调用方有 bug，这里不做「失效第 0 行」这种无意义的动作 */

    /* 折算见 dcache_line.h：起始向下对齐，结束取最后一个字节再向下对齐。
       按「从 addr 起每 64 字节失效一次」写会漏掉末尾那一行——
       addr 未对齐时那一行与起始行不是同一行。 */
    span = dcache_line_span((uint32_t)(uintptr_t)addr, bytes, DCACHE_LINE_BYTES);

    for (line = span.first; ; line += DCACHE_LINE_BYTES) {
        data_cache_invalidate_address((uintptr_t)line);
        if (line >= span.last)
            break;
    }

    __asm__ volatile ("" ::: "memory");
}
