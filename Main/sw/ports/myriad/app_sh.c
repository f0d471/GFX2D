/* 凝时 HALTTIME:控制台第 6 个应用(场景 3,按 6 直进)。
   世界几何见 sh_scene.c,背景素色见 r4_runtime.c 的 build_background。
   与第 4/5 个应用共用 R4 运行时;L1 平涂进入(零纹理,与山脊同口径)。 */
#include <stdint.h>
#include <string.h>
#include "board.h"
#include "renderer.h"
#include "core/format.h"
#include "r4.h"
#include "stg_slots.h"
#include "console_content.h"
#include "app_port.h"
#include "sh_scene.h"

#define SH_SCENE 3u

static uint32_t s_fail_base;
static int s_started, s_initialized, s_hw;

void console_sh_boot(appfw_ctx *ctx)
{
    r4_set_log(ctx->log);
    stg_runtime_r4_install_hooks();
    r4_set_key_source(appfw_input_held);
}

void console_sh_describe(appfw_ctx *ctx, appfw_detail *detail)
{
    (void)ctx;
    detail->backend = r4_backend_hardware() ? "hw" : "sw";
    detail->scene = r4_current_scene();
    detail->ladder = r4_current_ladder();
}

static int enter(appfw_ctx *c)
{
    uint32_t ticket;
    int blank;
    if (!s_initialized) { s_hw = (uint32_t)app_port_hardware(); s_initialized = 1; }
#if SH_BOARD_SOFTWARE
    s_hw=0;
#endif
    r4_scene_select(SH_SCENE);
    if (stg_runtime_surfaces_r4(content_runtime(c)) != 0 ||
        renderer_prepare(1, &content_runtime(c)->tab) != 0)
        return -1;
    s_started = 1;
    /* 凝时锁定 L1 平涂:零纹理进入,跳过 atlas 装载。 */
    if (r4_mode_enter(&content_runtime(c)->tab, &content_runtime(c)->mem,
                      (int)s_hw, 1u) != 0)
        return -1;
    s_fail_base = r4_fail_count();
    blank = content(c)->shown == STG_SLOT_FB0 ? STG_SLOT_FB1 : STG_SLOT_FB0;
    memset(board_fb(blank), 0, FB_BYTES);
    if (board_present_request(blank, &ticket) != 0 || board_present_wait(ticket) != 0)
        return -1;
    content(c)->shown = blank;
    memset(board_fb(blank ^ 1), 0, FB_BYTES);
    board_wait_frame();
    if (app_port_hardware() && board_display_stats_clear() != 0)
        return -1;
    r4_timing_start();
    content(c)->seq = (content(c)->seq & ~1u) | (uint32_t)(blank ^ 1);
    board_pointer_capture(1);
    {
        char line[64];
        core_out out;
        core_out_init(&out, line, sizeof line);
        core_out_str(&out, "console scene 3 halttime\r\n");
        appfw_log(line);
    }
    return 0;
}

static int leave(appfw_ctx *c)
{
    board_pointer_capture(0);
    int shown;
    const uint32_t before = r4_fail_count();
    if (renderer_pipeline_flush(&shown) != 0)
        return appfw_error("halttime flush", -1);
    if (shown >= 0)
        content(c)->shown = shown;
    if (s_started) { r4_mode_leave(); s_started = 0; }
    int result=r4_fail_count() == before ? 0 : appfw_error("halttime leave", -1);
#ifdef SH_BOARD_CAPTURE
    char line[48];core_out out;core_out_init(&out,line,sizeof line);
    core_out_str(&out,"SHDONE ");core_out_u32(&out,r4_fail_count()-s_fail_base);
    core_out_str(&out,"\r\n");appfw_log(line);
#endif
    return result;
}

static int frame(appfw_ctx *c, uint32_t keys, uint32_t n)
{
    r4_out out;
    int dst;
    int32_t dx,dy;
    (void)n;
#ifdef SH_BOARD_CAPTURE
    keys=0;
#ifdef SH_BOARD_WALL
    keys=n==1u?PLATFORM_KEY_ACTION:n==2u?PLATFORM_KEY_RESTART:0u;
#endif
#ifdef SH_BOARD_SEQUENCE
    static const uint32_t sequence[]={0,PLATFORM_KEY_ACTION,PLATFORM_KEY_RIGHT,
        PLATFORM_KEY_UP,PLATFORM_KEY_STRAFE_R,PLATFORM_KEY_RESTART,
        PLATFORM_KEY_LEFT,PLATFORM_KEY_ACTION};
    keys=sequence[n%8u];
#endif
#endif
    board_pointer_read(&dx,&dy);
    sh_world_look(dx,dy);
    dst = (int)(content(c)->seq & 1u);
    if (r4_mode_frame(&content_runtime(c)->tab, &content_runtime(c)->mem, dst,
                      keys | r4_pending_keys(), &out) != 0 ||
        r4_fail_count() != s_fail_base)
        return appfw_error("halttime frame", -1);
    if(out.presented_fb>=0)content(c)->shown=STG_SLOT_FB0+out.presented_fb;
    ++content(c)->seq;
#ifdef SH_BOARD_CAPTURE
    if(out.presented_fb<0)return APPFW_FRAME_OK;
    char line[128];core_out text;core_out_init(&text,line,sizeof line);core_out_str(&text,"SHCRC");
    uint32_t values[]={out.frames-1u,out.records,out.record_crc,out.crc,out.fail};
    for(unsigned i=0;i<5;++i){core_out_ch(&text,' ');core_out_u32(&text,values[i]);}
    core_out_str(&text,"\r\n");appfw_log(line);
    if(out.frames>=SH_BOARD_CAPTURE)appfw_request(appfw_app_id(APPFW_MENU));
#endif
    return APPFW_FRAME_OK;
}

const appfw_app g_app_sh = {
    APPFW_SH, CONSOLE_KEY_APPFW_SH, CONSOLE_TITLE_APPFW_SH,
    enter, leave, frame
};
