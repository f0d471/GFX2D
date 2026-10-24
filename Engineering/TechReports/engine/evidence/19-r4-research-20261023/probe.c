/* R4 research fixture only. Uses unchanged production libraries and BSP. */
#include <stdint.h>
#include <string.h>
#include "board.h"
#include "core/crc32.h"
#include "core/format.h"
#include "gfx/rast_ref.h"
#ifdef __riscv
#include "gfx_engine.h"
#endif

static fx16_mat4 projection, model;
static rast_record *records;
static uint8_t *sub;
static uint32_t color[640*16];
static uint16_t depth[640*16];
static uint32_t fails;
static void row(const char *kind,const uint32_t *v,unsigned n)
{
    char s[384];core_out o;core_out_init(&o,s,sizeof s);core_out_str(&o,kind);
    for(unsigned i=0;i<n;++i){core_out_ch(&o,' ');core_out_u32(&o,v[i]);}
    core_out_ch(&o,'\n');board_puts(s);
}
/* No near-plane intersection: small triangles on a grid, or full-frame bbox.
   The matrix stage deliberately uses the public general matrix function. */
static int generate(unsigned count,unsigned dense)
{
    for(unsigned i=0;i<count;++i){
        rast_record tmp[RAST_MAX_FAN];int n;
        if(dense){
            rast_vertex v[3]={{0,0,20000,0,0,0xff40a0e0},
                {639*16,0,20000,0,0,0xff40a0e0},
                {0,479*16,20000,0,0,0xff40a0e0}};
            n=rast_setup_polygon(v,3,RAST_VCOLOR_EN,tmp,RAST_MAX_FAN,0);
        }else{
            int32_t x=-50000+(int32_t)(i%8)*14000;
            int32_t y=40000-(int32_t)((i/8)%8)*11000;
            rast_view_vertex v[3]={{x,y,-2*65536,0,0,0xff40a0e0},
                {x+6000,y,-2*65536,0,0,0xff40a0e0},
                {x,y-6000,-2*65536,0,0,0xff40a0e0}};
            for(unsigned k=0;k<3;++k){
                fx16_vec4 in={{v[k].x,v[k].y,v[k].z,65536}},out;
                fx16_mat_vec(&model,&in,&out);
                v[k].x=out.v[0];v[k].y=out.v[1];v[k].z=out.v[2];
            }
            n=rast_setup_view(v,&projection,65536,9*65536,
                              RAST_VCOLOR_EN,tmp,RAST_MAX_FAN,0);
        }
        if(n!=1)return -1;
        records[i]=tmp[0];
    }
    return rast_bin(records,count,sub,32768);
}
static void workload(unsigned count,unsigned dense,int bytes)
{
    uint32_t refs=0,bbox=0;
    for(unsigned i=0;i<count;++i){
        unsigned y0=rast_get16(records+i,76),y1=rast_get16(records+i,78);
        refs+=y1/16-y0/16+1;
        bbox+=(rast_get16(records+i,74)-rast_get16(records+i,72)+1)*(y1-y0+1);
    }
    uint32_t v[]={dense,count,(uint32_t)bytes,refs,bbox,
        crc32_ieee(sub,(uint32_t)bytes),crc32_ieee(records,count*128)};
    row("WORK",v,7);
}
static uint32_t frame_crc(int dst)
{
    crc32_ctx c;crc32_begin(&c);
    for(unsigned y=0;y<480;++y)crc32_update(&c,board_fb(dst)+y*1024,2560);
    return crc32_end(&c);
}
#ifdef __riscv
static int submit(unsigned count,unsigned bytes,int dst,uint32_t *hw,uint32_t *rast)
{
    uint32_t code=~0u,index=0,tag=0,id=0,caps=0,refs=0;
    gfx_engine_raster p={g_mem_regions[REGION_SHARED].offset,65536,
        g_mem_regions[REGION_SHARED].offset+65536,32768,bytes,count,0,0};
    if(gfx_engine_surf_set(1,g_mem_regions[REGION_FB0+dst].offset,640,480,12,0))return -1;
    uint32_t t=board_cycles();
    int r=gfx_engine_submit_raster(g_mem_regions[REGION_CMDLIST_A].offset,0,1,&p);
    if(!r)r=gfx_engine_wait(&code,&index,&tag);
    *hw=board_cycles()-t;
    if(gfx_engine_raster_info(&id,&caps,rast,&refs) || refs!=(bytes-120)/2 || code)return -1;
    return r;
}
#else
static uint32_t expected(unsigned count)
{
    crc32_ctx c;crc32_begin(&c);
    for(unsigned s=0;s<30;++s){
        memset(color,0,sizeof color);memset(depth,255,sizeof depth);
        rast_target t={color,depth,640,s,0,0,0,0,0};
        if(rast_ref_run(records,count,&t))++fails;
        for(unsigned x=0;x<8;++x)color[8*640+x]=0x00204060;
        crc32_update(&c,color,sizeof color);
    }
    return crc32_end(&c);
}
#endif
int main(void)
{
    board_init();
    records=board_region(REGION_SHARED).ptr;sub=(uint8_t *)records+65536;
    if(fx16_perspective(&projection,16384,87381,65536,9*65536))++fails;
    fx16_identity(&model);
#ifdef __riscv
    (void)color;(void)depth;(void)frame_crc;
    uint32_t t=board_cycles();while((uint32_t)(board_cycles()-t)<2*BOARD_HZ){}
    board_puts("R4_RESEARCH v1 O2 production libraries; synthetic fixtures\n");
    uint32_t caps=0;gfx_engine_caps_read(&caps);row("CAPS",&caps,1);
    if(caps!=0xd46f)++fails;
    uint32_t *bg=board_region(REGION_OFFSCREEN).ptr;
    for(unsigned i=0;i<480*1024;++i)bg[i]=board_fb(0)[i]=board_fb(1)[i]=0;
    uint32_t *list=board_region(REGION_CMDLIST_A).ptr;memset(list,0,1048576);
    for(unsigned s=0;s<30;++s){
        list[s]=1;uint32_t *c=(uint32_t *)((uint8_t *)list+(s+1)*32768);
        c[0]=1u|(1u<<20);c[2]=8u<<16;c[3]=(1u<<16)|8u;c[4]=0x00204060;
    }
    if(gfx_engine_soft_reset() || gfx_engine_surf_set(0,g_mem_regions[REGION_OFFSCREEN].offset,
        640,480,12,0))++fails;
    board_present(0);board_wait_frame();if(board_display_stats_clear())++fails;
#else
    (void)frame_crc;
#endif
    const unsigned sizes[]={1,16,32,64,396};
    for(unsigned dense=0;dense<2;++dense)for(unsigned j=0;j<5;++j){
        unsigned count=sizes[j];if(dense && count>32)continue;
        for(unsigned rep=0;rep<3;++rep){
            uint32_t t0=board_cycles();int bytes=generate(count,dense);
            uint32_t cpu=board_cycles()-t0;if(bytes<0){++fails;continue;}
            if(!rep)workload(count,dense,bytes);
#ifdef __riscv
            uint32_t hw=0,ras=0;
            if(submit(count,(unsigned)bytes,1,&hw,&ras))++fails;
            uint32_t v[]={dense,count,rep,cpu,hw,ras};row("SAMPLE",v,6);
            if(rep==2){board_dcache_invalidate_range(board_fb(1),FB_BYTES);
                uint32_t c[]={dense,count,frame_crc(1)};row("CRC",c,3);}
#else
            (void)cpu;
            if(rep==0){uint32_t c[]={dense,count,expected(count)};row("CRC",c,3);}
#endif
        }
    }
#ifdef __riscv
    /* 120 frames only: a cadence probe, not the R4 60-second acceptance. */
    static uint32_t data[120][5];
    board_present(0);uint32_t anchor=board_frame_count();
    for(unsigned f=0;f<120;++f){
        int dst=(int)((f+1)&1);uint32_t t0=board_cycles(),hw=0,ras=0,ticket=0;
        int bytes=generate(32,0);
        if(bytes<0 || submit(32,(unsigned)bytes,dst,&hw,&ras))++fails;
        uint32_t render=board_cycles()-t0,due=anchor+2*(f+1);
        while((int32_t)(board_frame_count()-(due-1))<0){}
        if(board_present_request(dst,&ticket) || board_present_wait(ticket))++fails;
        data[f][0]=f;data[f][1]=render;data[f][2]=due;
        data[f][3]=board_frame_count();data[f][4]=board_swap_count();
    }
    for(unsigned f=0;f<120;++f)row("CADENCE",data[f],5);
    uint32_t health[5]={0};
    if(board_display_diag(health,health+1,health+2,health+3))++fails;
    health[4]=board_display_response_errors();row("HEALTH",health,5);
    if(health[2] || health[3] || health[4])++fails;
#endif
    row("fail",&fails,1);board_puts("done\n");return fails?1:0;
}
