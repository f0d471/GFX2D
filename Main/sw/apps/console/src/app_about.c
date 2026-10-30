#include <stdint.h>
#include <stddef.h>
#include "core/format.h"
#include "assets.h"
#include "stg_slots.h"
#include "game/stg_text.h"
#include "console_views.h"
#include "app_port.h"

static const stg_font s_font={STG_SLOT_FONT,ASSET_FONT8X16_CELL_W,ASSET_FONT8X16_CELL_H,
    ASSET_FONT8X16_COLS,0x20u,ASSET_FONT8X16_CELLS,0};
static const stg_font s_cyan={STG_SLOT_FONT_CYAN,ASSET_FONT_CYAN_CELL_W,ASSET_FONT_CYAN_CELL_H,
    ASSET_FONT_CYAN_COLS,0x20u,ASSET_FONT_CYAN_CELLS,ASSET_FONT_CYAN_KEY};
static uint32_t s_prev;
static int s_hw;
static int enter(appfw_ctx *c)
{
    s_hw=app_port_hardware();s_prev=0;
    return console_view_prepare(c,CONSOLE_VIEW_STG,s_hw);
}
static int leave(appfw_ctx *c) { (void)c;return 0; }
static uint32_t line(gfx_list *list,int dst,uint32_t row,const char *name,uint32_t value)
{
    char text[32];core_out out;
    core_out_init(&out,text,sizeof text);core_out_u32(&out,value);
    return stg_text_draw(list,&s_cyan,dst,120,120+(int)row*32,name,NULL)+
           stg_text_draw(list,&s_font,dst,344,120+(int)row*32,text,NULL);
}
static int frame(appfw_ctx *c,uint32_t keys,uint32_t n)
{
    const uint32_t pressed=keys&~s_prev;
    int dst;
    const gfx_rect all={0,0,CONSOLE_WIDTH-1,CONSOLE_HEIGHT-1};
    app_port_stats p;
    gfx_list list;
    uint32_t bad=0;
    (void)n;s_prev=keys;
    if(pressed&(PLATFORM_KEY_ACTION|PLATFORM_KEY_CONFIRM)) {
        appfw_request(appfw_app_id(APPFW_MENU));return APPFW_FRAME_IDLE;
    }
    app_port_snapshot(&p);
    if(console_view_begin(c,&list,&dst)!=0)return -1;
    bad+=(uint32_t)(gfx_set_clip(&list,all)!=0);
    bad+=(uint32_t)(gfx_fill(&list,dst,all,0xff0a0e16u,GFX_EQ_COPY,0)!=0);
    bad+=stg_text_draw(&list,&s_cyan,dst,120,64,"SYSTEM INFORMATION",NULL);
    bad+=line(&list,dst,0,"UPTIME MS",p.tick_ms);
    bad+=line(&list,dst,1,"HEAP FREE",p.heap_free);
    bad+=line(&list,dst,2,"TASKS",p.tasks);
    bad+=line(&list,dst,3,"DISPLAY FIELDS",console_view_fields());
    bad+=line(&list,dst,4,"PRESENTS",console_view_presents());
    bad+=line(&list,dst,5,"IRQ FAULTS",console_view_faults());
    bad+=stg_text_draw(&list,&s_font,dst,120,400,"ESC / SPACE: BACK TO MENU",NULL);
    bad+=stg_text_draw(&list,&s_font,dst,120,432,
                     app_port_hardware()?"TI60 / FREERTOS / TICK 1 MS":"PC / HOST EVENT LOOP",NULL);
    if(bad || gfx_list_end(&list)!=0)return appfw_error("about build",-1);
    if(console_view_submit(c,&list,dst,s_hw)!=0)return -1;
    return APPFW_FRAME_OK;
}
const appfw_app g_app_about={APPFW_ABOUT,CONSOLE_KEY_APPFW_ABOUT,CONSOLE_TITLE_APPFW_ABOUT,enter,leave,frame};
