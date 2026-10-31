/* Frozen 1d85590 record/pixel fixture + cached/uncached differential checks. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "r4.h"
#include "r4_scene.h"
#include "core/crc32.h"
#include "gfx/rast_ref.h"

static rast_record records[512];
static uint32_t colour[1792u*480u];
#ifndef FLIGHT_BASELINE
static rast_record reference[512];
static uint32_t row_a[1794],row_b[1794];
#endif
static uint16_t depth[1792u*16u];
static unsigned failures;
static void need(int pass,const char *message)
{if(!pass){fprintf(stderr,"FLIGHT FAIL %s\n",message);++failures;}}

static uint32_t pixels_crc(unsigned width,unsigned count)
{
    const rast_frame frame={width,480};
    memset(colour,0,(size_t)width*480u*sizeof *colour);
    for(unsigned strip=0;strip<30;++strip) {
        memset(depth,255,(size_t)width*16u*sizeof *depth);
        rast_target target={.color=colour+(size_t)strip*16u*width,.depth=depth,
            .stride=width,.strip=strip};
        need(rast_ref_run_frame(records,count,&target,&frame)==0,"reference raster");
    }
    return crc32_ieee(colour,width*480u*sizeof *colour);
}

static uint32_t keys_at(unsigned frame)
{
    static const unsigned keys[]={0,1,8,2,4,9,6,0};
    return keys[(frame/41u)%8u];
}

static void sequence(unsigned width,unsigned zone,unsigned frames)
{
    r4_world world;
    fx16_mat4 projection;
    const rast_viewport vp={0,0,(uint16_t)width,480};
    need(!fx16_perspective(&projection,R4_FOV_BAM,fx16_div((int)width*R4_ONE,480*R4_ONE),
                           R4_NEAR_Z,R4_FAR_Z),"projection");
    projection.v[6]=-R4_ONE/8;
    r4_world_init(&world,R4_SEED);world.scene=2;world.ladder=1;world.d=zone*240u*R4_ONE;
#ifndef FLIGHT_BASELINE
    r4_flight_prewarm(&world,&projection,&vp);
#endif
    uint32_t fail=0;
    for(unsigned i=1;i<=frames;++i) {
        r4_world_tick(&world,keys_at(i),&fail);
        r4_world expected=world;
        uint32_t count=r4_world_emit(&world,&projection,&vp,1,records,512,NULL);
        need(count>0 && count<=512,"record budget");
        if(count>512)return;
        need(!memcmp(&world,&expected,sizeof world),"drawing mutated world");
#ifndef FLIGHT_BASELINE
        uint32_t nr=r4_flight_reference(&world,&projection,&vp,reference,512);
        need(count==nr && !memcmp(records,reference,count*sizeof *records),"cache record stream");
#endif
        if(i<=8 || !(i%60u) || i+8u>frames) {
            uint32_t image=(i==60 || i==frames)?pixels_crc(width,count):0;
            printf("flight %u %u %u %u %08x %08x %08x\n",width,zone,i,count,
                crc32_ieee(records,count*sizeof *records),crc32_ieee(&world,sizeof world),image);
        }
    }
#ifndef FLIGHT_BASELINE
    r4_flight_cache_stats cache=r4_flight_cache_status();
    need(cache.cold==1 && !cache.late && !cache.fallback,"continuous prewarm deadline");
    /* A viewport change at the same distance must not reuse projected records. */
    rast_viewport changed=vp;changed.x=8;changed.w-=16;
    uint32_t n=r4_world_emit(&world,&projection,&changed,1,records,512,NULL);
    uint32_t nr=r4_flight_reference(&world,&projection,&changed,reference,512);
    need(n==nr && n<=512 && !memcmp(records,reference,n*sizeof *records),"viewport cache invalidation");
    projection.v[6]+=R4_ONE/20;
    n=r4_world_emit(&world,&projection,&changed,1,records,512,NULL);
    nr=r4_flight_reference(&world,&projection,&changed,reference,512);
    need(n==nr && n<=512 && !memcmp(records,reference,n*sizeof *records),"projection cache invalidation");
    const unsigned caps[]={0,299,300,320,511};
    for(unsigned i=0;i<sizeof caps/sizeof caps[0];++i) {
        n=r4_world_emit(&world,&projection,&changed,1,records,caps[i],NULL);
        nr=r4_flight_reference(&world,&projection,&changed,reference,caps[i]);
        need(n==nr && (n==UINT32_MAX || (n<=caps[i] && !memcmp(records,reference,n*sizeof *records))),
             "capacity fallback");
    }
#endif
    need(!fail,"world failure");
}

#ifndef FLIGHT_BASELINE
static void background(void)
{
    const unsigned widths[]={640,854,1792};
    for(unsigned zone=0;zone<4;++zone)for(unsigned y=0;y<480;++y)
        for(unsigned i=0;i<3;++i) {
            unsigned width=widths[i];
            for(unsigned x=0;x<1794;++x)row_a[x]=row_b[x]=0xdeadbeefu;
            r4_flight_bg_test(row_a+1,width,zone,y,0);
            r4_flight_bg_test(row_b+1,width,zone,y,1);
            need(!memcmp(row_a,row_b,sizeof row_a),"cached background pixels/padding");
        }
    need(r4_scene_identity(0)==R4_SCENE_ID && r4_scene_identity(1)==R4_SCENE_ID+1u &&
         r4_scene_identity(2)==R4_FLIGHT_SCENE_ID,"separate flight replay identity");
}
#endif

int main(int argc,char **argv)
{
    unsigned frames=argc>1?(unsigned)strtoul(argv[1],NULL,10):240u;
    if(!frames || frames>1801u)return 2;
    const unsigned widths[]={640,854,1792};
    for(unsigned i=0;i<3;++i)for(unsigned zone=0;zone<4;++zone)sequence(widths[i],zone,frames);
#ifndef FLIGHT_BASELINE
    background();
#endif
    if(failures)return 1;
    puts("FLIGHT PASS");return 0;
}
