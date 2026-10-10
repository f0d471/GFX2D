#include "renderer.h"
#include "board.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
static unsigned char *ram;
board_mem board_region(board_region_id id)
{board_mem m={(void *)(ram+g_mem_regions[id].offset),g_mem_regions[id].size};return m;}
uint32_t board_cycles(void){static uint32_t n;return ++n;}
int main(void)
{
    ram=calloc(1,DDR_SIM_BYTES);assert(ram);gfx_surface_table t={0};gfx_list a,b,again,stale;
    gfx_cmd *pa=board_region(REGION_CMDLIST_A).ptr,*pb=board_region(REGION_CMDLIST_B).ptr;
    assert(!renderer_list_begin(1,&a,pa,32768,&t,0));assert(a.instances);stale=a;
    assert(renderer_list_begin(1,&again,pa,32768,&t,0)==GFX_BUILD_FULL);
    assert(!renderer_list_begin(1,&b,pb,32768,&t,0));assert(a.instances!=b.instances);
    assert(!gfx_list_end(&a));exec_sw_state es;exec_sw_reset(&es);exec_sw_error e;exec_sw_stats st;uint32_t cycles;
    gfx_mem m={0,ram,DDR_SIM_BYTES,0,0};
    assert(!renderer_run(0,&t,&m,&es,&e,&a,&st,&cycles));
    assert(!renderer_list_begin(1,&again,pa,32768,&t,0));assert(again.instance_generation!=stale.instance_generation);
    assert(!gfx_list_end(&stale));
    assert(renderer_run(0,&t,&m,&es,&e,&stale,&st,&cycles)==GFX_BUILD_BAD_ARG);
    assert(renderer_list_begin(1,&a,pa,32768,&t,0)==GFX_BUILD_FULL);
    assert(!gfx_list_end(&again));assert(!renderer_run(0,&t,&m,&es,&e,&again,&st,&cycles));
    assert(!renderer_list_begin(1,&a,pa,32768,&t,0));
    puts("F4 LEASE PASS double_buffer=independent busy_reuse=rejected stale_generation=rejected release_after_consumer=ok");
    free(ram);return 0;
}
