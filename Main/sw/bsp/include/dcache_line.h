/******************************************************************************
* @file dcache_line.h: 把字节区间折算成缓存行区间（bsp 内部辅助）
*
* @brief  `board_dcache_invalidate_range` 要失效 [addr, addr+bytes) 触及的**每一行**。
*         起始地址没有按行对齐时，末尾那一行最容易被漏掉：
*         addr = 0x3F、bytes = 2 只触及 0x3F 与 0x40 两个字节，
*         却分别落在 0x00 与 0x40 两行里；按「从 addr 起每 64 字节失效一次」的写法
*         只会失效 0x00 那一行，0x40 那一行漏掉，调用方之后会读到旧数据。
*
*         正确的折算分两步：起始地址向下对齐到行首，
*         结束地址取**最后一个字节**（addr + bytes − 1）再向下对齐。
*
*         这段算术放在 bsp/include 而不是 board.c 里，是为了让它可判：
*         `lib/` 不能看 `bsp/`，`bsp/ti60/board.c` 又要 sdk 头文件、编不了主机，
*         只有放在两个实现都能包含的头里，`Main/tools/check/check_sw_layers.sh`
*         才能在主机上拿**真实的这一段代码**跑算例（见 TechReports sw 第 01 章 4.9）。
*
*         行宽由调用方给出，必须是 2 的幂（本构型的实测值是 64 字节，
*         来自 cacheMechTest 的 T4）。
******************************************************************************/
#ifndef GFX2D_DCACHE_LINE_H
#define GFX2D_DCACHE_LINE_H

#include <stdint.h>

typedef struct {
    uint32_t first;     /* 区间第一行的行首地址 */
    uint32_t last;      /* 区间最后一行的行首地址（闭区间，含这一行） */
} dcache_span;

/* 前提：bytes >= 1；且 [addr, addr+bytes) 不跨越 2^32
   （本构型的 DDR 分区都在 0x0300_0000 以下，远低于这个界限）。 */
static inline dcache_span dcache_line_span(uint32_t addr, uint32_t bytes, uint32_t line_bytes)
{
    const uint32_t mask = ~(line_bytes - 1u);
    dcache_span    s;

    s.first = addr & mask;
    s.last  = (addr + bytes - 1u) & mask;
    return s;
}

#endif /* GFX2D_DCACHE_LINE_H */
