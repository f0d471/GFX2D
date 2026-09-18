/******************************************************************************
* @file memmap.c: DDR3 分区表本体
*
* @brief  两个平台实现共用这一份表，见 memmap.h 与设计文档 2.1 节。
*         表里只有偏移与大小，没有平台相关的东西，所以它既不属于 bsp/ti60
*         也不属于 bsp/host。
******************************************************************************/

#include "memmap.h"

const mem_region g_mem_regions[REGION_COUNT] = {
    [REGION_CMDLIST_A] = { 0x00400000u, 0x00100000u },  /* 1MB，32768 条命令 */
    [REGION_CMDLIST_B] = { 0x00500000u, 0x00100000u },
    [REGION_SHARED]    = { 0x00600000u, 0x00A00000u },  /* 10MB */
    [REGION_ASSET]     = { 0x01000000u, 0x01000000u },  /* 16MB */
    [REGION_FB0]       = { 0x02000000u, 0x00400000u },  /* 4MB，实际用 1.875MB */
    [REGION_FB1]       = { 0x02400000u, 0x00400000u },
    [REGION_OFFSCREEN] = { 0x02800000u, 0x00800000u },  /* 8MB，末端 0x0300_0000 */
};
