/******************************************************************************
* @file board.c: 板级接口的 Ti60F225 实现
*
* @brief  这一层是叠在只读的 `sdk/` 之上的项目板级层：sdk 是 SoC IP 的生成物，
*         重新生成 IP 时整个目录会被覆盖，所以项目要往 sdk 加的东西一律放这里，
*         由本层转出去（SW1 spec 2.1 节）。
*
*         本条（SW1）只做最小可用：UART 日志、CLINT 计时、分区指针、缓存失效。
*         按键与显示在 SW9 补，引擎驱动在 engine 系列之后补；
*         在它们就绪之前，板上不上屏，画面以整帧 CRC 的形式从串口输出，
*         与 PC 逐帧比对（SW1 第 3 节）。
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

/* 数据缓存是 1 路直接映射、64 行 x 64 字节（cacheMechTest 的 T4 逐位实测），
   失效按行做，跨行的区间要逐行失效。 */
#define DCACHE_LINE_BYTES   64u

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

void board_puts(const char *s)
{
    while (*s != '\0')
        uart_write(BSP_UART_TERMINAL, *s++);
}

uint32_t board_keys(void)
{
    return 0u;          /* KEY0~3 接进 GPIO 之后由 SW9 补 */
}

uint32_t *board_fb(int index)
{
    if (index < 0 || index >= BOARD_FB_COUNT)
        return NULL;
    return (uint32_t *)(uintptr_t)g_mem_regions[REGION_FB0 + index].offset;
}

void board_present(int index)
{
    (void)index;        /* 扫描输出（display 系列）就绪之前不上屏 */
}

void board_wait_frame(void)
{
    /* 板上没有场同步可用之前不等：帧节拍由引擎的场同步中断给（display 系列）。
       这里的空实现让程序尽快跑完，把 CRC 打到串口上做逐帧比对。 */
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
