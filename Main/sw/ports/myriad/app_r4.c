#include <stdint.h>
#include <string.h>
#include "board.h"
#include "renderer.h"
#include "core/format.h"
#include "r4.h"
#include "stg_slots.h"
#include "console_content.h"
#include "app_port.h"

static uint32_t s_prev,s_hw,s_ladder=2,s_fail_base;
static int s_restart,s_started,s_initialized;
void console_r4_boot(appfw_ctx *ctx)
{
    r4_set_log(ctx->log);stg_runtime_r4_install_hooks();r4_set_key_source(appfw_input_held);
}
void console_r4_describe(appfw_ctx *ctx,appfw_detail *detail)
{
    (void)ctx;detail->backend=r4_backend_hardware()?"hw":"sw";
    detail->scene=r4_current_scene();detail->ladder=r4_current_ladder();
}

static int enter(appfw_ctx *c)
{
    uint32_t ticket;
    int blank;
    if(!s_initialized) { s_hw=(uint32_t)app_port_hardware();s_initialized=1; }
    r4_scene_select(0u);    /* 凝时使用独立应用，走廊每次进入都恢复自身场景。 */
    if(stg_runtime_surfaces_r4(content_runtime(c))!=0 || renderer_prepare(1,&content_runtime(c)->tab)!=0)return -1;
#ifdef INJECT_CONSOLE_ENTER
    return appfw_error("injected enter",-1);
#endif
    s_started=1; /* leave is valid even after a partially failed R4 setup. */
    if(r4_mode_enter(&content_runtime(c)->tab,&content_runtime(c)->mem,(int)s_hw,s_ladder)!=0)return -1;
    s_fail_base=r4_fail_count();
    blank=content(c)->shown==STG_SLOT_FB0?STG_SLOT_FB1:STG_SLOT_FB0;
    memset(board_fb(blank),0,FB_BYTES);
    if(board_present_request(blank,&ticket)!=0 || board_present_wait(ticket)!=0)return -1;
    content(c)->shown=blank;
    memset(board_fb(blank^1),0,FB_BYTES);board_wait_frame();
    if(app_port_hardware() && board_display_stats_clear()!=0)return -1;
    r4_timing_start();content(c)->seq=(content(c)->seq&~1u)|(uint32_t)(blank^1);
    s_prev=0;s_restart=0;
    {
        char line[96];core_out out;
        core_out_init(&out,line,sizeof line);core_out_str(&out,"console scene ");
        core_out_u32(&out,r4_current_scene());core_out_str(&out," ladder ");core_out_u32(&out,s_ladder);
        core_out_str(&out,"\r\n");appfw_log(line);
    }
    return 0;
}
static int leave(appfw_ctx *c)
{
    int shown;
    const uint32_t before=r4_fail_count();
    if(renderer_pipeline_flush(&shown)!=0)return appfw_error("r4 flush",-1);
    if(shown>=0)content(c)->shown=shown;
    if(s_started) { r4_mode_leave();s_started=0; }
    return r4_fail_count()==before?0:appfw_error("r4 leave",-1);
}
static int frame(appfw_ctx *c,uint32_t keys,uint32_t n)
{
    const uint32_t pressed=keys&~s_prev;
    r4_out out;
    int dst;
    (void)n;s_prev=keys;
    if(s_restart) {
        const uint32_t restart_keys=s_prev;
        if(leave(c)!=0 || enter(c)!=0)return -1;
        s_prev=restart_keys;
        return APPFW_FRAME_IDLE;
    }
    if(pressed&BOARD_KEY_SWAP) {
        if(app_port_hardware()) { s_hw^=1u;s_restart=1; }
        else appfw_log("console capability cpu-only\r\n");
    }
    if(pressed&BOARD_KEY_SLOW) { s_ladder=s_ladder==1u?2u:1u;s_restart=1; }
    if(s_restart)return APPFW_FRAME_IDLE;
    if(keys&BOARD_KEY_CONFIRM)keys|=BOARD_KEY_BOMB;
    keys&=~BOARD_KEY_CONFIRM;
    dst=(int)(content(c)->seq&1u);
    if(r4_mode_frame(&content_runtime(c)->tab,&content_runtime(c)->mem,dst,keys|r4_pending_keys(),&out)!=0 ||
       r4_fail_count()!=s_fail_base)
        return appfw_error("r4 frame",-1);
    content(c)->shown=dst;++content(c)->seq;return APPFW_FRAME_OK;
}
const appfw_app g_app_r4={APPFW_R4,CONSOLE_KEY_APPFW_R4,CONSOLE_TITLE_APPFW_R4,enter,leave,frame};
