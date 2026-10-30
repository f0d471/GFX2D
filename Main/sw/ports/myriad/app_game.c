/* Game and stress adapters share the renderer contract, not a second shell. */
#include <stdint.h>
#include <string.h>
#include "board.h"
#include "renderer.h"
#include "frontend_game.h"
#include "stg_modes.h"
#include "stg_slots.h"
#include "console_content.h"
#include "app_port.h"
#include "console_present.h"

static uint32_t s_mode,s_prev;
static uint32_t s_fps[4],s_valid[4];

static stg_main_ctx context(appfw_ctx *c)
{
    stg_main_ctx x;
    x.tab=&content_runtime(c)->tab;x.mem=&content_runtime(c)->mem;x.es=&content_runtime(c)->es;x.err=&content_runtime(c)->err;
    x.cmds[0]=content_runtime(c)->cmds[0];x.cmds[1]=content_runtime(c)->cmds[1];
    x.cap[0]=content_runtime(c)->cap[0];x.cap[1]=content_runtime(c)->cap[1];x.hw=content_runtime(c)->backend;
    return x;
}
static int enter_mode(appfw_ctx *c,uint32_t mode)
{
    if(frontend_game_reset()!=0 || stg_runtime_surfaces_stg(content_runtime(c))!=0)return -1;
    if(stg_runtime_backend_apply(content_runtime(c),(uint32_t)content_runtime(c)->backend,mode)!=0) {
        content_runtime(c)->backend=1;
        if(stg_runtime_backend_apply(content_runtime(c),1,mode)!=0)return -1;
    }
    if(renderer_prepare(content_runtime(c)->backend,&content_runtime(c)->tab)!=0)return -1;
    s_mode=mode;s_prev=0;
    stg_modes_set_presenter(console_present_request);
    stg_modes_set_frame_begin(console_present_begin);
    if(mode==APPFW_PRESS)stg_mode_press_enter();else stg_mode_game_enter();
    return 0;
}
static int game_enter(appfw_ctx *c) { return enter_mode(c,APPFW_GAME); }
static int press_enter(appfw_ctx *c) { return enter_mode(c,APPFW_PRESS); }
static int leave(appfw_ctx *c)
{
    int shown;
    if(renderer_pipeline_flush(&shown)!=0)return appfw_error("stg flush",-1);
    if(shown>=0)content(c)->shown=shown;
    if(frontend_game_reset()!=0)return appfw_error("stg lease",-1);
    stg_modes_reset();stg_modes_set_presenter(NULL);stg_modes_set_frame_begin(NULL);return 0;
}
static int switch_backend(appfw_ctx *c)
{
    if(!app_port_hardware()) {
        appfw_log("console capability cpu-only\r\n");return 0;
    }
    uint32_t next=((uint32_t)content_runtime(c)->backend+1u)%4u;
    int shown;
    if(next>=2u && !content_runtime(c)->tex_ready)next=0;
    if(renderer_pipeline_flush(&shown)!=0)return appfw_error("backend flush",-1);
    if(shown>=0)content(c)->shown=shown;
    if(frontend_game_reset()!=0)return appfw_error("backend lease",-1);
    if(stg_runtime_backend_apply(content_runtime(c),next,s_mode)!=0)return appfw_error("backend resources",-1);
    if(renderer_prepare((int)next,&content_runtime(c)->tab)!=0)return appfw_error("backend prepare",-1);
    content_runtime(c)->backend=(int)next;
    if(s_mode==APPFW_PRESS)stg_mode_press_enter(); /* A search never mixes backends. */
    return 0;
}
static int frame(appfw_ctx *c,uint32_t keys,uint32_t n)
{
    const uint32_t pressed=keys&~s_prev;
    const int dst=(int)(content(c)->seq&1u);
    const uint32_t before=stg_modes_fail();
    stg_main_ctx ctx;
    stg_mode_out out;
    stg_hud_live live;
    (void)n;
#ifdef INJECT_CONSOLE_FRAME
    if(n==10u)return appfw_error("injected frame",-1);
#endif
    s_prev=keys;
    if((pressed&BOARD_KEY_SWAP) && switch_backend(c)!=0)return -1;
    ctx=context(c);
    if(!renderer_pipeline_enabled(content_runtime(c)->backend) && dst==content(c)->shown)
        return appfw_error("stg front buffer",-1);
    s_fps[content_runtime(c)->backend]=c->fps_milli/1000u;s_valid[content_runtime(c)->backend]=c->fps_milli!=0;
    live=(stg_hud_live){s_fps[0],(int)s_valid[0],
        s_fps[content_runtime(c)->backend?content_runtime(c)->backend:1],
        (int)s_valid[content_runtime(c)->backend?content_runtime(c)->backend:1],0};
    memset(&out,0,sizeof out);out.presented_dst=-1;
    if(keys&BOARD_KEY_CONFIRM)keys|=BOARD_KEY_BOMB;
    keys&=~BOARD_KEY_CONFIRM;
    if(s_mode==APPFW_PRESS)stg_mode_press_frame(&ctx,dst,&live,&out);
    else stg_mode_game_frame(&ctx,dst,keys,&live,&out);
    if(stg_modes_fail()!=before)return appfw_error("stg frame",-1);
    if(out.presented_dst>=0)content(c)->shown=out.presented_dst;
    ++content(c)->seq;return APPFW_FRAME_OK;
}
const appfw_app g_app_stg={APPFW_GAME,CONSOLE_KEY_APPFW_GAME,CONSOLE_TITLE_APPFW_GAME,game_enter,leave,frame};
const appfw_app g_app_press={APPFW_PRESS,CONSOLE_KEY_APPFW_PRESS,CONSOLE_TITLE_APPFW_PRESS,press_enter,leave,frame};
