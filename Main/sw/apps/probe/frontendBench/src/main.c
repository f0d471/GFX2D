#include "gfx/pb.h"
/* F1：冻结真实游戏状态，计时内调用生产组帧，消费与排序在计时外。 */
#include "board.h"
#include "gfx/gfx.h"
#include "gfx_engine.h"
#include "core/assetpak.h"
#include "core/format.h"
#include "core/crc32.h"
#include "core/replay.h"
#include "game/stg_game.h"
#include "game/stg_kind.h"
#include "stg_modes.h"
#include "renderer.h"
#include "frontend_game.h"
#include "assets.h"
#include "stg_clear.h"
#include <string.h>
#ifndef FRONTEND_ITERATIONS
#define FRONTEND_ITERATIONS 1000u
#endif
#ifdef GFX_FRONTEND_DETAIL
static uint64_t detail_cycles[7];static uint32_t detail_calls[7];
uint32_t frontend_detail_clock(void){return board_cycles();}
void frontend_detail_end(uint32_t stage,uint32_t start)
{detail_cycles[stage]+=board_cycles()-start;detail_calls[stage]++;}
#endif
#ifndef FRONTEND_FIRST
#define FRONTEND_FIRST 0u
#endif
#ifndef FRONTEND_LAST
#define FRONTEND_LAST 17u
#endif
#ifndef FRONTEND_MODE_MASK
#define FRONTEND_MODE_MASK 0x7FFu
#endif
#ifndef FRONTEND_MODE_FIRST
#define FRONTEND_MODE_FIRST 0u
#endif
#ifndef FRONTEND_MODE_LAST
#define FRONTEND_MODE_LAST 10u
#endif
#ifndef FRONTEND_EXECUTE
#define FRONTEND_EXECUTE 0
#endif
#ifndef FRONTEND_SCENE_MASK
#define FRONTEND_SCENE_MASK 0x1ffffu
#endif
extern int frontend_memory_bench(void);
extern int frontend_sets_bench(void);
#ifdef FRONTEND_DRIVER_ONLY
extern int frontend_driver_probe(void);
#endif
static gfx_strip_workspace work;
static gfx_geometry_cache geometry;
extern uint32_t frontend_consume(const gfx_list *);
#ifdef FRONTEND_VERIFY
extern int frontend_verify(const gfx_list *,const gfx_mem *,const exec_sw_stats *,uint32_t *,uint32_t *);
#endif
#ifndef __riscv
extern void frontend_dump(uint32_t,uint32_t,const gfx_list *);
#endif
#ifdef GFX_FRONTEND_PROFILE
static uint32_t stage_time[9],stage_records[9];
static uint64_t stage_cycles[8],stage_counts[8];
void frontend_stage(uint32_t stage,const gfx_list *l)
{stage_time[stage]=board_cycles();stage_records[stage]=l->used;}
#endif
static stg_game game;
static gfx_surface_table tab;
static stg_draw_cfg draw;
static stg_hud_live live;
static uint32_t samples[FRONTEND_ITERATIONS], fails,probe_avg,probe_max;
static const uint32_t entries[13]={ASSET_BUL16_ENTRY,ASSET_BUL8_ENTRY,ASSET_RICE20_ENTRY,
 ASSET_FONT8X16_ENTRY,ASSET_PSHOT_ENTRY,ASSET_SPARK_ENTRY,ASSET_SHIPS_ENTRY,ASSET_BOSS_ENTRY,
 ASSET_STARS_ENTRY,ASSET_TITLE_ENTRY,ASSET_DIGITS_ENTRY,ASSET_ICONS_ENTRY,ASSET_FONT_CYAN_ENTRY};
