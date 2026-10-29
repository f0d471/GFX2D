#include "resource_pool.h"
#include "board.h"
int gfx_resource_bind(gfx_mem *mem,gfx_cmd *commands[2],uint32_t capacities[2])
{
    const uint32_t base=g_mem_regions[REGION_ASSET].offset;
    const uint32_t end=g_mem_regions[REGION_OFFSCREEN].offset+g_mem_regions[REGION_OFFSCREEN].size;
    const board_mem asset=board_region(REGION_ASSET);
    if(!mem || !commands || !capacities || !asset.ptr || end>DDR_SIM_BYTES)return -1;
    for(unsigned i=REGION_ASSET;i<REGION_COUNT;i++) {
        board_mem region=board_region((board_region_id)i);
        if(!region.ptr || (uintptr_t)region.ptr-(uintptr_t)asset.ptr!=g_mem_regions[i].offset-base)return -1;
    }
    *mem=(gfx_mem){.dev_base=base,.host=asset.ptr,.bytes=end-base};
    for(unsigned i=0;i<2;i++) {
        board_mem command=board_region((board_region_id)(REGION_CMDLIST_A+i));
        if(!command.ptr || (g_mem_regions[REGION_CMDLIST_A+i].offset&31u))return -1;
        commands[i]=command.ptr;capacities[i]=command.size/sizeof(gfx_cmd);
    }
    return 0;
}
