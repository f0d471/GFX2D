/* Actual appfw.c + registry with fake platform/resources: lifecycle fault tests. */
#include <stdio.h>
#include <string.h>

#include "appfw.h"
#include "input.h"
#include "app_port.h"




static unsigned failures;
static uint32_t now_ms,swaps,held;
static int hardware,caps,enter_fail,frame_fail,leave_fail,leave_left,recover_fail;
static unsigned entered[6],left[6],framed[6],recoveries;
static uint32_t delivered[6];
static char output[65536];static size_t output_len;
static int draw_frames=1;
#define CHECK(c,name) do { if(!(c)) { ++failures;printf("FAIL %s\n",name); } else printf("ok %s\n",name); } while(0)

static int enter_id(unsigned id) { ++entered[id];return enter_fail==(int)id?-1:0; }
static int leave_id(unsigned id)
{
    ++left[id];
    if(leave_fail==(int)id && leave_left) { --leave_left;return -1; }
    return 0;
}
static int frame_id(unsigned id,appfw_ctx *ctx,uint32_t keys)
{
    ++framed[id];delivered[id]=keys;
    if(frame_fail==(int)id)return -1;
    if(draw_frames) { ++swaps;(void)ctx; }
    return 0;
}
#define APP(symbol,id,text) \
static int symbol##_enter(appfw_ctx *c) { (void)c;return enter_id(id); } \
static int symbol##_leave(appfw_ctx *c) { (void)c;return leave_id(id); } \
static int symbol##_frame(appfw_ctx *c,uint32_t k,uint32_t f) { (void)f;return frame_id(id,c,k); } \
const appfw_app symbol={id,text,text,symbol##_enter,symbol##_leave,symbol##_frame}
APP(g_app_menu,0,"menu");APP(g_app_demo,1,"demo");APP(g_app_stg,2,"stg");
APP(g_app_press,3,"press");APP(g_app_r4,4,"r4");APP(g_app_about,5,"about");

uint32_t app_port_ms(void) { return now_ms; }
uint32_t app_port_keys(void) { return held; }
uint32_t app_port_held(void) { return held; }
int app_port_hardware(void) { return hardware; }
void app_port_snapshot(app_port_stats *p)
{ *p=(app_port_stats){.tick_ms=now_ms,.heap_free=57920,.tasks=4,.idle=now_ms,.monitor=now_ms/1000}; }
void app_port_idle(void) {}
uint32_t app_port_swap_count(void) { return swaps; }
uint32_t app_port_frame_events(void) { return now_ms*60u/1000u; }
uint32_t app_port_faults(void) { return 0; }
uint32_t app_port_tx_room(void) { return 65535; }
void app_port_wait_frame(void) { ++now_ms; }
void app_port_yield(void) {}
void app_port_puts(const char *s)
{
    size_t n=strlen(s);
    if(output_len+n<sizeof output) { memcpy(output+output_len,s,n+1);output_len+=n; }
}
extern const appfw_app *const g_appfw_registry[];
extern const uint32_t g_appfw_registry_n;
static int fake_recover(appfw_ctx *c) { (void)c;++recoveries;return recover_fail?-1:0; }
static int fake_available(void *u,const appfw_app *a) { (void)u;return a->id!=4 || !hardware || caps; }
static uint32_t navigate(uint32_t k,uint32_t p,uint32_t prev)
{
    uint32_t launch=k&PLATFORM_KEY_LAUNCH_MASK;
    if(p&PLATFORM_KEY_BACK)return 0;
    if(launch==PLATFORM_KEY_LAUNCH_MASK && (prev&PLATFORM_KEY_LAUNCH_MASK)!=launch)return 4;
    if(launch!=PLATFORM_KEY_LAUNCH_MASK) {
        if(p&PLATFORM_KEY_MODE1)return 1;
        if(p&PLATFORM_KEY_MODE2)return 2;
        if(p&PLATFORM_KEY_MODE3)return 3;
    }
    return UINT32_MAX;
}
static appfw_config config(void)
{
    return (appfw_config){.menu=&g_app_menu,.apps=g_appfw_registry,.count=g_appfw_registry_n,
        .system_mask=PLATFORM_KEY_LAUNCH_MASK|PLATFORM_KEY_BACK,
        .transition_mask=PLATFORM_KEY_LAUNCH_MASK|PLATFORM_KEY_BACK|PLATFORM_KEY_CONFIRM|
            PLATFORM_KEY_ACTION|PLATFORM_KEY_SCENE|PLATFORM_KEY_BACKEND|PLATFORM_KEY_SLOW,
        .recover=fake_recover,.available=fake_available,.navigate=navigate};
}
static void reset(void)
{
    static int started;
    enter_fail=frame_fail=leave_fail=-1;leave_left=recover_fail=0;
    if(started) {
        /* Each test releases the previous owner, including a failed shutdown. */
        (void)appfw_step(0);(void)appfw_step(PLATFORM_KEY_BACK);(void)appfw_shutdown();
    }
    memset(entered,0,sizeof entered);memset(left,0,sizeof left);memset(framed,0,sizeof framed);
    memset(delivered,0,sizeof delivered);memset(output,0,sizeof output);output_len=0;
    now_ms=swaps=held=0;hardware=0;caps=1;
    enter_fail=frame_fail=leave_fail=-1;leave_left=recover_fail=0;recoveries=0;draw_frames=1;
    appfw_config cfg=config();
    CHECK(appfw_init(&cfg,0)==0,"init-menu");
    CHECK(appfw_init(&cfg,0)!=0,"live-owner-cannot-reinitialize");
    started=1;
}
static void step(uint32_t keys) { held=keys;(void)appfw_step(keys); }

int main(void)
{
    uint32_t count;
    reset();
    CHECK(appfw_active_id()==0,"default-is-system-menu");
    CHECK(appfw_apps(&count)[0]->id==1 && count==5,"menu-not-launchable-content");
    CHECK(appfw_app_id(1)==&g_app_demo && appfw_app_id(2)==&g_app_stg &&
          appfw_app_id(3)==&g_app_press && appfw_app_id(4)==&g_app_r4,"stable-four-ids");
    step(128);step(0);CHECK(appfw_active_id()==1,"global-demo");
    step(256);step(0);CHECK(appfw_active_id()==2,"global-game");
    step(512);step(0);CHECK(appfw_active_id()==3,"global-pressure");
    step(896);step(0);CHECK(appfw_active_id()==4,"global-r4");
    CHECK((delivered[4]&PLATFORM_KEY_LAUNCH_MASK)==0,"launch-not-delivered-to-app");
    step(PLATFORM_KEY_SCENE);CHECK(delivered[4]==PLATFORM_KEY_SCENE,"scene-is-private-action");
    step(0);step(PLATFORM_KEY_BACK);CHECK(appfw_active_id()==0,"back-independent-of-frame-rate");
    step(PLATFORM_KEY_BACK);CHECK(entered[0]==2,"held-back-does-not-reenter");

    reset();appfw_request(&g_app_stg);step(PLATFORM_KEY_ACTION);step(PLATFORM_KEY_ACTION);
    CHECK(delivered[2]==0,"confirm-event-consumed-until-release");
    step(0);step(PLATFORM_KEY_ACTION);CHECK(delivered[2]==PLATFORM_KEY_ACTION,"new-business-key-after-release");
    step(0);step(PLATFORM_KEY_MODE1|PLATFORM_KEY_UP);step(PLATFORM_KEY_UP);
    CHECK(delivered[1]==PLATFORM_KEY_UP,"held-movement-survives-navigation");

    reset();enter_fail=4;step(896);step(0);
    CHECK(appfw_active_id()==0 && appfw_errors()==1,"failed-enter-recovers-to-menu");
    CHECK(framed[4]==0 && left[4]==1,"failed-app-never-frames-and-cleans-up");

    reset();step(128);step(0);frame_fail=1;step(0);
    CHECK(appfw_active_id()==0 && recoveries==1,"frame-failure-recovers");
    { unsigned n=framed[1];step(0);CHECK(framed[1]==n,"failed-frame-not-called-again"); }

    reset();step(128);step(0);leave_fail=1;leave_left=1;step(256);
    CHECK(appfw_active_id()==0 && recoveries==1 && framed[2]==0,"leave-failure-cannot-activate-next");

    reset();step(128);step(0);frame_fail=1;recover_fail=1;step(0);
    CHECK(appfw_active_id()==UINT32_MAX,"undrained-engine-stays-faulted");
    { unsigned n=framed[0];step(PLATFORM_KEY_BACK);CHECK(framed[0]==n,"no-menu-draw-before-drain"); }
    recover_fail=0;frame_fail=-1;step(0);step(PLATFORM_KEY_BACK);
    CHECK(appfw_active_id()==0,"explicit-retry-after-recovery");

    reset();hardware=1;caps=0;step(896);step(0);
    CHECK(appfw_active_id()==0 && framed[4]==0 && appfw_errors()==0,"unsupported-app-is-denied");
    CHECK(appfw_visible_count()==4 && appfw_visible_at(3)->id==5 && appfw_visible_at(4)==NULL,
          "filtered-index-does-not-use-application-id");
    reset();appfw_request(&g_app_stg);appfw_request(NULL);step(0);
    CHECK(appfw_active_id()==0,"pending-launch-can-be-cancelled");
    reset();step(128);step(0);leave_fail=1;leave_left=1;
    CHECK(appfw_shutdown()!=0 && appfw_active_id()==UINT32_MAX,"failed-shutdown-is-not-successful-leave");

    reset();draw_frames=0;now_ms=1100;swaps=66;step(0);
    while(output_len<10 || strstr(output,"fps_milli")==NULL)step(0);
    CHECK(strstr(output,"fps_milli 60000")!=NULL,"fps-normalized-by-elapsed-time");
    CHECK(appfw_shutdown()==0,"clean-shutdown");
    {
        appfw_app independent=g_app_demo;independent.id=42;
        const appfw_app *apps[]={&independent};
        appfw_config cfg=config();cfg.apps=apps;cfg.count=1;cfg.navigate=NULL;
        CHECK(appfw_init(&cfg,0)==0,"independent-framework-config");
        appfw_request(&independent);step(0);
        CHECK(appfw_active_id()==42,"framework-has-no-fixed-content-id");
        CHECK(appfw_shutdown()==0,"independent-framework-shutdown");
        const appfw_app *duplicates[]={&independent,&independent};cfg.apps=duplicates;cfg.count=2;
        CHECK(appfw_init(&cfg,0)!=0,"duplicate-ids-rejected");
        appfw_app other=independent;other.id=97;
        const appfw_app *same_name[]={&independent,&other};cfg.apps=same_name;
        CHECK(appfw_init(&cfg,0)!=0,"duplicate-names-rejected");
        cfg.apps=NULL;cfg.count=0;
        CHECK(appfw_init(&cfg,0)==0,"empty-catalog-still-has-system-menu");
        CHECK(appfw_visible_count()==0 && appfw_visible_at(0)==NULL,"empty-view-is-safe");
        CHECK(appfw_shutdown()==0,"empty-catalog-shutdown");
    }
    printf("fail %u\n",failures);return failures?1:0;
}
