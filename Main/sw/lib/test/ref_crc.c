/******************************************************************************
* @file ref_crc.c: CRC32 的参考实现
*
* @brief  见 ref_crc.h。
******************************************************************************/

#include <stdlib.h>

#include "ref_crc.h"

#define REF_POLY    0xEDB88320u

uint32_t ref_crc32_bitwise(const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t       c = 0xFFFFFFFFu;

    while (len--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (REF_POLY & (0u - (c & 1u)));
    }
    return ~c;
}

uint32_t ref_crc32_strided(const uint8_t *base, uint32_t rows,
                           uint32_t stride_bytes, uint32_t row_bytes)
{
    static uint8_t flat[8192];
    uint32_t       n = 0u;

    /* 超限直接停住，不返回一个看着像结果的值。
       原先这里 `return 0u`，SW3 差一点拿它算 640x480 的整帧 CRC：
       1228800 字节远超上限，每个用例都会得到 0，注错之后仍然是 0，
       判据全绿而其实什么也没验。0 是合法的 CRC 取值，分辨不出来。
       整帧那种规模要用 lib/core/crc32.h 的流式接口逐行 update。 */
    if (rows * row_bytes > (uint32_t)sizeof flat)
        abort();

    for (uint32_t y = 0; y < rows; y++) {
        const uint8_t *row = base + (uint32_t)y * stride_bytes;
        for (uint32_t k = 0; k < row_bytes; k++)
            flat[n++] = row[k];
    }
    return ref_crc32_bitwise(flat, n);
}
