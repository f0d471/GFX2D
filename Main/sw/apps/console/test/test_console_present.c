/* Presentation must upload the already-rendered full-width scene verbatim. */
#include <stdio.h>
#include <string.h>
#include "board.h"
#include "console_ids.h"
#include "console_present.h"
#include "game/myriad_hud.h"

static uint32_t source[2048u*480u+2u],target[1808u*480u+2u];
static unsigned width,stride,source_stride,app,failures,legacy_calls,presents,registered;
static int in_place;
const mem_region g_mem_regions[REGION_COUNT]={
    [REGION_FB0]={0x02000000u,0x00400000u},[REGION_FB1]={0x02400000u,0x00400000u}};
uint32_t appfw_active_id(void) {return app;}
uint32_t *board_fb(int index) {(void)index;return source+1;}
int gfx_surface_set(gfx_surface_table *tab,int index,const gfx_surface *surface)
{
    (void)tab;
    if(index!=0 || surface->width!=width || surface->height!=480 ||
       (1u<<surface->stride_log2)/4u!=source_stride)++failures;
    ++registered;return 0;
}
int board_frame_acquire(int index,int adaptive,board_frame *frame)
{
    if(index!=0 || !adaptive)return -1;
    *frame=(board_frame){in_place?source+1:target+1,width,480,in_place?source_stride:stride};
    return 0;
}
int board_present_request(int index,uint32_t *ticket)
{(void)index;++legacy_calls;*ticket=77;return 0;}
int board_present_frame_request(int index,const board_frame *frame,uint32_t *ticket)
{
    if(index!=0 || frame->width!=width || frame->height!=480)++failures;
    ++presents;*ticket=88;return 0;
}
static void check(const char *name,int pass)
{if(!pass){printf("FAIL %s\n",name);++failures;}}
static uint32_t pixel(unsigned x,unsigned y) {return 0x00400000u+y*2048u+x;}
static void sample(unsigned w,unsigned id,int overlap)
{
    gfx_surface_table tab={0};stg_draw_view view;
    width=w;app=id;in_place=overlap;stride=w+13u;source_stride=w>1024u?2048u:1024u;
    for(unsigned i=0;i<sizeof target/sizeof target[0];++i)target[i]=0xdeadbeefu;
    for(unsigned i=0;i<sizeof source/sizeof source[0];++i)source[i]=0xf00df00du;
    check("full render surface configured before drawing",console_present_begin(&tab,0,&view)==0);
    check("real dimensions reach scene builder",view.width==width && view.hud_x==(int)width-160);
    check("battlefield remains centered in simulation coordinates",view.field_x==((int)width-560)/2);
    for(unsigned y=0;y<480;++y)for(unsigned x=0;x<w;++x)source[1+y*source_stride+x]=pixel(x,y);
    uint32_t ticket=0;check("present succeeds",console_present_request(0,&ticket)==0 && ticket==88);
    const uint32_t *out=overlap?source+1:target+1;
    const unsigned out_stride=overlap?source_stride:stride;
    int pixels_ok=1,padding_ok=1,source_ok=1;
    for(unsigned y=0;y<480;++y) {
        for(unsigned x=0;x<w;++x) {
            pixels_ok&=out[y*out_stride+x]==pixel(x,y);
            source_ok&=source[1+y*source_stride+x]==pixel(x,y);
        }
        for(unsigned x=w;x<out_stride;++x)
            padding_ok&=out[y*out_stride+x]==(overlap?0xf00df00du:0xdeadbeefu);
    }
    check("every rendered scene pixel including former gutters survives",pixels_ok);
    check("source and destination padding untouched",padding_ok && source_ok);
    check("allocation guards",target[0]==0xdeadbeefu && target[sizeof target/sizeof target[0]-1u]==0xdeadbeefu &&
          source[0]==0xf00df00du && source[sizeof source/sizeof source[0]-1u]==0xf00df00du);
}
int main(void)
{
    const unsigned widths[]={640,768,853,1147,1707,1792};
    for(unsigned i=0;i<sizeof widths/sizeof widths[0];++i)
        for(unsigned id=APPFW_DEMO;id<=APPFW_PRESS;++id)sample(widths[i],id,0);
    for(unsigned id=APPFW_DEMO;id<=APPFW_PRESS;++id)sample(640,id,1);
    uint32_t ticket;app=APPFW_MENU;gfx_surface_table tab={0};
    check("menu keeps fixed rendering",console_present_begin(&tab,0,NULL)==1);
    check("menu uses original presentation",console_present_request(0,&ticket)==0 && ticket==77 && legacy_calls==1);
    check("all scene frames configured and presented",registered==21u && presents==21u);
    check("invalid framebuffer rejected",console_present_begin(&tab,-1,NULL)==-1);
    char fit[18];myriad_hud_fit(fit,"THIS STAGE NAME IS LONG",17);
    check("HUD text fits between margins",strlen(fit)==17 && MYRIAD_HUD_TEXT_X+17*8==MYRIAD_HUD_RIGHT);
    check("maximum uint32 value fits",myriad_hud_value_x("4294967295")>=MYRIAD_HUD_TEXT_X+7*8);
    printf("render surface / 6 widths / 3 modes / full scene upload / guards\nfail %u\n",failures);
    return failures?1:0;
}
