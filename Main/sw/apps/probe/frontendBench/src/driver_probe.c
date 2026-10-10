#ifdef FRONTEND_DRIVER_ONLY
#include "board.h"
#include "gfx/gfx.h"
#include "gfx_engine.h"
int frontend_driver_probe(void)
{
#ifdef __riscv
    gfx_list l;gfx_surface_table tab;uint32_t code=99,index=0,tag=0;
    board_mem cmd=board_region(REGION_CMDLIST_A);uint32_t base=g_mem_regions[REGION_FB0].offset;
    gfx_surface dst={base,640,480,12,0,0};int busy,waited,idle;
    if(gfx_engine_soft_reset() || gfx_engine_surf_set(0,base,640,480,12,0))return 1;
    gfx_surface_reset(&tab);gfx_surface_set(&tab,0,&dst);
    gfx_list_begin(&l,cmd.ptr,16,&tab);gfx_fill(&l,0,(gfx_rect){0,0,639,479},0x00112233,0,0);gfx_list_end(&l);
    if(gfx_engine_submit((uint32_t)(uintptr_t)cmd.ptr,l.used))return 1;
    busy=gfx_engine_surf_set(0,g_mem_regions[REGION_FB1].offset,640,480,12,0);
    waited=gfx_engine_wait(&code,&index,&tag);
    idle=gfx_engine_surf_set(0,base,640,480,12,0);
    board_puts(busy==GFX_ENG_E_REJECT?"driver busy SURF rejected ok\n":"driver busy SURF FAIL\n");
    board_puts(waited==0&&code==0?"driver complete and drained ok\n":"driver complete FAIL\n");
    board_puts(idle==0?"driver idle SURF restore ok\n":"driver idle SURF FAIL\n");
    return busy!=GFX_ENG_E_REJECT || waited || code || idle;
#else
    board_puts("driver probe needs hardware\n");return 1;
#endif
}
#endif
