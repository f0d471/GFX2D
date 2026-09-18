/******************************************************************************
* @file memmap.h: DDR3 分区表
*
* @brief  设计文档《赛题二总体设计》2.1 节的分区规划，只放偏移与大小，
*         **不含任何平台指针**：板上由 bsp/ti60 加 DDR 基址得到 CPU 地址，
*         PC 上由 bsp/host 指向模拟内存块里的同一偏移。
*
*         偏移以 DDR 颗粒地址 0 为基准。分区起点都 >= 0x0040_0000，
*         而 CPU 的 DDR 窗口从 0x0000_1000 起，且 0x0000_1000~0x0FFF_FFFF 内
*         CPU 地址就等于控制器地址（实验 E2 实测，platform 第 02 章），
*         所以这些偏移在板上可以直接当 CPU 地址用。
*
*         程序区（0x0000_1000~0x003F_FFFF）不在这里：它由链接脚本管，
*         应用只经 board_region 拿数据区。
******************************************************************************/
#ifndef GFX2D_MEMMAP_H
#define GFX2D_MEMMAP_H

#include <stdint.h>

typedef enum {
    REGION_CMDLIST_A = 0,   /* 命令表 A，1MB，最多 32768 条 32 字节命令 */
    REGION_CMDLIST_B,       /* 命令表 B，同上；A 填帧 N 时引擎执行 B 的帧 N-1 */
    REGION_SHARED,          /* 共享数据池，10MB，CPU 与引擎都访问 */
    REGION_ASSET,           /* 素材区，16MB，纹理图集、背景、字库 */
    REGION_FB0,             /* 帧缓冲 0，分区 4MB（实际用 480 x 4096 字节） */
    REGION_FB1,             /* 帧缓冲 1，同上 */
    REGION_OFFSCREEN,       /* 离屏层，8MB，背景预合成与调试抓帧 */
    REGION_COUNT
} board_region_id;

typedef struct {
    uint32_t offset;
    uint32_t size;
} mem_region;

/* 分区表本体在 bsp/memmap.c，两个平台实现共用一份，避免两处写两遍。 */
extern const mem_region g_mem_regions[REGION_COUNT];

#define DDR_TOTAL_BYTES     (256u * 1024u * 1024u)  /* 板上颗粒 256MB */
#define DDR_SIM_BYTES       (64u * 1024u * 1024u)   /* bsp/host 模拟块，覆盖到离屏层末端 */

/* 帧缓冲的行跨取 4096 字节：一行的读写正好是一次突发，永远不会跨 4KB 边界。
   地址计算只剩移位：addr = base + (y << 12) + (x << 2)。 */
#define FB_WIDTH            640u
#define FB_HEIGHT           480u
#define FB_STRIDE_LOG2      12u
#define FB_STRIDE_BYTES     (1u << FB_STRIDE_LOG2)
#define FB_BYTES            (FB_HEIGHT * FB_STRIDE_BYTES)   /* 1,966,080 字节 */
#define FB_ROW_PIXELS       (FB_STRIDE_BYTES / 4u)          /* 一行 1024 个像素位 */

#endif /* GFX2D_MEMMAP_H */
