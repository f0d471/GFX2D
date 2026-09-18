/******************************************************************************
* @file ref_crc.c: CRC32 的参考实现
*
* @brief  见 ref_crc.h。
******************************************************************************/

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

    if (rows * row_bytes > (uint32_t)sizeof flat)
        return 0u;      /* 测试用的规模远小于这个上限，超了说明参数写错 */

    for (uint32_t y = 0; y < rows; y++) {
        const uint8_t *row = base + (uint32_t)y * stride_bytes;
        for (uint32_t k = 0; k < row_bytes; k++)
            flat[n++] = row[k];
    }
    return ref_crc32_bitwise(flat, n);
}
