#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "core/format.h"
#include "core/logq.h"
#include "appfw.h"
#include "app_port.h"

#define LOGQ_BYTES 8192u
#define LOG_TAKE_MAX 256u
static logq s_logq;
static char s_log_mem[LOGQ_BYTES], s_log_out[LOG_TAKE_MAX+1u];
static appfw_config s_config;
static appfw_ctx s_ctx;
static const appfw_app *s_active, *s_request, *s_cleanup;
static uint32_t s_frames, s_errors, s_prev, s_blocked;
static uint32_t s_stat_ms, s_stat_swaps, s_app_swaps;
static int s_fault;

const appfw_app *const *appfw_apps(uint32_t *count)
{
    *count=s_config.count;
    return s_config.apps;
}
const appfw_app *appfw_app_at(uint32_t index)
{
    return index<s_config.count?s_config.apps[index]:NULL;
}
const appfw_app *appfw_app_id(uint32_t id)
{
    uint32_t i;
    if (id==s_config.menu->id) return s_config.menu;
    for(i=0;i<s_config.count;++i)
        if(s_config.apps[i]->id==id)return s_config.apps[i];
    return NULL;
}
const appfw_app *appfw_find(const char *name)
{
    uint32_t i;
    if(strcmp(name,s_config.menu->name)==0)return s_config.menu;
    for(i=0;i<s_config.count;++i)
        if(strcmp(name,s_config.apps[i]->name)==0)return s_config.apps[i];
    return NULL;
}
int appfw_available(const appfw_app *app)
{
    if(!app)return 0;
    return !s_config.available || s_config.available(s_config.user,app);
}
void appfw_request(const appfw_app *app)
{
    if(!app) { s_request=NULL;return; }
    if(app && appfw_app_id(app->id)==app && appfw_available(app))s_request=app;
}
int appfw_in_shell(void) { return s_active==s_config.menu && !s_fault; }
uint32_t appfw_active_id(void) { return s_active?s_active->id:UINT32_MAX; }
uint32_t appfw_errors(void) { return s_errors; }
uint32_t appfw_input_held(void)
{
    return app_port_held() & ~s_config.system_mask & ~s_blocked;
}
void appfw_log(const char *line) { (void)logq_put(&s_logq,line); }
static void pump(void)
{
    uint32_t n=app_port_tx_room();
    if(n>LOG_TAKE_MAX)n=LOG_TAKE_MAX;
    if(n && logq_used(&s_logq)) {
        (void)logq_take(&s_logq,s_log_out,n);
        app_port_puts(s_log_out);
    }
}
int appfw_error(const char *detail,int code)
{
    char line[128];core_out out;
    core_out_init(&out,line,sizeof line);
    core_out_str(&out,"console detail ");core_out_str(&out,detail);
    core_out_str(&out," code ");core_out_u32(&out,code<0?0u-(uint32_t)code:(uint32_t)code);
    core_out_str(&out,"\r\n");appfw_log(line);
    return -1;
}
static void event(const char *kind,const appfw_app *app)
{
    char line[96];core_out out;
    core_out_init(&out,line,sizeof line);core_out_str(&out,"console ");
    core_out_str(&out,kind);core_out_ch(&out,' ');core_out_str(&out,app->name);
    core_out_str(&out,"\r\n");appfw_log(line);
}
static void error_event(const char *stage)
{
    char line[96];core_out out;
    ++s_errors;
    core_out_init(&out,line,sizeof line);core_out_str(&out,"console error ");
    core_out_str(&out,stage);core_out_str(&out," total ");core_out_u32(&out,s_errors);
    core_out_str(&out,"\r\n");appfw_log(line);
}
static void complete(const appfw_app *app)
{
    char line[160];core_out out;
    core_out_init(&out,line,sizeof line);core_out_str(&out,"console complete app ");
    core_out_str(&out,app->name);core_out_str(&out," id ");core_out_u32(&out,app->id);
    core_out_str(&out," frames ");core_out_u32(&out,s_frames);
    core_out_str(&out," presents ");core_out_u32(&out,app_port_swap_count()-s_app_swaps);
    core_out_str(&out," errors ");core_out_u32(&out,s_errors);
    core_out_str(&out,"\r\n");appfw_log(line);
}

