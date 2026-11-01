/* RFL3 motion/anchoring oracles and uncached fixture + cached byte comparison. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "r4.h"
#include "r4_scene.h"
#include "r4_geometry.h"
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

static void motion(void)
{
    r4_world w,l,r;
    uint32_t fail=0;
    r4_world_init(&w,R4_SEED);w.scene=2;w.ship_y=-R4_ONE/4;
    r4_world_tick(&w,8,&fail);
    int32_t step=2*R4_ONE/30;
    need(w.manual && w.flight.vx==step/4 && w.cam_x==step/4,"control acceleration");
    need(w.bank<0 && w.strafe>0,"right turn attitude sign");
    for(unsigned i=0;i<4;++i) {
        int32_t vx=w.flight.vx;r4_world_tick(&w,8,&fail);
        need(w.flight.vx>vx && w.flight.vx<step,"acceleration ramp");
    }
    int32_t x=w.cam_x,vx=w.flight.vx;
    r4_world_tick(&w,0,&fail);
    need(w.cam_x>x && w.flight.vx>0 && w.flight.vx<vx && w.bank<0,"release damping");
    for(unsigned i=0;i<80;++i)r4_world_tick(&w,0,&fail);
    need(!w.flight.vx && !w.bank && !w.strafe && !w.pitch,"settled neutral attitude");
    r4_world_init(&l,R4_SEED);l.scene=2;l.ship_y=-R4_ONE/4;r=l;
    for(unsigned i=0;i<60;++i) {
        r4_world_tick(&l,i<10?4:0,&fail);r4_world_tick(&r,i<10?8:0,&fail);
        need(l.cam_x==-r.cam_x && l.flight.vx==-r.flight.vx && l.bank==-r.bank,"left right symmetry");
        need(l.d==(i+1u)*(4u*R4_ONE/30u) && l.d==r.d,"fixed forward speed");
    }
    r4_world_init(&w,R4_SEED);w.scene=2;w.ship_y=-R4_ONE/4;
    r4_world_tick(&w,1,&fail);need(w.pitch>0 && w.flight.vy>0,"nose-up response");
    for(unsigned i=0;i<80;++i)r4_world_tick(&w,8,&fail);
    need(w.cam_x==R4_ONE && !w.flight.vx,"lateral boundary stop");
    r4_world_tick(&w,0,&fail);need(w.cam_x==R4_ONE && !w.flight.vx,"no boundary creep");
    r4_world_tick(&w,4,&fail);need(w.cam_x<R4_ONE && w.flight.vx<0,"boundary reversal");
    for(unsigned i=0;i<160;++i) {
        uint32_t m=w.d>>16;r4_flight_state camera=w.flight;
        r4_world_tick(&w,i%40u<20u?4u:8u,&fail);
        if((w.d>>16)==m)need(w.flight.camera_x==camera.camera_x && w.flight.camera_y==camera.camera_y &&
            w.flight.next_camera_x==camera.next_camera_x && w.flight.next_camera_y==camera.next_camera_y,"camera metre cadence");
        need(w.flight.camera_x<=R4_ONE/8 && w.flight.camera_x>=-R4_ONE/8,"camera follow limit");
    }
    need(!fail,"motion world health");
}

static void anchors(void)
{
    const unsigned metres[]={0,1,8,9,46,47,78,87,88,95,96,239,240,65534};
    for(unsigned k=0;k<sizeof metres/sizeof metres[0];++k) {
        unsigned m=metres[k];
        for(unsigned kind=0;kind<2;++kind) {
            int period=kind?96:88,behind=kind?16:10;
            int a=r4_flight_anchor_test(m*R4_ONE,31,period,behind);
            int b=r4_flight_anchor_test((m+1u)*R4_ONE,31,period,behind);
            need(a>=-behind && a<period-behind && b>=-behind && b<period-behind,"anchor window");
            need(b==a-1 || (a==-behind && b==period-behind-1),"world anchor direction");
        }
        for(unsigned row=0;row<=10;++row) {
            int a=r4_flight_grid_test(m*R4_ONE,row),b=r4_flight_grid_test((m+1u)*R4_ONE,row);
            need(((int)m+a-1)%9==0,"mesh fixed world grid");
            need(b==a-1 || ((m+1u)%9u==0u && b==a+8),"mesh grid advance");
            int x=R4_ONE/3;
            need(r4_flight_ground_test(x,m*R4_ONE,30)==
                 r4_flight_ground_test(x,(m+1u)*R4_ONE,29),"stationary world ground");
        }
    }
    /* Inside one actual mesh triangle the height is planar. */
    const int32_t dx=40*R4_ONE/7,x0=-20*R4_ONE+3*dx;
    int32_t a=r4_flight_ground_test(x0,0,1),b=r4_flight_ground_test(x0,0,10);
    int32_t at=r4_flight_ground_test(x0,0,4);
    need(llabs((long long)at-((long long)2*a+b)/3)<=3,"planar mesh edge height");
    b=r4_flight_ground_test(x0+dx,0,1);
    int32_t c=r4_flight_ground_test(x0+dx,0,10);
    at=r4_flight_ground_test(x0+dx/2,0,4);
    need(llabs((long long)at-((long long)3*a+b+2LL*c)/6)<=4,"planar mesh triangle height");
}

