#include "renderer.h"
#include "board.h"
#include "gfx/pb.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
static unsigned char *ram;
board_mem board_region(board_region_id id)
{board_mem m={(void *)(ram+g_mem_regions[id].offset),g_mem_regions[id].size};return m;}
uint32_t board_cycles(void){static uint32_t n;return ++n;}
int main(void)
{
    ram=calloc(1,DDR_SIM_BYTES);assert(ram);
    gfx_surface_table t={0};gfx_list a,b,again,stale;
    gfx_surface dst={g_mem_regions[REGION_FB0].offset,640,480,12,0,0};
    assert(!gfx_surface_set(&t,0,&dst));
    gfx_cmd *pa=board_region(REGION_CMDLIST_A).ptr,*pc=board_region(REGION_CMDLIST_B).ptr;
    assert(!renderer_list_begin(3,&a,pa,32768,&t,0));assert(a.instances&&a.pb&&a.pb_config);stale=a;
    assert(a.pb->bytes==gfx_pb_bytes(a.pb_config));assert(a.pb->bytes==0x20400u);
    assert(renderer_list_begin(3,&again,pa,32768,&t,0)==GFX_BUILD_FULL);
    assert(!renderer_list_begin(3,&b,pc,32768,&t,0));assert(a.pb!=b.pb&&a.instances!=b.instances);
    assert(a.pb->dev_base+a.pb->bytes<=b.pb->dev_base);
    assert(a.instances->dev_base+a.instances->bytes<=a.pb->dev_base);
    assert(!gfx_list_end(&a));exec_sw_state es;exec_sw_reset(&es);exec_sw_error e;exec_sw_stats st;uint32_t cycles;
    gfx_mem m={0,ram,DDR_SIM_BYTES,0,0};
    assert(!renderer_run(0,&t,&m,&es,&e,&a,&st,&cycles));
    assert(!renderer_list_begin(3,&again,pa,32768,&t,0));assert(again.instance_generation!=stale.instance_generation);
    assert(!gfx_list_end(&stale));
    assert(renderer_run(0,&t,&m,&es,&e,&stale,&st,&cycles)==GFX_BUILD_BAD_ARG);
    assert(renderer_list_begin(3,&a,pa,32768,&t,0)==GFX_BUILD_FULL);
    assert(!gfx_list_end(&again));assert(!renderer_run(0,&t,&m,&es,&e,&again,&st,&cycles));
    assert(!renderer_list_begin(3,&a,pa,32768,&t,0));
    puts("F5 PB LEASE PASS independent_frames=2 busy_reuse=rejected stale_generation=rejected release_after_consumer=ok");
    free(ram);return 0;
}