static int activate(const appfw_app *next)
{
    if(!next || !appfw_available(next))return -1;
    if(s_config.prepare && s_config.prepare(&s_ctx)!=0)return -1;
    if(next->enter(&s_ctx)!=0)return -1;
    s_active=next; /* Commit only after resource preparation succeeds. */
    s_frames=0;s_ctx.fps_milli=0;
    s_stat_ms=app_port_ms();s_stat_swaps=app_port_swap_count();
    s_app_swaps=s_stat_swaps;
    event("enter",next);
    return 0;
}
static int recover(const char *stage)
{
    error_event(stage);
    s_active=NULL;s_request=NULL;s_fault=1;
    /* No new surface or framebuffer writes until the engine has recovered. */
    if(s_config.recover && s_config.recover(&s_ctx)!=0)return -1;
    if(s_cleanup && s_cleanup->leave(&s_ctx)!=0)return -1;
    s_cleanup=NULL;
    if(s_config.reset)s_config.reset(&s_ctx);
    s_blocked|=s_prev;
    if(activate(s_config.menu)!=0)return -1;
    s_fault=0;
    return 0;
}
static int switch_to(const appfw_app *next,uint32_t held)
{
    const appfw_app *old=s_active;
    s_request=NULL;
    if(next==old && !s_fault)return 0;
    if(!appfw_available(next)) { event("unavailable",next);return 0; }
    if(s_fault)return recover("retry");
    if(old && old->leave(&s_ctx)!=0) { s_cleanup=old;return recover("leave"); }
    if(old) { complete(old);event("leave",old); }
    s_active=NULL;
    if(activate(next)!=0) { s_cleanup=next;return recover("enter"); }
    s_blocked|=held&s_config.transition_mask;
    /* Retain held movement; consume control actions until release. */
    return 0;
}
static void status(void)
{
    const uint32_t now=app_port_ms(), elapsed=now-s_stat_ms;
    app_port_stats p;
    appfw_detail detail={"none",0,0};
    if(s_config.describe)s_config.describe(&s_ctx,s_active,&detail);
    char line[384];core_out out;
    if(elapsed<1000u)return;
    s_ctx.fps_milli=(uint32_t)(((uint64_t)(app_port_swap_count()-s_stat_swaps)*1000000u)/elapsed);
    s_stat_ms=now;s_stat_swaps=app_port_swap_count();
    app_port_snapshot(&p);
    core_out_init(&out,line,sizeof line);
    core_out_str(&out,"console status v 2 app ");core_out_str(&out,s_active?s_active->name:"fault");
#define VALUE(label,value) do { core_out_str(&out,label);core_out_u32(&out,(value)); } while(0)
    VALUE(" id ",appfw_active_id());core_out_str(&out," backend ");core_out_str(&out,detail.backend);
    VALUE(" fps_milli ",s_ctx.fps_milli);VALUE(" frames ",s_frames);
    VALUE(" presents ",app_port_swap_count()-s_app_swaps);VALUE(" errors ",s_errors);
    VALUE(" scene ",detail.scene);
    VALUE(" ladder ",detail.ladder);
    VALUE(" dropped ",logq_dropped(&s_logq));VALUE(" tick_ms ",p.tick_ms);
    VALUE(" irqA ",app_port_frame_events());VALUE(" heap ",p.heap_free);
    VALUE(" faults ",app_port_faults());
    VALUE(" tasks ",p.tasks);VALUE(" idle ",p.idle);VALUE(" monitor ",p.monitor);
    VALUE(" health ",p.health_faults);VALUE(" stack_free ",p.stack_free);
    core_out_str(&out,"\r\n");appfw_log(line);
#undef VALUE
}
int appfw_init(const appfw_config *config,uint32_t start_id)
{
    if(s_active || s_fault)return -1; /* Never replace a live/faulted owner's resources. */
    if(!config || !config->menu || !config->apps || !config->count)return -1;
    for(uint32_t i=0;i<config->count;i++) {
        const appfw_app *a=config->apps[i];
        if(!a || !a->name || !a->enter || !a->leave || !a->frame || a->id==config->menu->id)return -1;
        for(uint32_t j=0;j<i;j++)if(config->apps[j]->id==a->id)return -1;
    }
    if(!config->menu->name || !config->menu->enter || !config->menu->leave || !config->menu->frame)return -1;
    s_config=*config;
    logq_init(&s_logq,s_log_mem,LOGQ_BYTES);
    memset(&s_ctx,0,sizeof s_ctx);s_ctx.user=config->user;s_ctx.log=&s_logq;
    s_active=NULL;s_request=NULL;s_cleanup=NULL;s_frames=s_errors=s_prev=s_blocked=0;s_fault=0;
    app_port_puts("console boot\r\nconsole protocol 2 tick_hz 1000\r\n");
    if(s_config.boot && s_config.boot(&s_ctx)!=0)return -1;
    if(activate(appfw_app_id(start_id))!=0) {
        s_cleanup=appfw_app_id(start_id);return recover("boot-enter");
    }
    return 0;
}
int appfw_step(uint32_t keys)
{
    const uint32_t pressed=keys&~s_prev;
    const uint32_t previous=s_prev;
    const appfw_app *next=NULL;
    int rc=APPFW_FRAME_IDLE;
    s_prev=keys;s_blocked&=keys;
    if(s_config.navigate) {
        uint32_t id=s_config.navigate(keys,pressed,previous);
        if(id!=UINT32_MAX)next=appfw_app_id(id);
    }
    if(!next)next=s_request;
    if(next)rc=switch_to(next,keys);
    else if(!s_fault && s_active) {
        uint32_t app_keys=keys&~s_config.system_mask&~s_blocked;
        rc=s_active->frame(&s_ctx,app_keys,s_frames);
        if(rc<0) { s_cleanup=s_active;rc=recover("frame"); }
        else if(rc==APPFW_FRAME_OK)++s_frames;
    }
    status();pump();
    return rc<0?-1:0;
}
int appfw_shutdown(void)
{
    if(s_active) {
        if(s_active->leave(&s_ctx)!=0) { s_cleanup=s_active;error_event("shutdown");s_fault=1; }
        else { complete(s_active);event("leave",s_active); }
    }
    s_active=NULL;
    while(logq_used(&s_logq)) {
        if(!app_port_tx_room())app_port_wait_frame();
        pump();app_port_yield();
    }
    return s_fault || s_errors || logq_dropped(&s_logq)?-1:0;
}