static void number(uint32_t v){char b[16];core_out o;core_out_init(&o,b,sizeof b);core_out_u32(&o,v);board_puts(b);}
static void field(uint32_t v){board_puts(",");number(v);}
static int cmp_u32(const void *a,const void *b)
{uint32_t x=*(const uint32_t *)a,y=*(const uint32_t *)b;return x>y?1:x<y?-1:0;}
#include <stdlib.h>
static int setup(void)
{
    board_mem a=board_region(REGION_ASSET),s=board_region(REGION_SHARED);
    assetpak p;assetpak_limits lim={a.size};uint32_t bytes,i,off=0;
    gfx_surface dst={g_mem_regions[REGION_FB0].offset,640,480,12,0,0};
    if(board_asset_read(0,a.ptr,ASSET_PAK_TOTAL_LEN))return 1;
    bytes=ASSET_PAK_TOTAL_LEN;
    if(assetpak_parse(&p,a.ptr,bytes,&lim))return 2;
    gfx_surface_reset(&tab);if(gfx_surface_set(&tab,0,&dst))return 3;
    for(i=0;i<13u;i++) {
        assetpak_entry e;gfx_surface z;
        if(assetpak_entry_at(&p,entries[i],&e))return 4;
        z=(gfx_surface){g_mem_regions[REGION_ASSET].offset+e.offset,e.width,e.height,e.stride_log2,0,e.format};
        if(i==0u||i==1u||i==5u){memcpy((uint8_t *)s.ptr+off,(uint8_t *)a.ptr+e.offset,e.length);z.base=off;z.space=1;off+=e.length;}
        if(gfx_surface_set(&tab,(int)i+2,&z))return 5;
    }
#ifdef __riscv
    if(gfx_engine_soft_reset()||gfx_engine_tex_write(s.ptr,0,off))return 6;
#endif
    {const gfx_surface bg={g_mem_regions[REGION_OFFSCREEN].offset,640,480,12,0,0};
     if(gfx_surface_set(&tab,15,&bg))return 7;}
    stg_modes_draw_cfg(&draw,0);return 0;
}
static void scene(uint32_t id)
{
    static const uint32_t k[6]={0,384,1024,1648,1984,2400};uint32_t i;
    stg_game_init(&game,STG_GAME_SEED);
    if(id<12u){stg_game_press_begin(&game,id<6u?STG_KIND_ROUND16:STG_KIND_SMALL8);stg_game_set_knob(&game,k[id%6u]);for(i=0;i<1200u;i++)stg_game_step(&game,0);}
    else {
        static const uint32_t f[5]={0,5000,9000,9027,9207};replay_view r;
        if(replay_open(stg_clear_bytes,STG_CLEAR_LEN,&r)) {fails++;return;}
        for(i=0;i<=f[id-12u];i++){uint32_t keys;if(replay_key_at(&r,i,&keys)){fails++;break;}stg_game_step(&game,keys);}
    }
}
static void measure(uint32_t id,uint32_t mode,uint32_t repeat)
{
    board_mem mem=board_region(REGION_CMDLIST_A);gfx_list l;stg_frame_stats st;
    gfx_strip_config cfg={30,4,0,0};uint32_t i,start,crc,expected=0;uint64_t total=0;
    uint64_t exec_total=0,pre_total=0,band_total=0;uint32_t exec_max=0,perf_start[9]={0},perf_end[9]={0};
    uint32_t extended_start[GFX_PERF_COUNT]={0},extended_end[GFX_PERF_COUNT]={0};
    board_mem shared=board_region(REGION_SHARED);
    uint8_t *workspace=(uint8_t *)shared.ptr+0x40000u;
    const int bg_mode=(mode==5u||mode==12u)?1:((mode>=6u&&mode<=9u)||mode==13u)?2:0;
    const int planned=mode==2u||mode==4u||mode==7u||mode==9u;
    const int templ=mode==3u||mode==4u||mode==8u||mode==9u;
#ifdef FRONTEND_VERIFY
    exec_sw_stats last_stats={0};
#endif
    if(bg_mode)cfg.bg_id=15;
#ifdef GFX_FRONTEND_DETAIL
    memset(detail_cycles,0,sizeof detail_cycles);memset(detail_calls,0,sizeof detail_calls);
#endif
#ifdef GFX_FRONTEND_PROFILE
    memset(stage_cycles,0,sizeof stage_cycles);memset(stage_counts,0,sizeof stage_counts);
#endif
    work=(gfx_strip_workspace){.items=(gfx_strip_plan_item *)(void *)workspace,.item_cap=32768,
        .refs=(uint32_t *)(void *)(workspace+0x180000u),.ref_cap=30720};
    for(i=0;i<FRONTEND_ITERATIONS+8u;i++) {
        start=board_cycles();
        if(mode==0u || mode>=10u) {
            gfx_list_begin(&l,mem.ptr,32768,&tab);
            if(mode>=10u) {
                static gfx_instance_arena ia;
                ia=(gfx_instance_arena){(uint8_t *)shared.ptr+0x400000u,
                    g_mem_regions[REGION_SHARED].offset+0x400000u,0x80000u,0u,repeat+1u};
                l.instances=&ia;
                if(mode>=11u){
                    static gfx_pb_config pc;static gfx_pb_workspace pb;
                    pc=(gfx_pb_config){cfg,1024,8192};
                    pb=(gfx_pb_workspace){(uint8_t *)shared.ptr+0x500000u,g_mem_regions[REGION_SHARED].offset+0x500000u,0x20400u,repeat+1u};
                    l.pb_config=&pc;l.pb=&pb;
                }
            }
        }
        else if(planned?gfx_list_begin_strips_planned(&l,mem.ptr,mem.size,&tab,&cfg,0,&work):
                        gfx_list_begin_strips(&l,mem.ptr,mem.size,&tab,&cfg,0)){fails++;return;}
        if(templ&&gfx_list_geometry(&l,&geometry)){fails++;return;}
        if(frontend_game_compose_selected(&game,&l,bg_mode,&draw,&live,&st)){fails++;return;}
        if(i>=8u){samples[i-8u]=board_cycles()-start;total+=samples[i-8u];}
#ifdef GFX_FRONTEND_PROFILE
        if(i>=8u){uint32_t s;for(s=0;s<8;s++){stage_cycles[s]+=(uint32_t)(stage_time[s+1]-stage_time[s]);stage_counts[s]+=(uint32_t)(stage_records[s+1]-stage_records[s]);}}
#endif
        if(l.first_error||l.rejected||!l.ended){fails++;return;}
        if(i==7u)expected=frontend_consume(&l);
        if(i==8u && FRONTEND_EXECUTE){renderer_perf_read(perf_start);
#ifdef __riscv
            gfx_engine_perf_read(extended_start,GFX_PERF_COUNT);
#endif
        }
        if(FRONTEND_EXECUTE) {
            exec_sw_state es;exec_sw_error er;exec_sw_stats xs;uint32_t elapsed;
            gfx_mem mm={0,(uint8_t *)((uintptr_t)board_region(REGION_CMDLIST_A).ptr-g_mem_regions[REGION_CMDLIST_A].offset),DDR_SIM_BYTES,shared.ptr,16384};
            exec_sw_reset(&es);
#ifdef __riscv
            if(renderer_run(1,&tab,&mm,&es,&er,&l,&xs,&elapsed)){fails++;return;}
#else
            if(renderer_run(0,&tab,&mm,&es,&er,&l,&xs,&elapsed)){fails++;return;}
#endif
            if(i>=8u){exec_total+=elapsed;if(elapsed>exec_max)exec_max=elapsed;}
            if(i>=8u && l.frame) {
                if(l.frame->count==2u)pre_total+=l.frame->pass[0].elapsed_cycles;
                band_total+=l.frame->pass[l.frame->count-1u].elapsed_cycles;
            }
#ifdef FRONTEND_VERIFY
            last_stats=xs;
#endif
        } else if(l.frame&&gfx_frame_release(l.frame,l.frame->ticket)){fails++;return;}
    }
    crc=frontend_consume(&l);if(crc!=expected)fails++;
    if(mode>=10u) {
        uint32_t pk4=0,pk8=0,in4=0,in8=0,k;
        for(k=0;k<l.used;k++)if(gfx_op(&l.buf[k])==GFX_OP_BLIT_INST) {
            if(l.buf[k].w[5]&(1u<<16)){pk8++;in8+=l.buf[k].w[2];}else{pk4++;in4+=l.buf[k].w[2];}
        }
        board_puts("finst");field(id);field(repeat);field(l.instances->used);
        field(pk4);field(in4);field(pk8);field(in8);board_puts("\n");
    }
    if(FRONTEND_EXECUTE)renderer_perf_read(perf_end);
#ifdef __riscv
    if(FRONTEND_EXECUTE && mode>=11u){
        gfx_engine_perf_read(extended_end,GFX_PERF_COUNT);
        board_puts("fbin");field(id);field(mode);field(repeat);field(l.pb->bytes);field(l.pb_config->draw_cap);field(l.pb_config->ref_cap);
        for(uint32_t q=GFX_PERF_INSTANCE;q<GFX_PERF_COUNT;q++)field(extended_end[q]-extended_start[q]);
        board_puts("\n");
    }
#else
    (void)extended_start;(void)extended_end;
#endif
#ifdef FRONTEND_VERIFY
    {
        uint32_t hc=0,sc=0;int result;
        gfx_mem mm={0,(uint8_t *)((uintptr_t)board_region(REGION_CMDLIST_A).ptr-g_mem_regions[REGION_CMDLIST_A].offset),DDR_SIM_BYTES,shared.ptr,16384};
        result=frontend_verify(&l,&mm,&last_stats,&hc,&sc);
        board_puts("fverify");field(id);field(mode);field(repeat);field((uint32_t)result);field(hc);field(sc);
        field(last_stats.cmd);field(last_stats.pixel_total);field(last_stats.key_skip);board_puts("\n");
        if(l.frame) {
            uint32_t pass,k;
            for(pass=0;pass<l.frame->count;pass++) {
                const gfx_pass *p=&l.frame->pass[pass];
                board_puts("fpass");field(id);field(mode);field(repeat);field(pass);field(p->draw->used);
                for(k=0;k<9u;k++)field(p->perf[k]);
                field(p->elapsed_cycles);board_puts("\n");
            }
        }
        if(result)fails++;
    }
#endif
#ifndef __riscv
    if(!repeat)frontend_dump(id,mode,&l);
#endif
    qsort(samples,FRONTEND_ITERATIONS,sizeof samples[0],cmp_u32);
    board_puts("fcsv");field(id);field(mode);field(repeat);field(FRONTEND_ITERATIONS);
    field((uint32_t)(total>>32));field((uint32_t)total);field(samples[FRONTEND_ITERATIONS/2u]);
    field(samples[(FRONTEND_ITERATIONS*99u)/100u]);field(samples[FRONTEND_ITERATIONS-1u]);
    field(st.act.ebullet);field(l.used);field(l.used*32u);field(l.rejected);field(crc);
    field((uint32_t)(exec_total/FRONTEND_ITERATIONS));field(exec_max);
    field((uint32_t)(pre_total/FRONTEND_ITERATIONS));field((uint32_t)(band_total/FRONTEND_ITERATIONS));
    field((uint32_t)(perf_end[8]-perf_start[8])/FRONTEND_ITERATIONS);
    {uint32_t eq,logical=0;for(eq=0;eq<=GFX_EQ_MAX;eq++)logical+=st.by_eq[eq];field(logical);
     for(eq=0;eq<=GFX_EQ_MAX;eq++)field(st.by_eq[eq]);}
    field(planned?work.items_used:0u);field(planned?work.refs_used:0u);
    field(templ?geometry.hits:0u);field(templ?geometry.misses:0u);
    board_puts("\n");
#ifdef GFX_FRONTEND_DETAIL
    for(i=0;i<7u;i++){board_puts("fdetail");field(id);field(mode);field(repeat);field(i);field((uint32_t)(detail_cycles[i]/(FRONTEND_ITERATIONS+8u)));field(detail_calls[i]/(FRONTEND_ITERATIONS+8u));board_puts("\n");}
#endif
#ifdef GFX_FRONTEND_PROFILE
    for(i=0;i<8;i++){board_puts("fstage");field(id);field(mode);field(repeat);field(i);field((uint32_t)(stage_cycles[i]/FRONTEND_ITERATIONS));field((uint32_t)(stage_counts[i]/FRONTEND_ITERATIONS));board_puts("\n");}
#endif
}
int main(void)
{
    uint32_t id,m,r;board_init();crc32_init();
#ifdef FRONTEND_DRIVER_ONLY
    {uint32_t start=board_cycles();while((uint32_t)(board_cycles()-start)<BOARD_HZ/2u){}}
    board_puts("frontend driver guard\n");fails=(uint32_t)frontend_driver_probe();
    board_puts("fail ");number(fails);board_puts("\ndone\n");for(;;){}
#endif
    {
        uint32_t i,total=0,max=0;
        for(i=0;i<1000u;i++){uint32_t start=board_cycles(),d=board_cycles()-start;total+=d;if(d>max)max=d;}
        probe_avg=total/1000u;probe_max=max;
        board_puts("fprobe");field(total/1000u);field(max);board_puts("\n");
    }
#if defined(FRONTEND_ISOLATE) && (defined(FRONTEND_MEMORY) || defined(FRONTEND_SETS))
    if(board_begin_compare()){board_puts("isolate FAIL\nfail 1\ndone\n");return 1;}
#endif
#ifdef FRONTEND_MEMORY
    board_puts("frontendBench memory clock=100000000\n");
    fails=(uint32_t)frontend_memory_bench();board_puts("fail ");number(fails);board_puts("\ndone\n");
    for(;;){}
#endif
#ifdef FRONTEND_SETS
    board_puts("frontendBench sets clock=100000000\n");
    fails=(uint32_t)frontend_sets_bench();board_puts("fail ");number(fails);board_puts("\ndone\n");for(;;){}
#endif
    board_puts("frontendBench F1 clock=100000000 iterations=");number(FRONTEND_ITERATIONS);board_puts("\n");
    if(setup()){board_puts("setup FAIL\nfail 1\ndone\n");return 1;}
    board_puts("fprobe");field(probe_avg);field(probe_max);board_puts("\n");
#ifdef FRONTEND_ISOLATE
    if(board_begin_compare()){board_puts("isolate FAIL\nfail 1\ndone\n");return 1;}
#endif
#ifdef FRONTEND_VERIFY
    memset(board_fb(0),0xa5,FB_BYTES);
    memset((uint8_t *)board_region(REGION_OFFSCREEN).ptr+0x600000u,0xa5,FB_BYTES);
#endif
    if(FRONTEND_EXECUTE) {
        uint32_t x,y;uint32_t *front=board_fb(1);
        for(y=0;y<480u;y++)for(x=0;x<640u;x++)front[y*1024u+x]=0;
        board_irq_init();
        if(renderer_prepare(1,&tab)){board_puts("prepare FAIL\nfail 1\ndone\n");return 1;}
        board_present(1);
    }
    board_puts("fh,scene,mode,repeat,n,total_hi,total_lo,p50,p99,max,visible_enemy,records,bytes,errors,crc,exec_avg,exec_max,pre_avg,band_avg,gpu_busy_avg,logical_draws,eq_copy,eq_alpha,eq_add,eq_const,eq_mul,eq_st,plan_items,refs,template_hits,template_misses\n");
    for(id=FRONTEND_FIRST;id<FRONTEND_LAST;id++)if(FRONTEND_SCENE_MASK&(1u<<id))
        {scene(id);for(r=0;r<3u;r++)for(m=FRONTEND_MODE_FIRST;m<FRONTEND_MODE_LAST;m++)if(FRONTEND_MODE_MASK&(1u<<m))measure(id,m,r);}
    {uint32_t frame=0,low=0,zero=0,under=0;
     board_display_diag(&frame,&low,&zero,&under);board_puts("fdiag");field(frame);field(low);field(zero);field(under);field(board_irq_faults());board_puts("\n");}
    board_puts("fail ");number(fails);board_puts("\ndone\n");
#ifdef __riscv
    for(;;){}
#endif
    return fails?1:0;
}
