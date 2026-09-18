/******************************************************************************
* @file crc32.c: CRC32（与 zlib.crc32 同参）
*
* @brief  逐位算法与 `sw/bootSizeTest/src/main.c` 里那份一致，这里是查表版，
*         行为必须相同；相等由 `sw/lib/test/` 的独立逐位实现逐字节比对。
*
*         表在第一次使用时生成，不写成 `const` 表放 `.rodata`：
*         这样 1KB 的表不占镜像——它落在 `.bss` 里，由 `start.S` 清零，
*         不参与 bootloader 从 Flash 的拷贝；代价是首次使用时约两千个周期的生成。
*         生成本身是确定性的，两个平台得到同一张表。
*         （板上没有片上 ROM 可用，表最终都在 DDR 里，省下的是镜像拷贝量。
*         原先这里写的是「比占片上 RAM 划算」，理由不成立，已订正。）
******************************************************************************/

#include "core/crc32.h"

#define CRC32_POLY      0xEDB88320u
#define CRC32_INIT      0xFFFFFFFFu

static uint32_t s_table[256];
static int      s_table_ready;

void crc32_init(void)
{
    for (uint32_t i = 0; i < 256u; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (CRC32_POLY & (0u - (c & 1u)));
        s_table[i] = c;
    }
    s_table_ready = 1;
}

void crc32_begin(crc32_ctx *ctx)
{
    if (!s_table_ready)
        crc32_init();
    ctx->c = CRC32_INIT;
}

void crc32_update(crc32_ctx *ctx, const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t c = ctx->c;

    if (!s_table_ready)
        crc32_init();

    while (len--) {
        c = s_table[(c ^ *p) & 0xFFu] ^ (c >> 8);
        p++;
    }
    ctx->c = c;
}

uint32_t crc32_end(const crc32_ctx *ctx)
{
    return ~ctx->c;
}

uint32_t crc32_ieee(const void *data, uint32_t len)
{
    crc32_ctx ctx;

    crc32_begin(&ctx);
    crc32_update(&ctx, data, len);
    return crc32_end(&ctx);
}
