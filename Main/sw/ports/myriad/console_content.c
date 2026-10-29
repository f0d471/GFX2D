/* Composition root for one content family; the manager has no game knowledge. */
#include <string.h>
#include "board.h"
#include "app_port.h"
#include "console_content.h"
#include "renderer.h"
#include "stg_modes.h"
#include "r4.h"
static console_content s_content;
extern const appfw_app g_app_menu;
extern const appfw_app *const g_appfw_registry[];
extern const uint32_t g_appfw_registry_n;
static int boot(appfw_ctx *ctx)
{
    console_content *c=content(ctx);memset(c,0,sizeof *c);c->shown=-1;
    if(stg_runtime_boot(&c->runtime)!=0)return -1;
    stg_runtime_set_active(&c->runtime);c->runtime.backend=app_port_hardware()?1:0;
    stg_modes_set_log(ctx->log);r4_set_log(ctx->log);
    stg_runtime_r4_install_hooks();r4_set_key_source(appfw_input_held);
    if(app_port_hardware() && stg_runtime_tex_load(&c->runtime)!=0)
        app_port_puts("console capability tex unavailable\r\n");
    return 0;
}
static int prepare(appfw_ctx *ctx)
{
    console_content *c=content(ctx);exec_sw_reset(&c->runtime.es);
    if(c->shown>=0)c->seq=(c->seq&~1u)|((uint32_t)c->shown^1u);
    return 0;
}
static int recover(appfw_ctx *ctx) { (void)ctx;return renderer_recover(); }
static void reset(appfw_ctx *ctx)
{
    stg_runtime *r=content_runtime(ctx);stg_modes_reset();r->tex_on=r->tex_ready=0;
    if(app_port_hardware())(void)stg_runtime_tex_load(r);
}
static int available(void *user,const appfw_app *app)
{ (void)user;return app->id!=APPFW_R4 || !app_port_hardware() || stg_runtime_has_raster(); }
static void describe(appfw_ctx *ctx,const appfw_app *app,appfw_detail *detail)
{
    static const char *const names[]={"sw","hw","hw-tex","hw-strip"};
    uint32_t backend=content_runtime(ctx)->backend;
    if(!app)return;
    if(app->id==APPFW_MENU || app->id==APPFW_ABOUT)detail->backend=app_port_hardware()?"hw":"sw";
    else if(app->id==APPFW_R4) {
        detail->backend=r4_backend_hardware()?"hw":"sw";
        detail->scene=r4_current_scene();detail->ladder=r4_current_ladder();
    } else detail->backend=names[backend<4u?backend:0u];
}
static uint32_t navigate(uint32_t keys,uint32_t pressed,uint32_t previous)
{
    uint32_t launch=keys&BOARD_KEY_LAUNCH_MASK;
    if(pressed&BOARD_KEY_BACK)return APPFW_MENU;
    if(launch==BOARD_KEY_LAUNCH_MASK && (previous&BOARD_KEY_LAUNCH_MASK)!=launch)return APPFW_R4;
    if(launch!=BOARD_KEY_LAUNCH_MASK) {
        if(pressed&BOARD_KEY_MODE1)return APPFW_DEMO;
        if(pressed&BOARD_KEY_MODE2)return APPFW_GAME;
        if(pressed&BOARD_KEY_MODE3)return APPFW_PRESS;
    }
    return UINT32_MAX;
}
const appfw_config *console_content_config(void)
{
    static appfw_config cfg;
    cfg=(appfw_config){.user=&s_content,.menu=&g_app_menu,.apps=g_appfw_registry,.count=g_appfw_registry_n,
        .system_mask=BOARD_KEY_LAUNCH_MASK|BOARD_KEY_BACK,
        .transition_mask=BOARD_KEY_LAUNCH_MASK|BOARD_KEY_BACK|BOARD_KEY_CONFIRM|BOARD_KEY_BOMB|
                         BOARD_KEY_SCENE|BOARD_KEY_SWAP|BOARD_KEY_SLOW,
        .boot=boot,.prepare=prepare,.recover=recover,.reset=reset,.available=available,
        .describe=describe,.navigate=navigate};
    return &cfg;
}
