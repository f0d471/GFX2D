/* 独立编译单元消费全部有效记录的八个字段；没有 LTO，不进入计时区。 */
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
#ifndef __riscv
#include <stdio.h>
#include <stdlib.h>
void frontend_dump(uint32_t scene,uint32_t mode,const gfx_list *l)
{
    const char *dir=getenv("FRONTEND_INPUT_DIR");char path[1024];FILE *f;uint32_t s;
    if(!dir||mode>1u)return;
    snprintf(path,sizeof path,"%s/scene%02u_mode%u.bin",dir,scene,mode);f=fopen(path,"wb");
    if(!f)exit(3);
    fwrite(l->t,sizeof *l->t,1,f);fwrite(&l->used,4,1,f);
    if(!mode)fwrite(l->buf,32,l->used,f);
    else {
        const uint32_t *n=(const uint32_t *)(const void *)l->strip_table;
        fwrite(n,4,32,f);
        for(s=0;s<l->strips.lstrips;s++)fwrite(l->strip_table+(s+1u)*32768u,32,n[s],f);
    }
    if(fclose(f))exit(4);
}
#endif