static void clipped_surface(void)
{
    fx16_mat4 p;
    need(!fx16_perspective(&p,R4_FOV_BAM,4*R4_ONE/3,R4_NEAR_Z,R4_FAR_Z),"surface projection");
    p.v[6]=-R4_ONE/8;
    const rast_viewport vp={0,0,640,480};const rast_frame frame={640,480};
    unsigned samples=0;
    r4_world w;r4_world_init(&w,R4_SEED);w.scene=2;
    for(unsigned m=0;m<90;++m) {
        w.d=m*R4_ONE;
        unsigned n=r4_flight_terrain_test(&w,&p,&vp,records,512);
        need(n>0 && n<=512,"clipped terrain budget");
        if(!n || n>512)continue;
        for(int lane=-1;lane<=1;++lane) {
            int32_t x=lane*R4_ONE/2;
            /* Integer query at exactly 2m preserves the accepted collision
             * convention. Its projected depth must lie on the drawn surface. */
            rast_view_vertex point={x,r4_flight_ground_test(x,w.d,2),-2*R4_ONE,0,0,0};
            rast_vertex projected;
            need(!r4_project_inside(&point,1,&p,R4_NEAR_Z,R4_FAR_Z,&vp,&projected),"surface query projection");
            int px=projected.x/16,py=projected.y/16;
            if(px<0 || px>=640 || py<0 || py>=480)continue;
            ++samples;
            memset(depth,255,640u*16u*sizeof *depth);
            rast_target target={.color=colour,.depth=depth,.stride=640,.strip=(unsigned)py/16u};
            need(!rast_ref_run_frame(records,n,&target,&frame),"surface depth raster");
            int actual=depth[(py%16)*640+px];
            need(abs(actual-(int)projected.z)<=200,"near-clipped surface matches collision");
        }
    }
    need(samples>=60,"surface oracle coverage");
}

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
    world.flight.camera_x+=R4_ONE/64;
    n=r4_world_emit(&world,&projection,&changed,1,records,512,NULL);
    nr=r4_flight_reference(&world,&projection,&changed,reference,512);
    need(n==nr && n<=512 && !memcmp(records,reference,n*sizeof *records),"camera cache invalidation");
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
    motion();anchors();clipped_surface();
    const unsigned widths[]={640,854,1792};
    for(unsigned i=0;i<3;++i)for(unsigned zone=0;zone<4;++zone)sequence(widths[i],zone,frames);
#ifndef FLIGHT_BASELINE
    background();
#endif
    if(failures)return 1;
    puts("FLIGHT PASS");return 0;
}
