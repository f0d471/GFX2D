/******************************************************************************
* @file crc32.h: CRC32（与 zlib.crc32 同参）
*
* @brief  反射多项式 0xEDB88320，初值与结果都取反。与 `sw/` 下既有诊断程序
*         里那份逐位实现同参，所以 historical 的期望值（bootSizeTest 的
*         crc_all / crc_tail）可以直接对照。
*
*         这是纯算法，两个平台编同一份，不依赖任何平台头文件。
*         流式接口是为了整帧 CRC：帧缓冲行跨 4096 字节，像素只占每行前
*         2560 字节，要逐行 update，不能一次喂整块（间隙里的字节没写过）。
******************************************************************************/
#ifndef GFX2D_CORE_CRC32_H
#define GFX2D_CORE_CRC32_H

#include <stdint.h>

typedef struct {
    uint32_t c;         /* 运行中的寄存器，begin 之后才有效 */
} crc32_ctx;

/* 预生成 256 项查表。可以不调用：第一次 update/ieee 会自己补上。
   显式调用只是把这一步挪到启动阶段，避免第一帧多花一点时间。 */
void     crc32_init(void);

void     crc32_begin(crc32_ctx *ctx);
void     crc32_update(crc32_ctx *ctx, const void *data, uint32_t len);
uint32_t crc32_end(const crc32_ctx *ctx);

/* 一次算完一块连续内存，等价于 begin + update + end。 */
uint32_t crc32_ieee(const void *data, uint32_t len);

#endif /* GFX2D_CORE_CRC32_H */
