/* Compile the actual target scheduler on the host. The engine model retains
 * descriptors until wait(), independently checks generations and ownership. */
#define __riscv 1
#define GFX2D_R4_JUDGE 1
#define R4_BOARD_CRC 0
#define R4_HUD_NONE 1
#include "../../../sw/ports/myriad/r4_runtime.c"
#include <stdlib.h>

#define CHECK(x) do{if(!(x)){fprintf(stderr,"SH PIPE FAIL line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static uint32_t ram[DDR_SIM_BYTES/4u];
static uint32_t clock_ms,field,swaps,prepared,presented,logic_keys,logic_ticks;
static int front,display_pending,engine_pending,fail_emit,fail_wait,reset_count;
static gfx_engine_raster in_flight;
static uint32_t in_list,in_dst,in_bg,refs;
static uint8_t record_copy[1024],bin_copy[32768],header_copy[120];
static uint8_t ranges[2][2u*SH_PC_RECORD_CAP];
static unsigned range_index;
static uint32_t keys_by_frame[64];
static const uint32_t worlds[]={2,0,3,1,0,4};
static const uint32_t guns[]={1,2,0,2,1,0};
static void *at(uint32_t address){CHECK(address<sizeof ram);return (uint8_t *)ram+address;}
static void stable(void)
{
    if(!engine_pending)return;
    CHECK(!memcmp(record_copy,at(in_flight.tri_base),in_flight.tri_alloc));
    CHECK(!memcmp(bin_copy,at(in_flight.sub_base),in_flight.sub_bytes));
    CHECK(!memcmp(header_copy,at(in_list),sizeof header_copy));
}
board_mem board_region(board_region_id id)
{return (board_mem){at(g_mem_regions[id].offset),g_mem_regions[id].size};}
uint32_t *board_fb(int i){CHECK(i>=0 && i<2);return at(g_mem_regions[REGION_FB0+i].offset);}
uint32_t board_cycles(void){return clock_ms*100000u;}
uint64_t board_time_ms(void){return clock_ms;}
uint32_t board_frame_count(void){return field;}
uint32_t board_swap_count(void){return swaps;}
void board_wait_yield(void){++field;clock_ms+=17;}
int board_should_quit(void){return 0;}
void board_puts(const char *s){(void)s;}
int board_frame_acquire(int i,int adaptive,board_frame *frame)
{(void)adaptive;*frame=(board_frame){board_fb(i),640,480,FB_ROW_PIXELS};return 0;}
static int rendered[2];
int board_present_frame_request(int i,const board_frame *frame,uint32_t *ticket)
{
    CHECK(frame->pixels==board_fb(i));CHECK(!engine_pending && !display_pending);
    CHECK(i!=front && rendered[i]==(int)presented);
    display_pending=1;*ticket=swaps;return 0;
}
int board_present_wait(uint32_t ticket)
{
    CHECK(display_pending && ticket==swaps && !engine_pending);
    front=s_pipe_buf[s_pipe_cur].dst_slot;
    ++swaps;++presented;field+=2;clock_ms+=34;display_pending=0;return 0;
}
int board_display_diag(uint32_t *f,uint32_t *m,uint32_t *z,uint32_t *u)
{*f=field;*m=347;*z=*u=0;return 0;}
uint32_t board_display_response_errors(void){return 0;}
int gfx_engine_submit_raster(uint32_t list,uint32_t bg,uint32_t dst,const gfx_engine_raster *p)
{
    CHECK(!engine_pending && !display_pending);
    const uint32_t gen=presented,k=gen%6u;
    const uint32_t base=g_mem_regions[REGION_SHARED].offset;
    const uint32_t rec=base+((gen&1u)?R4_REC_B_OFF:R4_REC_OFF);
    const int weapon=bg==R4_SH_WORLD_SLOT || worlds[k]==0;
    const uint32_t first=weapon?worlds[k]:0;
    CHECK(p->tri_base==rec+first*RAST_RECORD_BYTES);
    CHECK(p->tri_count==(weapon?guns[k]:worlds[k]));
    CHECK(p->sub_base==base+(weapon?((gen&1u)?R4_GUN_SUB_B_OFF:R4_SH_GUN_SUB_OFF):
        ((gen&1u)?R4_SUB_B_OFF:R4_SUB_OFF)));
    CHECK(p->sub_alloc==(weapon?((gen&1u)?R4_GUN_SUB_B_CAP:R4_SH_GUN_SUB_CAP):
        ((gen&1u)?R4_SUB_B_CAP:R4_SUB_CAP)));
    CHECK(dst==R4_SH_WORLD_SLOT || (int)dst!=front);
    for(unsigned i=0;i<p->tri_count;++i) {
        const rast_record *r=(const rast_record *)at(p->tri_base)+i;
        CHECK(rast_get32(r,80)==gen+1u);
        CHECK(rast_get32(r,84)==keys_by_frame[gen]);
    }
    in_flight=*p;in_list=list;in_dst=dst;in_bg=bg;engine_pending=1;
    memcpy(record_copy,at(p->tri_base),p->tri_alloc);
    memcpy(bin_copy,at(p->sub_base),p->sub_bytes);
    memcpy(header_copy,at(list),sizeof header_copy);
    return GFX_ENG_OK;
}
int gfx_engine_wait(uint32_t *code,uint32_t *index,uint32_t *tag)
{
    stable();*code=0;*index=0;*tag=0;
    if(engine_pending) {
        if(fail_wait){*code=3;return GFX_ENG_OK;}
        refs=(in_flight.sub_bytes-120u)/2u;
        if(in_dst<2u)rendered[in_dst]=(int)presented;
        engine_pending=0;clock_ms+=3;
    }
    return GFX_ENG_OK;
}
int gfx_engine_raster_info(uint32_t *id,uint32_t *caps,uint32_t *cycles,uint32_t *r)
{*id=GFX_R3_ID_VALUE;*caps=0;*cycles=300000;*r=refs;return 0;}
int gfx_engine_error_stage_read(uint32_t *stage){*stage=0;return 0;}
int gfx_engine_soft_reset(void){engine_pending=0;++reset_count;return 0;}
int gfx_engine_surf_set_space(int slot,uint32_t base,uint32_t w,uint32_t h,uint32_t stride,uint32_t space,uint32_t format)
{(void)slot;(void)base;(void)w;(void)h;(void)stride;(void)space;(void)format;return 0;}
void sh_world_tick(uint32_t keys,uint32_t *fail)
{(void)fail;logic_keys=keys;++logic_ticks;clock_ms+=2;}
uint32_t sh_score(void){return logic_keys;}
uint32_t sh_shield(void){return 1;}
uint32_t sh_ticks(void){return logic_ticks;}
void sh_world_ranges_use(unsigned i){range_index=i;}
uint8_t *sh_world_ranges(void){return ranges[range_index];}
int sh_world_ranges_commit(uint32_t count){CHECK(count>0);stable();return 0;}
void sh_world_cache_bind(void *p,uint32_t bytes)
{(void)bytes;if(!p)CHECK(!engine_pending);}
void r4_flight_cache_reset(void){}
uint32_t sh_world_weapon_start(void){return worlds[(prepared-1u)%6u];}
uint32_t sh_world_emit(const fx16_mat4 *proj,const rast_viewport *vp,rast_record *out,uint32_t cap)
{
    (void)proj;(void)vp;stable();
    if(fail_emit)return UINT32_MAX;
    uint32_t gen=prepared++,count=worlds[gen%6u]+guns[gen%6u];CHECK(count<=cap);
    keys_by_frame[gen]=logic_keys;
    for(unsigned i=0;i<count;++i) {
        memset(out+i,0,sizeof *out);rast_put32(out+i,80,gen+1u);rast_put32(out+i,84,logic_keys);
        rast_put16(out+i,72,(uint16_t)(10u+i));rast_put16(out+i,74,(uint16_t)(20u+i));
        rast_put16(out+i,76,(uint16_t)(16u*i));rast_put16(out+i,78,(uint16_t)(16u*i+2u));
    }
    memset(ranges[range_index],0xFF,sizeof ranges[0]);clock_ms+=40;
    stable();return count;
}
static void setup(void)
{
    memset(ram,0,sizeof ram);memset(&s_out,0,sizeof s_out);memset(s_pipe_buf,0,sizeof s_pipe_buf);
    clock_ms=field=swaps=prepared=presented=logic_keys=logic_ticks=0;
    engine_pending=display_pending=fail_emit=fail_wait=reset_count=0;
    front=1;rendered[0]=rendered[1]=-1;
    s_fail=s_time_count=s_window_late=s_window_over=s_window_exact=s_render_n=0;
    s_anchor=s_swaps0=s_render_sum=s_pipe_lag_count=s_pipe_lag_sum=s_pipe_lag_max=0;
    s_pipe_lag_min=UINT32_MAX;s_pipe_on=1;s_scene=3;s_hw=1;s_ladder=1;
    s_pipe_cur=s_pipe_valid=0;s_pipe_prepared=s_pipe_presented=0;
    s_record_capacity=R4_REC_MAX;s_raster_frame=(rast_frame){640,480};
    uint8_t *shared=board_region(REGION_SHARED).ptr;
    s_pipe_rec[0]=(rast_record *)shared;s_pipe_rec[1]=(rast_record *)(shared+R4_REC_B_OFF);
    s_pipe_sub[0]=shared+R4_SUB_OFF;s_pipe_sub[1]=shared+R4_SUB_B_OFF;
    s_pipe_gun_sub[0]=shared+R4_SH_GUN_SUB_OFF;s_pipe_gun_sub[1]=shared+R4_GUN_SUB_B_OFF;
    s_hooks.tex_hotset_restore=NULL;s_log=NULL;s_replay_on=0;
}
int main(void)
{
    gfx_surface_table tab={0};gfx_mem mem={0};r4_out out;
    setup();
    for(unsigned call=0;call<19;++call) {
        CHECK(!r4_mode_frame_pipe(&tab,&mem,(int)(call&1u),100u+call,&out));
        CHECK(!engine_pending && !display_pending);
        if(!call)CHECK(out.presented_fb==-1 && out.frames==0 && presented==0);
        else {
            CHECK(out.presented_fb==(int)((call-1u)&1u));CHECK(out.frames==call && presented==call);
            CHECK(out.score==99u+call && out.ticks==call);
            CHECK(s_pipe_prepared-s_pipe_presented==1u);
        }
    }
    r4_mode_leave();CHECK(!engine_pending && !s_pipe_on);
    puts("SH PIPE PASS A/B records/bins, three submission states, present-after-draw, prior-input snapshot, prime, leave");
    setup();CHECK(!r4_mode_frame_pipe(&tab,&mem,0,1,&out));fail_emit=1;
    CHECK(r4_mode_frame_pipe(&tab,&mem,1,2,&out)==-1 && engine_pending);
    r4_mode_leave();CHECK(!engine_pending && !s_pipe_on && presented==0);
    puts("SH PIPE PASS preparation failure drains before cache release");
    setup();CHECK(!r4_mode_frame_pipe(&tab,&mem,0,1,&out));fail_wait=1;
    CHECK(r4_mode_frame_pipe(&tab,&mem,1,2,&out)==-1);
    CHECK(reset_count && !engine_pending && presented==0);
    fail_wait=0;r4_mode_leave();CHECK(!s_pipe_on);
    puts("SH PIPE PASS engine failure resets and never presents a failed frame");
    return 0;
}
