/* 测量基线已部署源码：独立编译单元消费有效记录，无 LTO。 */
#include "gfx/gfx.h"
#include "core/crc32.h"
uint32_t frontend_consume(const gfx_list *l)
{
    crc32_ctx c;
    uint32_t s;
    crc32_begin(&c);
    if (!l->strip_table) crc32_update(&c,l->buf,l->used*32u);
    else {
        const uint32_t *n=(const uint32_t *)(const void *)l->strip_table;
        crc32_update(&c,n,128u);
        for(s=0;s<l->strips.lstrips;s++)
            crc32_update(&c,l->strip_table+(s+1u)*GFX_STRIP_REGION_BYTES,n[s]*32u);
    }
    return crc32_end(&c);
}
