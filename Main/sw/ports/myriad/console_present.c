/* Configure the actual render surface before composition. Presentation only
 * uploads that completed surface; it never fabricates scenery or side panels. */
#include <string.h>
#include "board.h"
#include "appfw.h"
#include "console_ids.h"
#include "console_present.h"
#include "render_policy.h"

#ifndef __riscv
static board_frame s_frame[BOARD_FB_COUNT];
static unsigned s_source_stride[BOARD_FB_COUNT];
static int s_ready[BOARD_FB_COUNT];
#endif

int console_present_begin(gfx_surface_table *surfaces,int index,stg_draw_view *view)
{
#ifndef __riscv
    if(index<0 || index>=BOARD_FB_COUNT)return -1;
    s_ready[index]=0;
    const uint32_t app=appfw_active_id();
    if(!GFX_FRAME_DIAGNOSTIC && (app==APPFW_DEMO || app==APPFW_GAME || app==APPFW_PRESS)) {
        board_frame frame;
        if(board_frame_acquire(index,1,&frame))return -1;
        const unsigned log2=frame.width>FB_ROW_PIXELS?13u:FB_STRIDE_LOG2;
        if(((uint64_t)1u<<log2)*frame.height>g_mem_regions[REGION_FB0+index].size)return -1;
        gfx_surface surface={g_mem_regions[REGION_FB0+index].offset,(uint16_t)frame.width,
            (uint16_t)frame.height,(uint8_t)log2,GFX_SPACE_DDR,GFX_FMT_XRGB8888};
        if(gfx_surface_set(surfaces,index,&surface))return -1;
        s_frame[index]=frame;s_source_stride[index]=(1u<<log2)/sizeof(uint32_t);s_ready[index]=1;
        if(view)*view=(stg_draw_view){frame.width,((int)frame.width-160-400)/2,(int)frame.width-160};
        return 0;
    }
#else
    (void)surfaces;(void)index;(void)view;
#endif
    return 1;
}

int console_present_request(int index,uint32_t *ticket)
{
#ifndef __riscv
    const uint32_t app=appfw_active_id();
    if(index>=0 && index<BOARD_FB_COUNT && s_ready[index] &&
       (app==APPFW_DEMO || app==APPFW_GAME || app==APPFW_PRESS)) {
        const board_frame *frame=&s_frame[index];
        const uint32_t *source=board_fb(index);
        if(frame->pixels!=source || frame->stride!=s_source_stride[index])
            for(unsigned y=0;y<frame->height;++y)
                memcpy(frame->pixels+(size_t)y*frame->stride,
                       source+(size_t)y*s_source_stride[index],frame->width*sizeof(uint32_t));
        return board_present_frame_request(index,frame,ticket);
    }
#endif
    return board_present_request(index,ticket);
}
