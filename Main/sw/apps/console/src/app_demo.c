#include <stdint.h>
#include "game/sw6_cfg.h"
#include "game/sw6_scene_a.h"
#include "game/sw6_scene_b.h"
#include "game/sw6_hud.h"
#include "console_views.h"
#include "app_port.h"

static sw6_scene_a s_a;
static sw6_scene_b s_b;
static uint32_t s_stage,s_start,s_prev;
static int s_hw;

static int enter(appfw_ctx *c)
{
    if(console_demo_prepare(c,&s_b)!=0)return -1;
    sw6_a_begin(&s_a,SW6_SEED_DEMO,SW6_N_START);
    s_hw=console_view_backend(c);
    s_stage=0;s_start=app_port_ms();s_prev=0;
    return 0;
}
static int leave(appfw_ctx *c)
{
    return console_view_flush(c);
}
static int frame(appfw_ctx *c,uint32_t keys,uint32_t n)
{
    const uint32_t pressed=keys&~s_prev;
    const uint32_t now=app_port_ms();
    int dst;
    gfx_list list;
    gfx_rect all={0,0,CONSOLE_WIDTH-1,CONSOLE_HEIGHT-1};
    sw6_build_stat fs;
    sw6_hud_in hud={0};
    uint32_t cmd,pix;
    (void)n;s_prev=keys;
    if(pressed&PLATFORM_KEY_BACKEND) {
        if(app_port_hardware())s_hw=!s_hw;
        else appfw_log("console capability cpu-only\r\n");
        console_view_set_backend(c,s_hw);
        if(console_view_prepare(c,CONSOLE_VIEW_DEMO,s_hw)!=0)return -1;
    }
    if(now-s_start>=10000u || (pressed&PLATFORM_KEY_SCENE)) { s_stage^=1u;s_start=now; }
    sw6_build_stat_clear(&fs);
    if(console_view_begin(c,&list,&dst)!=0)return -1;
    const unsigned width=console_view_width(c,dst);all.x1=(int)width-1;
    if(gfx_set_clip(&list,all)!=0)return -1;
    if(!s_stage) {
        sw6_a_adjust(&s_a,(pressed&PLATFORM_KEY_UP)!=0,(pressed&PLATFORM_KEY_DOWN)!=0);
        sw6_a_update(&s_a);
        if(gfx_fill(&list,dst,all,SW6_BG_COLOR,GFX_EQ_COPY,0)!=0)return -1;
        sw6_a_build_view(&s_a,&list,dst,width,&fs);
    } else {
        uint32_t scene;
        sw6_b_adjust(&s_b,(pressed&PLATFORM_KEY_UP)!=0,(pressed&PLATFORM_KEY_DOWN)!=0);
        scene=sw6_b_step(&s_b);
        if(width==CONSOLE_WIDTH) {
            if(gfx_blit(&list,SW6_SLOT_OFF(scene),0,0,dst,0,0,CONSOLE_WIDTH,CONSOLE_HEIGHT,GFX_EQ_COPY,0,0,0)!=0)return -1;
        } else sw6_b_build_view(scene,&list,dst,width,&fs);
    }
    hud.n=s_stage?0:s_a.n;hud.k=s_stage?s_b.k:0;
    hud.sw_fps=c->fps_milli/1000u;hud.sw_fps_valid=!s_hw && c->fps_milli!=0;
    hud.hw_fps=hud.sw_fps;hud.hw_fps_valid=s_hw && c->fps_milli!=0;hud.live=1;
    sw6_hud_build_at(&list,dst,&hud,(int)width-(int)SW6_HUD_W,&cmd,&pix);
    if(fs.bad || sw6_hud_bad() || gfx_list_end(&list)!=0 || list.rejected || list.overflow)
        return appfw_error("demo build",-1);
    if(console_view_submit(c,&list,dst,s_hw)!=0)return -1;
    return APPFW_FRAME_OK;
}
const appfw_app g_app_demo={APPFW_DEMO,CONSOLE_KEY_APPFW_DEMO,CONSOLE_TITLE_APPFW_DEMO,enter,leave,frame};
