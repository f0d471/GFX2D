#include "renderer.h"
#include "board.h"
#include "gfx_engine.h"
#include "gfx/pb.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
static unsigned char *ram;
static uint32_t capabilities;
board_mem board_region(board_region_id id)
{board_mem m={ram+g_mem_regions[id].offset,g_mem_regions[id].size};return m;}
int gfx_engine_caps_read(uint32_t *caps){*caps=capabilities;return 0;}
int main(void)
{
    ram=calloc(1,DDR_SIM_BYTES);assert(ram);
    gfx_surface_table t={0};gfx_list list;
    gfx_surface dst={g_mem_regions[REGION_FB0].offset,640,480,12,0,0};
    assert(!gfx_surface_set(&t,0,&dst));
    const uint32_t cases[]={0,GFX_CAPS_INST_M,GFX_CAPS_BIN_M,GFX_CAPS_INST_M|GFX_CAPS_BIN_M};
    for(unsigned i=0;i<4;i++){
        capabilities=cases[i];
        assert(!renderer_list_begin(3,&list,board_region(REGION_CMDLIST_A).ptr,32768,&t,0));
        if(i<3){assert(list.strip_table&&!list.pb&&!list.instances);assert(!gfx_list_end(&list));}
        else {assert(!list.strip_table&&list.pb&&list.instances);assert(list.pb->bytes==0x20400u);}
    }
    free(ram);
    puts("F5 CAPABILITY PASS caps=none/INST/BIN/both fallback_v2_before_publish=ok");
    return 0;
}
