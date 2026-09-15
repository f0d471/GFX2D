/******************************************************************************
*
* @file main.c: bootSizeTest
*
* @brief  检查 bootloader 是否把整个应用镜像从 Flash 拷进了 DDR。
*         程序带一个约 300KB 的数据块（src/blob.S），镜像超过 124KB。
*         启动后对数据块整体和最后 4KB 各算一次 CRC32，经串口打印，
*         与 PC 上从 bootSizeTest.bin 算出的值比较。
*         官方 124KB bootloader 只拷前 124KB，数据块尾部是 DDR 里的旧内容，
*         两个 CRC 都应对不上；512KB bootloader 下两个 CRC 都应一致。
*
******************************************************************************/

#include <stdint.h>
#include "bsp.h"

#define APP_ORIGIN      0x00001000u
#define OLD_COPY_SIZE   0x0001F000u     /* 官方 bootloader 的 USER_SOFTWARE_SIZE */
#define TAIL_BYTES      4096u

extern const uint8_t blob_start[];
extern const uint8_t blob_end[];
extern char _edata[];

/* 与 zlib.crc32 相同：反射多项式 0xEDB88320，初值与结果都取反 */
static uint32_t crc32(const uint8_t *p, uint32_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

/* bsp_printf 的 %x 不补零，这里固定输出 8 位 */
static void hex8(char *buf, uint32_t v)
{
    static const char digits[] = "0123456789abcdef";
    for (int k = 7; k >= 0; k--) {
        buf[k] = digits[v & 0xF];
        v >>= 4;
    }
    buf[8] = '\0';
}

void main()
{
    uint32_t image = (uint32_t)_edata - APP_ORIGIN;
    uint32_t size = (uint32_t)(blob_end - blob_start);
    const uint8_t *tail = blob_end - TAIL_BYTES;
    char a[9], b[9], c[9], d[9];

    hex8(a, (uint32_t)blob_start);
    hex8(b, (uint32_t)tail);
    bsp_printf("bootSizeTest\r\n");
    bsp_printf("image %d bytes, old copy size %d bytes\r\n", image, OLD_COPY_SIZE);
    bsp_printf("blob 0x%s size %d, tail 0x%s\r\n", a, size, b);

    hex8(c, crc32(blob_start, size));
    hex8(d, crc32(tail, TAIL_BYTES));
    bsp_printf("crc_all 0x%s\r\n", c);
    bsp_printf("crc_tail 0x%s\r\n", d);
    bsp_printf("done\r\n");
}
