#include "board.h"
#include "gfx_engine.h"
#include "bsp.h"

static uint32_t rd(uint32_t off)
{return *(volatile uint32_t *)(uintptr_t)(IO_APB_SLAVE_0_INPUT+off);}
static void wr(uint32_t off,uint32_t value)
{*(volatile uint32_t *)(uintptr_t)(IO_APB_SLAVE_0_INPUT+off)=value;}
static int inside(uint32_t base,uint32_t bytes,board_region_id id)
{
    const mem_region *r=&g_mem_regions[id];
    return base>=r->offset && (uint64_t)base+bytes<=(uint64_t)r->offset+r->size;
}
static int overlaps(uint32_t a,uint64_t an,uint32_t b,uint64_t bn)
{return (uint64_t)a+an>b && (uint64_t)b+bn>a;}

int gfx_engine_raster_info(uint32_t *id,uint32_t *caps,uint32_t *cycles,uint32_t *refs)
{
    if(!gfx_engine_present())return GFX_ENG_E_ABSENT;
    if(!(rd(GFX_CAPS)&GFX_CAPS_RAST_M))return GFX_ENG_E_CAPS;
    if(!id || !caps || !cycles || !refs)return GFX_ENG_E_ARG;
    *id=rd(GFX_R3_ID);*caps=rd(GFX_R3_CAPS);
    *cycles=rd(GFX_R3_PERF_CYCLES);*refs=rd(GFX_R3_PERF_RETIRED);
    return GFX_ENG_OK;
}

int gfx_engine_submit_raster(uint32_t list,uint32_t bg,uint32_t dst,
                             const gfx_engine_raster *p)
{
    uint32_t surface[2],span[2];
    if(!gfx_engine_present())return GFX_ENG_E_ABSENT;
    if(!(rd(GFX_CAPS)&GFX_CAPS_RAST_M))return GFX_ENG_E_CAPS;
    if(!p || !p->tri_count || p->tri_count>65535u || bg>15u || dst>15u ||
       p->sub_bytes<120u || (p->sub_bytes&1u) || p->sub_bytes>0x00fffffeu || p->texcfg)
        return GFX_ENG_E_ARG;
    if((list&32767u)||(p->tri_base&31u)||(p->sub_base&15u))return GFX_ENG_E_ALIGN;
    uint32_t envelope=(p->sub_bytes+15u)&~15u;
    if(p->tri_alloc<(uint64_t)p->tri_count*128u || p->sub_alloc<envelope ||
       !inside(p->tri_base,p->tri_alloc,REGION_SHARED) ||
       !inside(p->sub_base,p->sub_alloc,REGION_SHARED) ||
       overlaps(p->tri_base,p->tri_alloc,p->sub_base,p->sub_alloc) ||
       !(inside(list,31u*32768u,REGION_CMDLIST_A)||inside(list,31u*32768u,REGION_CMDLIST_B)))
        return GFX_ENG_E_ARG;
    if(rd(GFX_STATUS)&(GFX_STATUS_BUSY|GFX_STATUS_DRAINING))return GFX_ENG_E_REJECT;
    for(unsigned i=0;i<2;++i) {
        unsigned slot=i ? dst:bg;
        uint32_t size=rd(GFX_SURF(slot,1)),control=rd(GFX_SURF(slot,2));
        surface[i]=rd(GFX_SURF(slot,0));
        if(size!=((480u<<16)|640u) || (control&0x8000f300u)!=0x80000000u ||
           (control&31u)<12u || (control&31u)>18u || (surface[i]&15u))return GFX_ENG_E_ARG;
        uint64_t end=(uint64_t)479u*(1u<<(control&31u))+2560u;
        if((uint64_t)surface[i]+end>DDR_TOTAL_BYTES)return GFX_ENG_E_ARG;
        span[i]=(uint32_t)end;
        if(overlaps(surface[i],span[i],list,31u*32768u) ||
           overlaps(surface[i],span[i],p->tri_base,p->tri_alloc) ||
           overlaps(surface[i],span[i],p->sub_base,p->sub_alloc))return GFX_ENG_E_ARG;
    }
    if(overlaps(surface[0],span[0],surface[1],span[1]))return GFX_ENG_E_ARG;
    wr(GFX_R3_TRI_BASE,p->tri_base);wr(GFX_R3_SUB_BASE,p->sub_base);
    wr(GFX_R3_SUB_BYTES,p->sub_bytes);wr(GFX_R3_TRI_COUNT,p->tri_count);
    wr(GFX_R3_RAS_KEY,p->ras_key);wr(GFX_R3_TEXCFG,p->texcfg);
    wr(GFX_LSTRIPS,30u);wr(GFX_STRIP_H,4u);wr(GFX_BAND_SURF,(bg<<4)|dst);
    wr(GFX_CTRL,GFX_CTRL_EN|GFX_CTRL_BAND);wr(GFX_LIST_BASE,list);wr(GFX_LIST_COUNT,0u);
    __asm__ volatile("fence rw,rw" ::: "memory");
    wr(GFX_DOORBELL,1u);
    return GFX_ENG_OK;
}
