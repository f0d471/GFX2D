/******************************************************************************
* @file ref_crc.h: CRC32 的参考实现（只给主机侧测试用）
*
* @brief  `lib/core/crc32.c` 是查表版，这里再写一份不追求速度、逐位照定义抄的
*         实现，两者必须逐字节相等。教训：只有一个实现时，模型写错了也看不出来。
*
*         本文件只在 `sw/lib/test/` 里使用，不进 lib 的库列表。
******************************************************************************/
#ifndef GFX2D_TEST_REF_CRC_H
#define GFX2D_TEST_REF_CRC_H

#include <stdint.h>

/* 逐位实现：反射多项式 0xEDB88320，初值与结果都取反。 */
uint32_t ref_crc32_bitwise(const void *data, uint32_t len);

/* 把 rows 段、每段 row_bytes 字节、段间隔 stride_bytes 的内容先拼成一块连续
   内存，再算 CRC。这是「整帧 CRC」的定义本身：没写过的间隙不参与。 */
uint32_t ref_crc32_strided(const uint8_t *base, uint32_t rows,
                           uint32_t stride_bytes, uint32_t row_bytes);

#endif /* GFX2D_TEST_REF_CRC_H */
