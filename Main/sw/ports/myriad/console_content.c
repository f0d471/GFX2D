/* Composition root for one content family; the manager has no game knowledge. */
#include <string.h>
#include "board.h"
#include "app_port.h"
#include "console_content.h"
#include "renderer.h"
#include "stg_modes.h"
#include "gfx_engine.h"
#include "core/format.h"
#include "console_catalog.h"
static console_content s_content;
extern const appfw_app g_app_menu;
extern const appfw_app *const g_appfw_registry[];
extern const uint32_t g_appfw_registry_n;
static int boot(appfw_ctx *ctx)
{
    console_content *c=content(ctx);memset(c,0,sizeof *c);c->shown=-1;
    if(stg_runtime_boot(&c->runtime)!=0)return -1;
    stg_runtime_set_active(&c->runtime);c->runtime.backend=app_port_hardware()?1:0;
    if(app_port_hardware() && gfx_engine_caps_read(&c->caps)!=GFX_ENG_OK)return -1;
    stg_modes_set_log(ctx->log);console_modules_boot(ctx);
    if(app_port_hardware() && stg_runtime_tex_load(&c->runtime)!=0)
        app_port_puts("console capability tex unavailable\r\n");
    console_catalog_report();
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
{
    const console_content *c=user;
    const console_app_meta *m=console_app_metadata(app->id);
    if(app->id==APPFW_MENU)return 1;
    if(!m)return 0;
    return console_app_supported(m,app_port_hardware(),c->caps);
}
static void describe(appfw_ctx *ctx,const appfw_app *app,appfw_detail *detail)
{
    static const char *const names[]={"sw","hw","hw-tex","hw-strip"};
    uint32_t backend=content_runtime(ctx)->backend;
    detail->catalog=g_console_catalog_sha256;detail->capabilities=content(ctx)->caps;
    if(!app)return;
    const console_app_meta *m=console_app_metadata(app->id);
    if(app->id==APPFW_MENU || (m && strcmp(m->kind,"system")==0))detail->backend=app_port_hardware()?"hw":"sw";
    else if(m && m->describe)m->describe(ctx,detail);
    else detail->backend=names[backend<4u?backend:0u];
}
static uint32_t navigate(uint32_t keys,uint32_t pressed,uint32_t previous)
{
    uint32_t launch=keys&BOARD_KEY_LAUNCH_MASK;
    if(pressed&BOARD_KEY_BACK)return APPFW_MENU;
    if(launch==BOARD_KEY_LAUNCH_MASK && (previous&BOARD_KEY_LAUNCH_MASK)!=launch)return console_shortcut(4);
    if(launch!=BOARD_KEY_LAUNCH_MASK) {
        if(pressed&BOARD_KEY_MODE1)return console_shortcut(1);
        if(pressed&BOARD_KEY_MODE2)return console_shortcut(2);
        if(pressed&BOARD_KEY_MODE3)return console_shortcut(3);
    }
    return UINT32_MAX;
}
void console_catalog_report(void)
{
    char line[320];core_out o;
    core_out_init(&o,line,sizeof line);core_out_str(&o,"console catalog begin v 1 profile ");
    core_out_str(&o,g_console_profile);core_out_str(&o," hash ");core_out_str(&o,g_console_catalog_sha256);
    core_out_str(&o," count ");core_out_u32(&o,g_appfw_registry_n);core_out_str(&o,"\r\n");app_port_puts(line);
    for(uint32_t i=0;i<g_appfw_registry_n;i++) {
        const console_app_meta *m=&g_console_catalog[i];
        core_out_init(&o,line,sizeof line);core_out_str(&o,"console catalog app id ");core_out_u32(&o,m->app->id);
        core_out_str(&o," key ");core_out_str(&o,m->app->name);core_out_str(&o," kind ");core_out_str(&o,m->kind);
        core_out_str(&o," graphics ");core_out_str(&o,m->graphics);core_out_str(&o," shortcut ");core_out_u32(&o,m->shortcut);
        core_out_str(&o," available ");core_out_u32(&o,(uint32_t)appfw_available(m->app));core_out_str(&o," title ");
        for(const char *p=m->app->desc;*p;p++) {
            static const char digits[]="0123456789abcdef";
            core_out_ch(&o,digits[(unsigned char)*p>>4]);core_out_ch(&o,digits[(unsigned char)*p&15u]);
        }
        core_out_str(&o,"\r\n");app_port_puts(line);
        app_port_runner_progress();app_port_yield();
    }
    app_port_puts("console catalog end\r\n");
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
