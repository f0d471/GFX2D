#define _GNU_SOURCE
#include "renderer.h"
#include "board.h"
#include "gfx_engine.h"
#include "gfx_regs.h"
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* 实际 renderer 的异步协议模型；设备解释器独立消费表，不做性能结论。 */
static gfx_surface_table device,captured;
static gfx_mem memory;
static uint32_t ticks,events,perf[GFX_PERF_COUNT],wait_ticks,swaps;
static int busy,front=1,next_front=1,is_strips,reset_error;
static const gfx_cmd *commands;
static const void *table;
static uint32_t count,ecode,eindex,etag;
static gfx_strip_config config;
static uint32_t scratch[32768];
static void need(int ok,const char *s){if(!ok){printf("FAIL pipeline: %s\n",s);exit(1);}}
static void finish_device(void)
{
    exec_sw_state es;exec_sw_stats st={0};exec_sw_error err;int rc;uint32_t dst;
    exec_sw_reset(&es);
    if(is_strips){rc=exec_sw_run_strips(&captured,&memory,table,GFX_STRIP_TABLE_BYTES,&config,scratch,sizeof scratch,&st,&err);dst=config.dst_id;}
    else {rc=exec_sw_run(&es,&captured,&memory,commands,count,count,&st,&err);dst=gfx_dst(commands);}
    if(!rc)need(captured.s[dst].base!=g_mem_regions[REGION_FB0+front].offset,"GPU wrote scanning surface");
    ecode=(uint32_t)rc;eindex=err.index;etag=err.tag;
    perf[0]+=st.cmd;perf[1]+=st.pixel_total-st.src_oob;perf[2]+=st.key_skip;perf[8]+=1000;
    busy=0;events++;
}
void board_puts(const char *s){fputs(s,stdout);}
uint32_t board_cycles(void){ticks+=100;return ticks;}
uint32_t board_irq_engine_events(void){if(busy&&!--wait_ticks)finish_device();return events;}
void board_engine_irq_enable(void){}
board_mem board_region(board_region_id id){return (board_mem){(void *)(uintptr_t)g_mem_regions[id].offset,g_mem_regions[id].size};}
uint32_t *board_fb(int i){return (uint32_t *)(uintptr_t)g_mem_regions[REGION_FB0+i].offset;}
int board_present_request(int i,uint32_t *ticket){need(!busy,"present before DONE");next_front=i;*ticket=swaps;return 0;}
int board_present_wait(uint32_t ticket){need(ticket==swaps,"wrong present ticket");ticks+=1666667u;front=next_front;swaps++;return 0;}
int gfx_engine_caps_read(uint32_t *caps){*caps=GFX_CAPS_VALUE;return 0;}
int gfx_engine_soft_reset(void){need(!busy,"reset while active");if(reset_error)return GFX_ENG_E_TIMEOUT;gfx_surface_reset(&device);memset(perf,0,sizeof perf);return 0;}
int gfx_engine_surf_set_space(int id,uint32_t base,uint32_t w,uint32_t h,uint32_t stride,uint32_t space,uint32_t format)
{gfx_surface s={base,(uint16_t)w,(uint16_t)h,(uint8_t)stride,(uint8_t)space,(uint8_t)format};if(busy)return GFX_ENG_E_REJECT;return gfx_surface_set(&device,id,&s);}
int gfx_engine_submit(uint32_t base,uint32_t n)
{need(!busy,"overlapping submit");captured=device;commands=(const gfx_cmd *)(uintptr_t)base;count=n;is_strips=0;busy=1;wait_ticks=2;return 0;}
int gfx_engine_submit_strips(uint32_t base,uint32_t strips,uint32_t log2,uint32_t bg,uint32_t dst)
{need(!busy,"overlapping BAND");captured=device;table=(const void *)(uintptr_t)base;config=(gfx_strip_config){strips,log2,bg,dst};is_strips=1;busy=1;wait_ticks=3;return 0;}
int gfx_engine_wait(uint32_t *code,uint32_t *index,uint32_t *tag)
{need(!busy,"wait consumed unfinished device");*code=ecode;*index=eindex;*tag=etag;return 0;}
int gfx_engine_perf_read(uint32_t *v,uint32_t n){memcpy(v,perf,n*4u);return 0;}
typedef struct {gfx_surface_table tab;gfx_list pre,dyn,child;gfx_frame_plan plan;} context;
static context ctx[2];static gfx_frame_arena arena;
static uint32_t mask_for(uint32_t i){return 15u<<(i*4u);}
static void compose_kind(uint32_t frame,int cached)
{
    uint32_t idx=frame&1u,b=idx*4u;context *c=&ctx[idx];gfx_pass p={0};
    gfx_surface d0={g_mem_regions[REGION_FB0].offset,640,480,12,0,0},d1=d0,bg=d0;
    gfx_strip_config cfg={30,4,15,idx};uint32_t color=(frame+1u)*0x00010101u;
    d1.base=g_mem_regions[REGION_FB1].offset;bg.base=g_mem_regions[REGION_OFFSCREEN].offset+idx*0x200000u;
    need(!gfx_frame_begin(&c->plan,&arena,frame,mask_for(idx)),"frame lease begin");
    if(cached){uint32_t x,y;uint32_t *pixels=(uint32_t *)(uintptr_t)bg.base;for(y=0;y<480;y++)for(x=0;x<640;x++)pixels[y*1024+x]=color;}
    gfx_surface_reset(&c->tab);gfx_surface_set(&c->tab,0,&d0);gfx_surface_set(&c->tab,1,&d1);gfx_surface_set(&c->tab,15,&bg);
    gfx_list_begin(&c->pre,(gfx_cmd *)(uintptr_t)arena.page[b+1u].base,128,&c->tab);
    gfx_fill(&c->pre,15,(gfx_rect){0,0,639,479},color,0,0);gfx_list_end(&c->pre);
    need(!gfx_list_begin_strips(&c->dyn,(void *)(uintptr_t)arena.page[b+2u].base,GFX_STRIP_TABLE_BYTES,&c->tab,&cfg,NULL),"BAND begin");
    gfx_fill(&c->dyn,(int)idx,(gfx_rect){(int)(frame%640u),1,(int)(frame%640u),1},0x00ffffff,0,0);gfx_list_end(&c->dyn);c->child=c->dyn;
    p.surfaces=c->tab;p.draw=&c->pre;p.target=(gfx_pass_resource){bg.base,FB_BYTES,1,b};p.dependency=GFX_PASS_NO_DEP;
    if(!cached)need(!gfx_frame_add_pass(&c->plan,&p),"prepass plan");
    p.draw=&c->child;p.target=(gfx_pass_resource){idx?d1.base:d0.base,FB_BYTES,1,b+3u};
    p.initial=(gfx_pass_resource){bg.base,FB_BYTES,1,b};p.load=1;p.dependency=cached?GFX_PASS_NO_DEP:0u;p.initialized=cached;
    need(!gfx_frame_add_pass(&c->plan,&p)&&!gfx_frame_seal(&c->plan),"frame seal");c->dyn.frame=&c->plan;
}
int main(void)
{
    uint32_t i;int presented;exec_sw_stats st;uint32_t cyc,ready;
    void *mapped=mmap((void *)0x100000u,DDR_SIM_BYTES,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
    need(mapped==(void *)0x100000u,"low address mapping unavailable");memory=(gfx_mem){0x100000u,mapped,DDR_SIM_BYTES,NULL,0};
    for(i=0;i<2;i++) {
        need(!gfx_frame_arena_add(&arena,g_mem_regions[REGION_OFFSCREEN].offset+i*0x200000u,0x200000u,1),"BG arena");
        need(!gfx_frame_arena_add(&arena,g_mem_regions[REGION_SHARED].offset+0x800000u+i*0x10000u,0x10000u,1),"pre arena");
        need(!gfx_frame_arena_add(&arena,g_mem_regions[REGION_CMDLIST_A+i].offset,0x100000u,1),"cmd arena");
        need(!gfx_frame_arena_add(&arena,g_mem_regions[REGION_FB0+i].offset,0x400000u,1),"dst arena");
    }
    {gfx_surface_table empty={0};need(!renderer_prepare(3,&empty),"prepare");}
#ifdef F4_LEASE_RESET_TEST
    {
        gfx_surface_table empty={0};gfx_list old,again;gfx_cmd *p=board_region(REGION_CMDLIST_A).ptr;
        need(!renderer_list_begin(2,&old,p,32768,&empty,0)&&old.instances,"instance lease begin");
        reset_error=1;need(renderer_prepare(2,&empty)!=0,"failed reset accepted");
        need(renderer_list_begin(2,&again,p,32768,&empty,0)==GFX_BUILD_FULL,"failed drain released lease");
        reset_error=0;need(!renderer_prepare(2,&empty),"reset recovery");
        need(!renderer_list_begin(2,&again,p,32768,&empty,0),"drained lease not recovered");
        need(old.instance_generation!=again.instance_generation,"reset generation reused");
        need(!gfx_list_end(&again),"recovered list end");
        exec_sw_state es;exec_sw_error er;exec_sw_stats xs;uint32_t cyc;exec_sw_reset(&es);
        need(!renderer_run(0,&empty,&memory,&es,&er,&again,&xs,&cyc),"recovered arena consumer");
        puts("F4 LEASE RESET PASS failed_drain=held later_successful_reset=released generation=renewed");
    }
#endif
    for(i=0;i<64;i++) {
        uint32_t before;gfx_frame_plan extra;
        if(i==1)arena.next_ticket=UINT32_MAX;
        compose_kind(i,0);before=ticks;
        {gfx_census all;need(!renderer_list_census(&ctx[i&1u].tab,&ctx[i&1u].dyn,&all),"whole pass census");
         need(gfx_census_cmd_total(&all)==2&&gfx_census_pix_total(&all)==307201u,"prepass omitted from census");}
        need(!renderer_pipeline_step(&ctx[i&1u].dyn,(int)(i&1u),&st,&cyc,&ready,&presented),"step");
        need(busy&&ctx[i&1u].plan.arena&& !ctx[i&1u].plan.drained,"leases retained until BAND DONE");
        need(gfx_frame_begin(&extra,&arena,1000,mask_for(i&1u))==GFX_BUILD_FULL,"inflight page reuse rejected");
        need((uint32_t)(ready-before)<1666667u,"swap wait leaked into frame budget");
        if(i) {
            uint32_t *fb=board_fb(presented);need(presented==(int)((i-1u)&1u),"presented frame id");
            need(fb[0]==i*0x00010101u&&fb[1024u+(i-1u)%640u]==0x00ffffffu,"frame pixel/generation mismatch");
            need(st.cmd==2&&st.pixel_total==307201u,"whole frame PERF aggregation");
        } else need(presented==-1,"prime present");
    }
    need(!renderer_pipeline_flush(&presented)&&presented==1&&!busy,"final drain");
    for(i=0;i<arena.count;i++)need(!arena.page[i].owner,"final lease released");
    {uint32_t old=swaps;compose_kind(64,0);ctx[0].pre.buf[0].w[0]=0x7fu;
     need(renderer_pipeline_step(&ctx[0].dyn,0,&st,&cyc,&ready,&presented)!=0,"prepass fault accepted");
     need(swaps==old&&!busy,"failed prepass presented");
     for(i=0;i<arena.count;i++)need(!arena.page[i].owner,"error drain release");}
    compose_kind(65,1);
    need(renderer_pipeline_step(&ctx[1].dyn,1,&st,&cyc,&ready,&presented)!=0&&!busy,"front reuse after failed frame rejected");
    compose_kind(66,1);
    need(!renderer_pipeline_step(&ctx[0].dyn,0,&st,&cyc,&ready,&presented),"cached single pass pipeline");
    need(busy&&ctx[0].plan.arena,"cached lease retained");
    need(!renderer_pipeline_flush(&presented)&&presented==0&&board_fb(0)[0]==67u*0x00010101u,"cached single pass pixels");
    puts("PASS: 64 async frames plus cached pass, two owned BG/CMD sets, generation/ticket wrap, whole PERF, no scan overwrite, error no present");
    munmap(mapped,DDR_SIM_BYTES);return 0;
}
