/* 独立诊断构建：完成后将同一输入重定向到离屏影子，逐像素与 PERF 对账。 */
#ifdef FRONTEND_VERIFY
#include "board.h"
#include "gfx/gfx.h"
#include "gfx/frame.h"
#include "core/crc32.h"
#include <string.h>
static uint32_t scratch[32768];
static uint32_t face_crc(const gfx_mem *m,uint32_t base)
{
    crc32_ctx c;uint32_t y;crc32_begin(&c);
    for(y=0;y<480u;y++)crc32_update(&c,(const void *)((uintptr_t)m->host+base-m->dev_base+y*4096u),2560u);
    return crc32_end(&c);
}
int frontend_verify(const gfx_list *l,const gfx_mem *m,const exec_sw_stats *hw,
                    uint32_t *hw_crc,uint32_t *sw_crc)
{
    gfx_surface_table tab=*l->t;exec_sw_state es;exec_sw_stats expected={0};exec_sw_error er;
    uint32_t shadow=g_mem_regions[REGION_OFFSCREEN].offset+0x600000u;
    uint32_t dst=l->strip_table?l->strips.dst_id:0u,original=tab.s[dst].base;int rc;
    tab.s[dst].base=shadow;
    if(l->frame && l->frame->count==2u) {
        const gfx_list *pre=l->frame->pass[0].draw;
        exec_sw_reset(&es);
        rc=exec_sw_run(&es,&tab,m,pre->buf,pre->used,pre->used,&expected,&er);
        if(rc)return 1;
        {const exec_sw_stats *ph=&l->frame->pass[0].executed;
         if(ph->cmd!=expected.cmd||ph->pixel_total!=expected.pixel_total-expected.src_oob||
            ph->key_skip!=expected.key_skip||ph->pixel_written!=expected.pixel_written)return 5;}
    }
    if(l->strip_table)
        rc=exec_sw_run_strips(&tab,m,l->strip_table,GFX_STRIP_TABLE_BYTES,&l->strips,scratch,sizeof scratch,&expected,&er);
    else {exec_sw_reset(&es);rc=exec_sw_run(&es,&tab,m,l->buf,l->used,l->used,&expected,&er);}
    if(rc)return 2;
    board_dcache_invalidate_all();
    *hw_crc=face_crc(m,original);*sw_crc=face_crc(m,shadow);
    if(*hw_crc!=*sw_crc)return 3;
    {uint32_t y;for(y=0;y<480u;y++)
        if(memcmp((const void *)((uintptr_t)m->host+original-m->dev_base+y*4096u),
                  (const void *)((uintptr_t)m->host+shadow-m->dev_base+y*4096u),4096u))return 3;}
    if(hw->cmd!=expected.cmd||hw->pixel_total!=expected.pixel_total-expected.src_oob||
       hw->key_skip!=expected.key_skip||hw->pixel_written!=expected.pixel_written)return 4;
    return 0;
}
#endif
