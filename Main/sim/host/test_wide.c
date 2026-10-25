/* Software-canvas contract: analytic rectangle coverage beyond x=639,
   atomic rejection, stride canaries, aspect-correct projection and pure world
   rendering. Legacy R1 tests continue to exercise the fixed hardware ABI. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gfx/rast_ref.h"
#include "r4.h"
#include "r4_scene.h"
#include "r4_geometry.h"
#include "r4_cup.h"

#define need(test, name) do { if(!(test)) { \
    fprintf(stderr,"WIDE CHECK FAIL %s line %d\n",name,__LINE__);exit(1); } } while(0)

static void rectangle(unsigned width)
{
    const rast_frame frame={width,480};
    const rast_viewport vp={0,0,(uint16_t)width,480};
    rast_vertex vertices[4]={
        {600*16,40*16,1234,0,0,0x00abcdef},
        {(int32_t)(width+64)*16,40*16,1234,0,0,0x00abcdef},
        {(int32_t)(width+64)*16,72*16,1234,0,0,0x00abcdef},
        {600*16,72*16,1234,0,0,0x00abcdef}};
    rast_record records[12],before[12];
    memset(records,0xa5,sizeof records);
    memcpy(before,records,sizeof records);
    rast_frame bad={RAST_FRAME_MAX_WIDTH+1u,480};
    need(rast_setup_polygon_frame(vertices,4,&vp,&bad,RAST_VCOLOR_EN,records,12,0)==-1,
         "invalid canvas rejected");
    need(!memcmp(before,records,sizeof records),"setup atomic rejection");
    int n=rast_setup_polygon_frame(vertices,4,&vp,&frame,RAST_VCOLOR_EN,records,12,0);
    need(n>0 && n<=12,"rectangle setup");
    for(int i=0;i<n;++i)need(rast_record_valid_frame(&records[i],&frame),"frame record valid");
    unsigned stride=width+7u;
    size_t count=(size_t)stride*480u;
    uint32_t *memory=malloc((count+16u)*sizeof *memory),*pixels=memory+8;
    uint16_t *zmemory=malloc(((size_t)stride*16u+16u)*sizeof *zmemory),*depth=zmemory+8;
    need(memory && zmemory,"allocation");
    for(size_t i=0;i<count+16u;++i)memory[i]=0x12345678;
    for(size_t i=0;i<(size_t)stride*16u+16u;++i)zmemory[i]=0x7654;
    for(unsigned s=0;s<30;++s) {
        for(size_t i=0;i<(size_t)stride*16u;++i)depth[i]=65535;
        rast_target target={pixels+(size_t)s*16u*stride,depth,stride,s,0,0,0,0,0};
        need(!rast_ref_run_frame(records,(unsigned)n,&target,&frame),"rectangle raster");
        for(unsigned y=0;y<16;++y)for(unsigned x=0;x<stride;++x) {
            unsigned yy=s*16u+y;
            int inside=x>=600 && x<width && yy>=40 && yy<72;
            need(depth[(size_t)y*stride+x]==(inside?1234u:65535u),"depth coverage");
        }
    }
    for(unsigned y=0;y<480;++y)for(unsigned x=0;x<stride;++x) {
        int inside=x>=600 && x<width && y>=40 && y<72;
        need(pixels[(size_t)y*stride+x]==(inside?0x00abcdefu:0x12345678u),"coverage");
    }
    if(width>640)need(!rast_record_valid(&records[n-1]),"hardware rejects wide record");
    for(unsigned i=0;i<8;++i) {
        need(memory[i]==0x12345678 && memory[count+8u+i]==0x12345678,"color canary");
        need(zmemory[i]==0x7654 && zmemory[(size_t)stride*16u+8u+i]==0x7654,"depth canary");
    }
    uint8_t bins[2048],bins_before[2048];memset(bins,0x5a,sizeof bins);memcpy(bins_before,bins,sizeof bins);
    need(rast_bin_frame(records,(unsigned)n,bins,1,&frame)==-2,"bin capacity rejection");
    need(!memcmp(bins,bins_before,sizeof bins),"bin atomic rejection");
    int bytes=rast_bin_frame(records,(unsigned)n,bins,sizeof bins,&frame);
    need(bytes>120,"bin result");
    unsigned at=0;
    for(unsigned s=0;s<30;++s) {
        unsigned hits=0,c=bins[at]|(unsigned)bins[at+1]*256u;at+=4;
        for(int i=0;i<n;++i)if(rast_get16(&records[i],76)/16u<=s && rast_get16(&records[i],78)/16u>=s) {
            need(hits<c && (bins[at+2u*hits]|(unsigned)bins[at+2u*hits+1u]*256u)==(unsigned)i,
                 "bin independent ordering");++hits;
        }
        need(hits==c,"bin independent counts");at+=2u*c;
    }
    need(at==(unsigned)bytes,"bin exact size");
    /* A valid prefix must not write before a later invalid record is rejected. */
    rast_record damaged[12];memcpy(damaged,records,sizeof damaged);
    rast_put16(&damaged[n-1],RAST_O_BBOX+2,(uint16_t)width);
    for(size_t i=0;i<count;++i)pixels[i]=0x12345678;
    rast_target target={pixels,depth,stride,0,0,0,0,0,0};
    need(rast_ref_run_frame(damaged,(unsigned)n,&target,&frame)==-1,"bad record rejected");
    for(size_t i=0;i<count;++i)need(pixels[i]==0x12345678,"raster atomic rejection");
    free(zmemory);free(memory);
    printf("WIDE RECT PASS %u clipped coverage/depth/bin/canaries/atomic\n",width);
}

static void scene(unsigned width)
{
    const rast_frame frame={width,480};
    const rast_viewport vp={0,0,(uint16_t)(width-160u),480};
    fx16_mat4 projection;
    fx16_t aspect=fx16_div((fx16_t)vp.w*FX16_ONE,(fx16_t)vp.h*FX16_ONE);
    need(!fx16_perspective(&projection,R4_FOV_BAM,aspect,R4_NEAR_Z,R4_FAR_Z),"perspective");
    rast_view_vertex points[4]={
        {-FX16_ONE/2,0,-4*FX16_ONE,0,0,0}, {FX16_ONE/2,0,-4*FX16_ONE,0,0,0},
        {0,-FX16_ONE/2,-4*FX16_ONE,0,0,0}, {0,FX16_ONE/2,-4*FX16_ONE,0,0,0}};
    rast_vertex projected[4];
    need(!r4_project_inside(points,4,&projection,R4_NEAR_Z,R4_FAR_Z,&vp,projected),"project square");
    int dx=projected[1].x-projected[0].x,dy=projected[2].y-projected[3].y;
    need(abs(dx-dy)<=2,"physical square aspect");
    rast_record records[512];
    r4_world world,expected;
    r4_world_init(&world,R4_SEED);r4_world_init(&expected,R4_SEED);
    world.ladder=expected.ladder=2;
    for(unsigned i=0;i<60;++i) {
        uint32_t fail=0,keys=i<20?1u:i<40?8u:16u;
        r4_world_tick(&world,keys,&fail);r4_world_tick(&expected,keys,&fail);
        need(!fail,"world tick");
        uint32_t n=r4_world_emit(&world,&projection,&vp,2,records,512,0);
        need(n && n<=512,"wide world emission");
        need(!memcmp(&world,&expected,sizeof world),"render preserves world/replay state");
        for(unsigned k=0;k<n;++k)need(rast_record_valid_frame(&records[k],&frame),"world record bounds");
    }
    int n=r4_cup_emit_view(30,&vp,records,512);
    need(n>0 && n<=512,"wide cup emission");
    for(int k=0;k<n;++k)need(rast_record_valid_frame(&records[k],&frame),"cup bounds");
    printf("WIDE SCENE PASS %u square projection/world invariance/cup\n",width);
}

int main(void)
{
    const unsigned widths[]={640,768,854,1120,1707,1792};
    for(unsigned i=0;i<sizeof widths/sizeof widths[0];++i) {rectangle(widths[i]);scene(widths[i]);}
    puts("PC WIDE PASS");return 0;
}
