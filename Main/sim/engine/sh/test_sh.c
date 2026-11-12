/* Exercise the actual scene state and production raster path, including private
 * collision helpers. No test-only behavior is compiled into the scene. */
#include "../../../sw/apps/game/gfx2dGame/src/sh_scene.c"
#include <stdlib.h>
#include "gfx/rast_ref.h"

#define CHECK(x) do {if(!(x)){fprintf(stderr,"SH FAIL line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static uint32_t fail;
static void division_reuse_checks(void)
{
#ifdef SH_DIVISION_REUSE
    static const uint32_t edges[]={0u,1u,2u,3u,65535u,65536u,0x7fffffffu,0x80000000u,UINT32_MAX};
    for(unsigned i=0;i<sizeof edges/sizeof *edges;++i)
        for(unsigned j=1;j<sizeof edges/sizeof *edges;++j) {
            uint32_t n=edges[i],d=edges[j],r,q=sh_divrem32(n,d,UINT32_MAX/d,&r);
            CHECK(q==n/d && r==n%d);
        }
    uint32_t rng=0x196723u;
    for(unsigned i=0;i<200000u;++i) {
        rng=rng*1664525u+1013904223u;uint32_t d=i<64u?(1u<<(i&31u)):(rng?rng:1u);
        rng=rng*1664525u+1013904223u;uint32_t n=i<64u?d:rng;
        uint32_t r,q=sh_divrem32(n,d,UINT32_MAX/d,&r);
        CHECK(q==n/d && r==n%d && r<d);
        CHECK((uint64_t)q*d+r==n);
    }
    puts("SH DIVREM PASS 200000 u32 quotient/remainder pairs, powers and exact correction");
#endif
}
static void flat_setup_checks(void)
{
    uint32_t rng=17;
    int32_t tie;
    CHECK(sh_gradient20(-1,1u<<21,&tie) && tie==0);
    CHECK(sh_gradient20(1,1u<<21,&tie) && tie==1);
    CHECK(sh_gradient20(-3,1u<<21,&tie) && tie== -1);
    for(unsigned i=0;i<100000u;++i) {
        rng=rng*1664525u+1013904223u;int32_t n=(int32_t)rng;
        rng=rng*1664525u+1013904223u;uint32_t d=1u+rng%((1u<<28)-1u);
        int32_t actual=0;int accepted=sh_gradient20(n,d,&actual);
        int64_t expected=fx16_round_div((int64_t)n*(1u<<20),d);
        CHECK(accepted==(expected> -(INT64_C(1)<<30) && expected<(INT64_C(1)<<30)));
        if(accepted)CHECK(actual==expected);
    }
    for(unsigned i=0;i<10000u;++i) {
        unsigned width=i%2?640u:1792u;
        rast_viewport vp={0,0,(uint16_t)width,480};rast_frame frame={width,480};
        rast_vertex v[3];
        for(unsigned k=0;k<3;++k) {
            rng=rng*1664525u+1013904223u;v[k].x=(int32_t)(rng%(width*16u));
            rng=rng*1664525u+1013904223u;v[k].y=(int32_t)(rng%7680u);
            rng=rng*1664525u+1013904223u;v[k].z=(uint16_t)rng;
            v[k].u=v[k].v=0;v[k].color=0xb32917u;
        }
        rast_record fast,reference[RAST_MAX_FAN];rast_setup_stats stats={0};
        memset(&fast,0x5a,sizeof fast);
        int result=sh_flat_setup(v,&vp,&fast);
        int count=gfx_setup_polygon(v,3,&vp,&frame,RAST_VCOLOR_EN,reference,RAST_MAX_FAN,&stats);
        if(!count && stats.backface){rast_vertex t=v[1];v[1]=v[2];v[2]=t;
            count=gfx_setup_polygon(v,3,&vp,&frame,RAST_VCOLOR_EN,reference,RAST_MAX_FAN,&stats);}
        if(result!= -3){CHECK(result==count);if(count)CHECK(!memcmp(&fast,reference,sizeof fast));}
    }
    puts("SH FLAT SETUP PASS 100000 exact signed divisions and 10000 byte-identical triangles");
}
/* The extended fan path must reproduce the generic clipped polygon record for
 * record: same fan split, same backface skip/retry order, atomic fallback. */
static void flat_fan_checks(void)
{
    uint32_t rng=424243u;
    rast_record fast[RAST_MAX_FAN],reference[RAST_MAX_FAN];
    for(unsigned i=0;i<20000u;++i) {
        const unsigned width=i%2?640u:1792u;
        rast_viewport vp={0,0,(uint16_t)width,480};rast_frame frame={width,480};
        if(i%2){vp.x=13u;vp.y=7u;vp.w=(uint16_t)(width-26u);vp.h=466u;} /* edge-crossing cases */
        const unsigned n=3u+i%2u;
        rast_vertex v[4];
        const unsigned color_mode=i%3u; /* uniform, mixed, mixed */
        for(unsigned k=0;k<n;++k) {
            rng=rng*1664525u+1013904223u;v[k].x=(int32_t)(rng%(width*16u));
            rng=rng*1664525u+1013904223u;v[k].y=(int32_t)(rng%7680u);
            rng=rng*1664525u+1013904223u;v[k].z=(uint16_t)rng;
            v[k].u=v[k].v=0;
            if(!color_mode)v[k].color=0xb32917u;
            else {rng=rng*1664525u+1013904223u;v[k].color=rng&0xffffffu;}
        }
        rast_vertex fv[4];memcpy(fv,v,n*sizeof *v);
        uint32_t fan_backface=0;
        memset(fast,0x5a,sizeof fast);
        int fcount=sh_flat_fan(fv,n,&vp,fast,RAST_MAX_FAN,&fan_backface);
        if(fcount!=-4 && !fcount && fan_backface) {
            rast_vertex t=fv[1];fv[1]=fv[n-1u];fv[n-1u]=t;
            fcount=sh_flat_fan(fv,n,&vp,fast,RAST_MAX_FAN,&fan_backface);
        }
        if(fcount==-4)continue; /* whole polygon fell back; the generic path owns it */
        rast_setup_stats stats={0};
        int count=gfx_setup_polygon(v,n,&vp,&frame,RAST_VCOLOR_EN,reference,RAST_MAX_FAN,&stats);
        if(!count && stats.backface){rast_vertex t=v[1];v[1]=v[n-1u];v[n-1u]=t;
            count=gfx_setup_polygon(v,n,&vp,&frame,RAST_VCOLOR_EN,reference,RAST_MAX_FAN,&stats);}
        CHECK(fcount==count);
        for(unsigned r2=0;r2<(unsigned)(fcount>0?fcount:0);++r2)
            CHECK(!memcmp(&fast[r2],&reference[r2],sizeof fast[r2]));
    }
    puts("SH FLAT FAN PASS 20000 tris/quads byte-identical to the generic clipped path");
}
static void normal_checks(void)
{
    static const int32_t in[][3]={{SH_ONE,0,0},{-1966,0,0},{0,655,0},{0,0,1},
        {3*SH_ONE,4*SH_ONE,0},{-3*SH_ONE,4*SH_ONE,0},{1,1,1},{16*SH_ONE,16*SH_ONE,16*SH_ONE},{0,0,0}};
    static const int32_t expected[][3]={{SH_ONE,0,0},{-SH_ONE,0,0},{0,SH_ONE,0},{0,0,SH_ONE},
        {39322,52429,0},{-39322,52429,0},{37837,37837,37837},{37837,37837,37837},{0,SH_ONE,0}};
    for(unsigned i=0;i<sizeof in/sizeof *in;++i) {
        int32_t v[3];memcpy(v,in[i],sizeof v);sh_normalize(v);
        for(unsigned k=0;k<3;++k)CHECK(v[k]-expected[i][k]>= -16 && v[k]-expected[i][k]<=16);
    }
    puts("SH NORMAL PASS unit axes, tiny vectors, 3-4-5, diagonal and signs");
}
static void tick(uint32_t keys) {sh_world_tick(keys,&fail);CHECK(!fail);}
static void elapsed_checks(void)
{
    sh_world_init(101);sh_world_advance(PLATFORM_KEY_UP,0,&fail);
    for(unsigned i=0;i<20;++i)sh_world_advance(PLATFORM_KEY_UP,50,&fail);
    int32_t x=s_px,z=s_pz;uint32_t ticks=sh_ticks();CHECK(ticks==31);
    sh_world_init(101);sh_world_advance(PLATFORM_KEY_UP,0,&fail);
    for(unsigned i=0;i<60;++i)sh_world_advance(PLATFORM_KEY_UP,i%3?17:16,&fail);
    CHECK(sh_ticks()==ticks && s_px==x && s_pz==z && !fail);
    puts("SH CLOCK PASS 30Hz physics independent of render batches");
}
static void projection_checks(void)
{
#if !SH_DISABLE_VERTEX_CACHE
    uint32_t rng=123;
    for(unsigned i=0;i<50000;++i) {
        rng=rng*1664525u+1013904223u;uint32_t d=R4_NEAR_Z+rng%(R4_FAR_Z-R4_NEAR_Z);
        rng=rng*1664525u+1013904223u;int32_t a=(int32_t)(rng%(40u*SH_ONE))-20*SH_ONE;
        uint32_t reciprocal=UINT32_MAX/d+(UINT32_MAX%d==d-1u);
        CHECK(sh_project_div(a,d,reciprocal)==fx16_div(a,(int32_t)d));
    }
    puts("SH PROJECTION PASS 50000 reciprocal divisions match R1 rounding exactly");
#endif
}
static void empty(void)
{
    sh_world_init(1234);memset(s_enemy,0,sizeof s_enemy);s_spawn=0;
    memset(s_bullet,0,sizeof s_bullet);s_px=s_pz=0;
}
static void enemy(int x,int z)
{s_enemy[0]=(sh_enemy){x, z,0,0,-100*SH_ONE,0,1,0};}
static void enemy_motion_checks(void)
{
    empty();s_px=4*SH_ONE;s_pz=3*SH_ONE;
    s_enemy[0]=(sh_enemy){0,0,0,0,-100*SH_ONE,0,1,0};
    s_enemy[1]=(sh_enemy){SH_ONE/2,0,0,0,-100*SH_ONE,0,1,0};
    s_enemy[2]=(sh_enemy){0,SH_ONE/2,0,0,-100*SH_ONE,0,1,0};
    tick(0);CHECK(!s_enemy[0].x && !s_enemy[0].z && !s_enemy[0].ph && !s_enemy[0].move);
    empty();s_px=27*SH_ONE/10;s_pz=-13*SH_ONE/10;
    enemy(s_px,-245*SH_ONE/100);int32_t old_x=s_enemy[0].x;
    tick(0);CHECK(s_enemy[0].x!=old_x);
    puts("SH AI PASS blocked animation stops and cover tangent makes progress");
}
static void simulation(void)
{
    sh_world_init(42);
    unsigned alive=0;for(unsigned i=0;i<SH_ENEMY_MAX;++i)if(s_enemy[i].alive){
        ++alive;CHECK(s_enemy[i].x>= -5*SH_ONE && s_enemy[i].x<=5*SH_ONE);
        CHECK(s_enemy[i].z>= -4*SH_ONE && s_enemy[i].z<=4*SH_ONE);
    }CHECK(alive==3);
    for(int i=0;i<30;++i)tick(0);
    CHECK(s_ts==SH_TS_MIN);CHECK(s_enemy[0].cool<2*SH_ONE);
    int32_t before=s_pz;tick(PLATFORM_KEY_UP);CHECK(s_pz<before);CHECK(s_ts>SH_TS_MIN);
    empty();enemy(3*SH_ONE,3*SH_ONE);
    tick(PLATFORM_KEY_STRAFE_R|PLATFORM_KEY_RIGHT);CHECK(s_px>0 && s_yaw==32768u);
    empty();enemy(3*SH_ONE,3*SH_ONE);tick(PLATFORM_KEY_RIGHT);CHECK(s_yaw<32768u);
    sh_world_look(10,-10);CHECK(s_pitch>0);CHECK(s_yaw<32768u);
    /* Stop against a wall without letting held movement run world time at x1. */
    empty();enemy(0,-2*SH_ONE);s_px=5*SH_ONE-SH_ONE/5;
    tick(PLATFORM_KEY_STRAFE_R|PLATFORM_KEY_RIGHT);CHECK(s_ts==SH_TS_MIN);
    CHECK(!sh_free(27*SH_ONE/10,-19*SH_ONE/10,SH_ONE/5,0));
    CHECK(sh_segment(-SH_ONE,0,0,SH_ONE,0,0,0,0,0,SH_ONE/10,SH_ONE/10,SH_ONE/10)>0);
    CHECK(sh_segment(-SH_ONE,SH_ONE,0,SH_ONE,SH_ONE,0,0,0,0,SH_ONE/10,SH_ONE/10,SH_ONE/10)<0);
    empty();enemy(0,-2*SH_ONE);tick(PLATFORM_KEY_ACTION);
    CHECK(s_ammo==5 && s_enemy[0].alive); /* projectile travel, no hitscan */
    for(unsigned i=0;i<10 && s_enemy[0].alive;++i){s_ts=SH_ONE;tick(0);}
    CHECK(!s_enemy[0].alive && s_score==100 && s_kills==1);
    for(unsigned i=0;i<80;++i)tick(0);
    unsigned count=0;for(unsigned i=0;i<SH_ENEMY_MAX;++i)count+=s_enemy[i].alive;
    CHECK(count==SH_ENEMY_MAX && s_score==100);
    empty();enemy(2*SH_ONE,-2*SH_ONE);tick(PLATFORM_KEY_ACTION);
    for(int i=0;i<10;++i){s_ts=SH_ONE;tick(0);}CHECK(s_enemy[0].alive && !s_score);
    /* An enemy behind the real 3m column cannot be shot through it. */
    empty();s_px=27*SH_ONE/10;s_pz=0;enemy(s_px,-34*SH_ONE/10);
    tick(PLATFORM_KEY_ACTION);for(int i=0;i<12;++i){s_ts=SH_ONE;tick(0);}
    CHECK(s_enemy[0].alive && !s_score);
    /* A fast round is consumed on impact, but invincibility must preserve play. */
    empty();enemy(3*SH_ONE,3*SH_ONE);s_ts=SH_ONE;
    s_bullet[0]=(sh_bullet){-SH_ONE,SH_EYE,0,2*SH_ONE,0,0,1,0};
    tick(0);CHECK(s_hits==1 && s_shield==1 && !s_bullet[0].alive);
    for(int i=0;i<80;++i)tick(PLATFORM_KEY_UP);
    CHECK(s_pz<0 && s_shield==1);
    s_ammo=2;s_reload=0;tick(PLATFORM_KEY_RESTART);CHECK(s_reload && s_ammo==2);
    for(int i=0;i<27;++i)tick(0);
    CHECK(!s_reload && s_ammo==6);
    empty();enemy(3*SH_ONE,3*SH_ONE);s_ts=SH_ONE;
    s_bullet[0]=(sh_bullet){-SH_ONE,SH_EYE,SH_ONE/3,2*SH_ONE,0,0,1,0};tick(0);CHECK(!s_hits && s_shield==1);
    /* Gravity, rotation and lifetime share the same time scale as position. */
    empty();enemy(3*SH_ONE,3*SH_ONE);
    s_shard[0]=(sh_shard){0,SH_ONE,0,SH_ONE/10,SH_ONE/10,0,30*SH_ONE,0,1};
    int32_t y=s_shard[0].y;tick(0);
    CHECK(s_shard[0].y-y<SH_ONE/100 && s_shard[0].life>29*SH_ONE);
    s_drop[0]=(sh_drop){s_px,s_pz,1};s_ammo=0;tick(PLATFORM_KEY_PICKUP);CHECK(s_ammo==6 && !s_reload && !s_drop[0].alive);
    sh_world_init(77);
    for(unsigned i=0;i<9000;++i) {
        if(i%27==0)for(unsigned e=0;e<SH_ENEMY_MAX;++e)if(s_enemy[e].alive) {
            s_bullet[SH_BULLET_MAX-1]=(sh_bullet){s_enemy[e].x,SH_EYE,s_enemy[e].z,0,0,0,1,1};break;
        }
        if(i%3==0)sh_player_hit();
        tick(i%11==0?PLATFORM_KEY_RESTART:0);
        CHECK(s_shield==1);
    }
    CHECK(s_spawned>250 && s_kills>250 && s_hits>=3000);
    printf("SH ENDLESS PASS ticks=9000 spawned=%u kills=%u protected_hits=%u\n",s_spawned,s_kills,s_hits);
    /* A blocked preferred door must fall back to another entrance. */
    empty();s_px=-32*SH_ONE/10;s_pz=-34*SH_ONE/10;
    CHECK(sh_spawn_enemy() && s_enemy[0].x!=s_px);
    puts("SH GAMEPLAY PASS spawn/time/strafe/aim/projectile/cover/swept-hit/invincible/reload/pickup/shards");
}
static rast_record records[SH_PC_RECORD_CAP+1];
static uint8_t bins[32768];
static uint32_t image[480*1800];
static uint16_t depth_strip[16*640];
static uint32_t cache_memory[SH_CACHE_BYTES/4u+2u];
static rast_record saved_weapon[256];
static rast_record saved_scene[SH_PC_RECORD_CAP];
static void scene_cache_checks(void)
{
    cache_memory[0]=cache_memory[SH_CACHE_BYTES/4u+1u]=0x5a7e31cdu;
    sh_world_cache_bind(cache_memory+1,SH_CACHE_BYTES);sh_world_init(42);
    fx16_mat4 proj;CHECK(!fx16_perspective(&proj,R4_FOV_BAM,4*SH_ONE/3,R4_NEAR_Z,R4_FAR_Z));
    rast_viewport vp={0,0,640,480};uint32_t n=sh_world_emit(&proj,&vp,records,SH_PC_RECORD_CAP);
    CHECK(n!=UINT32_MAX);
#if !SH_DISABLE_SCENE_CACHE
    CHECK(s_room_cache.valid && s_room_cache.count && s_room_cache.count<=1024u);
#endif
    uint32_t split=sh_world_weapon_start(),weapon=n-split;CHECK(weapon && weapon<=256);
    memcpy(saved_weapon,records+split,weapon*sizeof *records);
    for(unsigned i=0;i<5;++i) {
        sh_world_look(7,0);
        n=sh_world_emit(&proj,&vp,records,SH_PC_RECORD_CAP);CHECK(n!=UINT32_MAX);
        split=sh_world_weapon_start();CHECK(n-split==weapon);
        CHECK(!memcmp(saved_weapon,records+split,weapon*sizeof *records));
        uint32_t old_count=n;memcpy(saved_scene,records,n*sizeof *records);
        sh_world_init(42);sh_world_look((int32_t)(i+1)*7,0);
        n=sh_world_emit(&proj,&vp,records,SH_PC_RECORD_CAP);
        CHECK(n==old_count && !memcmp(saved_scene,records,n*sizeof *records));
    }
    CHECK(cache_memory[0]==0x5a7e31cdu && cache_memory[SH_CACHE_BYTES/4u+1u]==0x5a7e31cdu);
    sh_world_cache_bind(NULL,0);
    puts("SH CACHE PASS camera-independent weapon records, invalidation and arena guards");
}
static uint32_t hash(uint32_t h,const void *data,size_t n)
{const uint8_t *p=data;while(n--)h=(h^*p++)*16777619u;return h;}
/* Frozen twelve-triangle construction: an independent oracle for ring reuse. */
static void shadow_reference(int32_t x,int32_t z,int32_t rx,int32_t rz,
                              const rast_viewport *vp,rast_record **out,uint32_t *cap)
{
    const int32_t y=SH_ONE/64;
    for(unsigned i=0;i<12;++i) {
        uint16_t a=(uint16_t)(i*65536u/12u),b=(uint16_t)((i+1)*65536u/12u);
        int32_t x1=x+sh_mul(sh_cos(a),rx),z1=z+sh_mul(sh_sin(a),rz);
        int32_t x2=x+sh_mul(sh_cos(b),rx),z2=z+sh_mul(sh_sin(b),rz);
        int64_t dx=x-s_px,dz=z-s_pz;
        uint32_t edge=sh_fog(dx*dx+(int64_t)SH_EYE*SH_EYE+dz*dz,0xedf0f3u);
        rast_view_vertex v[3]={{x,y,z,0,0,sh_fog(dx*dx+dz*dz,0x8b929cu)},
            {x1,y,z1,0,0,edge},{x2,y,z2,0,0,edge}};
        sh_poly(v,3,vp,out,cap);
    }
}
static void shadow_reuse_checks(void)
{
    static rast_record fast[160],reference[160];
    uint32_t rng=0x72316u;
    sh_world_cache_bind(NULL,0);sh_world_init(7);
    for(unsigned pose=0;pose<32;++pose) {
        const unsigned width=pose%3u==0?640u:pose%3u==1?854u:1792u;
        fx16_mat4 proj;CHECK(!fx16_perspective(&proj,R4_FOV_BAM,
            (int32_t)(width*SH_ONE/480u),R4_NEAR_Z,R4_FAR_Z));
        if(pose%5u==0)proj.v[1]=1234; /* generic projection matrix */
        rast_viewport vp={0,0,(uint16_t)width,480};
        if(pose&1u){vp.x=13;vp.y=7;vp.w-=26;vp.h-=14;}
        s_px=(int32_t)(pose%7u)*SH_ONE/3-SH_ONE;
        s_pz=(int32_t)(pose%9u)*SH_ONE/3-SH_ONE;
        s_yaw=(uint16_t)(pose*1731u);s_pitch=(int32_t)(pose%7u)*2500-7500;
        (void)sh_world_emit(&proj,&vp,records,SH_PC_RECORD_CAP);
        for(unsigned i=0;i<64;++i) {
            rng=rng*1664525u+1013904223u;int32_t x=(int32_t)(rng%12001u)*SH_ONE/1000-6*SH_ONE;
            rng=rng*1664525u+1013904223u;int32_t z=(int32_t)(rng%12001u)*SH_ONE/1000-6*SH_ONE;
            int32_t rx=(int32_t)(i%17u)*SH_ONE/20,rz=(int32_t)(i%13u)*SH_ONE/20;
            unsigned capacity=i%8u==0?0u:i%8u==1?3u:i%8u==2?9u:160u;
            memset(fast,0xa5,sizeof fast);memset(reference,0xa5,sizeof reference);
            rast_record *a=fast,*b=reference;uint32_t ac=capacity,bc=capacity;
            s_n=0;s_failed=0;sh_ground_shadow(x,z,rx,rz,&vp,&a,&ac);
            unsigned produced=s_n,failed=s_failed;
            s_n=0;s_failed=0;shadow_reference(x,z,rx,rz,&vp,&b,&bc);
            CHECK(produced==s_n && failed==s_failed && ac==bc && a-fast==b-reference);
            CHECK(!memcmp(fast,reference,sizeof fast));
        }
    }
    puts("SH SHADOW REUSE PASS 2048 rings, seams/clip/projection/viewport/capacity byte identity");
}
static void view_reuse_checks(void)
{
    uint32_t crc=2166136261u;
    sh_world_cache_bind(NULL,0);
    for(unsigned pose=0;pose<96;++pose) {
        sh_world_init(77);
        unsigned width=pose%3u==0?640u:pose%3u==1?854u:1792u;
        fx16_mat4 proj;CHECK(!fx16_perspective(&proj,R4_FOV_BAM,
            (int32_t)(width*SH_ONE/480u),R4_NEAR_Z,R4_FAR_Z));
        if(pose%7u==0)proj.v[1]=777;
        rast_viewport vp={0,0,(uint16_t)width,480};
        if(pose&1u){vp.x=13;vp.w-=26;}
        s_px=s_enemy[0].x+(int32_t)(pose%8u)*SH_ONE/40-SH_ONE/10;
        s_pz=s_enemy[0].z+(int32_t)(pose%9u)*SH_ONE/40-SH_ONE/10;
        s_yaw=(uint16_t)(pose*1459u);s_pitch=(int32_t)(pose%9u)*2000-8000;
        uint32_t n=sh_world_emit(&proj,&vp,records,SH_PC_RECORD_CAP);
        CHECK(n && n<=SH_PC_RECORD_CAP);
        crc=hash(crc,&n,sizeof n);crc=hash(crc,records,n*sizeof *records);
    }
    /* Established against the frozen pre-reuse scene and the generic path. */
    CHECK(crc==0x1c2d4e40u);
    printf("SH VIEW REUSE PASS near-camera cases=96 crc=%08x\n",crc);
}
static void bin_cache_checks(void)
{
    static uint8_t plain[32768],cached[32768],warm[2u*SH_PC_RECORD_CAP];
    static union {uint32_t align;uint8_t bytes[32776];} guarded;
    sh_world_init(7);fx16_mat4 proj;
    CHECK(!fx16_perspective(&proj,R4_FOV_BAM,4*SH_ONE/3,R4_NEAR_Z,R4_FAR_Z));
    rast_viewport vp={0,0,640,480};rast_frame frame={640,480};
    for(unsigned round=0;round<4;++round) {
        sh_world_look((int32_t)round*5-5,(int32_t)round-1);
        tick(round&1u?PLATFORM_KEY_UP:PLATFORM_KEY_STRAFE_R);
        uint32_t n=sh_world_emit(&proj,&vp,records,SH_PC_RECORD_CAP);
        CHECK(n && n!=UINT32_MAX && n<=SH_PC_RECORD_CAP);
        int a=rast_bin_frame(records,n,plain,sizeof plain,&frame);CHECK(a>=120);
        /* The scene-maintained table mixes trusted cached slices with fresh
           0xFF entries; the output must equal the full-validation bin. */
        int b=rast_bin_frame_cache(records,n,sh_world_ranges(),cached,sizeof cached,&frame);
        CHECK(b==a && !memcmp(plain,cached,(size_t)a));
        /* A fully warm table re-bins identically without touching record bodies. */
        memcpy(warm,sh_world_ranges(),2u*n);
        int c=rast_bin_frame_cache(records,n,warm,cached,sizeof cached,&frame);
        CHECK(c==a && !memcmp(plain,cached,(size_t)a));
        for(unsigned offset=0;offset<4;++offset) {
            uint8_t *dst=guarded.bytes+4u+offset;
            memset(guarded.bytes,0xa5,sizeof guarded.bytes);
            CHECK(rast_bin_frame_cache(records,n,warm,dst,(size_t)a,&frame)==a);
            CHECK(!memcmp(plain,dst,(size_t)a) && dst[-1]==0xa5 && dst[a]==0xa5);
            memset(guarded.bytes,0xa5,sizeof guarded.bytes);
            CHECK(rast_bin_frame_cache(records,n,warm,dst,(size_t)a-1u,&frame)==-2);
            for(unsigned k=0;k<sizeof guarded.bytes;++k)CHECK(guarded.bytes[k]==0xa5);
        }
        /* A corrupt trusted entry must fail the frame, not reach the raster. */
        warm[2u*(n/2u)]=0xFEu;warm[2u*(n/2u)+1u]=0xFFu;
        CHECK(rast_bin_frame_cache(records,n,warm,cached,sizeof cached,&frame)==-1);
    }
    puts("SH BIN CACHE PASS ranged binner byte-identical, trust errors caught");
}
static void range_reuse_checks(void)
{
    static uint8_t plain[32768],cached[32768];
    sh_world_cache_bind(cache_memory+1,SH_CACHE_BYTES);sh_world_init(7);
    fx16_mat4 proj;CHECK(!fx16_perspective(&proj,R4_FOV_BAM,4*SH_ONE/3,R4_NEAR_Z,R4_FAR_Z));
    rast_viewport vp={0,0,640,480};rast_frame frame={640,480};
    uint32_t n=sh_world_emit(&proj,&vp,records,SH_PC_RECORD_CAP),split=sh_world_weapon_start();
    CHECK(n && n<=SH_PC_RECORD_CAP && split && split<n);
    CHECK(rast_bin_frame_cache(records,split,sh_world_ranges(),cached,sizeof cached,&frame)>=120);
#if !SH_DISABLE_SCENE_CACHE
    CHECK(sh_world_ranges_commit(n)==-1); /* weapon slice has not been validated */
    CHECK(s_room_cache.ranges[0]==0xFFu); /* no partial publication */
#endif
    for(unsigned pass=0;pass<5;++pass) {
        int a=rast_bin_frame(records,n,plain,sizeof plain,&frame);
        int b=rast_bin_frame_cache(records,n,sh_world_ranges(),cached,sizeof cached,&frame);
        CHECK(a>=120 && a==b && !memcmp(plain,cached,(size_t)a));
        CHECK(!sh_world_ranges_commit(n));
        memcpy(saved_scene,records,n*sizeof *records);uint32_t previous=n;
#if !SH_DISABLE_SCENE_CACHE
        CHECK(s_room_cache.ranges[0]!=0xFFu);
#endif
        n=sh_world_emit(&proj,&vp,records,SH_PC_RECORD_CAP);
        CHECK(n==previous && !memcmp(saved_scene,records,n*sizeof *records));
#if !SH_DISABLE_SCENE_CACHE
        for(unsigned i=0;i<n;++i)CHECK(sh_world_ranges()[2u*i]!=0xFFu);
#endif
        if(pass==1 || pass==3) {
            sh_world_look(8,0); /* world invalidates, viewmodel remains reusable */
            n=sh_world_emit(&proj,&vp,records,SH_PC_RECORD_CAP);
            CHECK(n && n<=SH_PC_RECORD_CAP);
#if !SH_DISABLE_SCENE_CACHE
            CHECK(s_room_cache.count && sh_world_ranges()[0]==0xFFu);
            CHECK(sh_world_ranges()[2u*sh_world_weapon_start()]!=0xFFu);
            CHECK(rast_bin_frame_cache(records,n,sh_world_ranges(),cached,1,&frame)==-2);
            CHECK(s_room_cache.ranges[0]==0xFFu); /* rejected capacity never publishes */
#endif
        }
        if(pass==2) {
            proj.v[0]+=31;vp.x=13;vp.w=614;
            n=sh_world_emit(&proj,&vp,records,SH_PC_RECORD_CAP);
            CHECK(n && n<=SH_PC_RECORD_CAP);
#if !SH_DISABLE_SCENE_CACHE
            CHECK(sh_world_ranges()[0]==0xFFu);
            CHECK(sh_world_ranges()[2u*sh_world_weapon_start()]==0xFFu);
#endif
        }
    }
    sh_world_cache_bind(NULL,0);
    puts("SH RANGE REUSE PASS next-frame warm slices, camera/projection/viewport invalidation, atomic publication");
}
static void mesh_checks(void)
{
    CHECK(SH_MESH_VERTICES<150u && SH_MESH_FACES<=240u && SH_GUN_FACES<=110u);
    for(unsigned pose=0;pose<33u;++pose)for(unsigned f=0;f<SH_MESH_FACES;++f) {
        const uint8_t *ids=sh_mesh_faces[f];
        for(unsigned k=0;k<3;++k)CHECK(ids[k]<SH_MESH_VERTICES);
        const int16_t *a=sh_mesh_vertices[pose][ids[0]],*b=sh_mesh_vertices[pose][ids[1]],
                      *c=sh_mesh_vertices[pose][ids[2]];
        int64_t u[3],v[3],cross[3],dot=0,area=0;
        for(unsigned k=0;k<3;++k){u[k]=b[k]-a[k];v[k]=c[k]-a[k];}
        for(unsigned k=0;k<3;++k){cross[k]=u[(k+1)%3]*v[(k+2)%3]-u[(k+2)%3]*v[(k+1)%3];
            dot+=cross[k]*sh_mesh_normals[pose][f][k];area+=cross[k]*cross[k];}
        CHECK(area>0 && dot>0 && sh_mesh_materials[f]<4u);
    }
    sh_world_init(42);fx16_mat4 proj;
    CHECK(!fx16_perspective(&proj,R4_FOV_BAM,4*SH_ONE/3,R4_NEAR_Z,R4_FAR_Z));
    rast_viewport vp={0,0,640,480};CHECK(sh_world_emit(&proj,&vp,records,SH_PC_RECORD_CAP)!=UINT32_MAX);
    s_n=0;uint32_t cap=SH_PC_RECORD_CAP;rast_record *out=records;
    sh_mesh_draw(sh_mesh_vertices[16],SH_MESH_VERTICES,sh_mesh_faces,sh_mesh_normals[16],
        sh_mesh_materials,SH_MESH_FACES,0,0,0,0,SH_ONE,&vp,&out,&cap);
    CHECK(s_n>50u && s_n<=SH_MESH_FACES);
    unsigned changes=0;
    for(unsigned i=0;i<s_n;++i) {
        for(unsigned k=0;k<6;++k)CHECK(rast_get32(&records[i],RAST_O_DRGB+k*4)==0u);
        for(unsigned k=1;k<3;++k)CHECK(rast_get32(&records[i],RAST_O_COLOR)==rast_get32(&records[i],RAST_O_COLOR+k*4));
        if(i && rast_get32(&records[i],RAST_O_COLOR)!=rast_get32(&records[i-1],RAST_O_COLOR))++changes;
    }
    CHECK(changes>5u);
    puts("SH FACETS PASS 33 poses, outward nondegenerate faces, bounded topology and flat RGB with material contrast");
}
static void geometry(void)
{
    uint32_t h=2166136261u,peak=0;
    for(unsigned pass=0;pass<2;++pass) {
        uint32_t run=2166136261u;sh_world_init(42);
        for(unsigned i=0;i<240;++i) {
            unsigned width=i%3==0?640:i%3==1?854:1792;
            rast_viewport vp={0,0,(uint16_t)width,480};fx16_mat4 proj;
            CHECK(!fx16_perspective(&proj,R4_FOV_BAM,fx16_div((int32_t)width*SH_ONE,480*SH_ONE),R4_NEAR_Z,R4_FAR_Z));
            sh_world_look(i%2?7:-3,(int32_t)(i%5)-2);
            tick(i%90<30?PLATFORM_KEY_UP:i%90<60?PLATFORM_KEY_STRAFE_R|PLATFORM_KEY_RIGHT:0);
            memset(&records[SH_PC_RECORD_CAP],0x5a,sizeof *records);
            uint32_t n=sh_world_emit(&proj,&vp,records,SH_PC_RECORD_CAP);CHECK(n && n!=UINT32_MAX && n<=SH_PC_RECORD_CAP);
            CHECK(sh_world_weapon_start()>0 && sh_world_weapon_start()<n);
            for(unsigned g=0;g<sizeof *records;++g)CHECK(records[SH_PC_RECORD_CAP].bytes[g]==0x5a);
            rast_frame frame={width,480};CHECK(rast_bin_frame(records,n,bins,sizeof bins,&frame)>=120);
            if(n>peak)peak=n;
            run=hash(run,records,(size_t)n*sizeof *records);
            if(i%40==0) {
                for(unsigned j=0;j<480*1800;++j)image[j]=0xabcdef;
                sh_world_overlay(image,width,480,1800);
                for(unsigned y=0;y<480;++y)for(unsigned x=width;x<1800;++x)CHECK(image[y*1800+x]==0xabcdef);
            }
        }
        if(pass)CHECK(h==run);else h=run;
    }
    /* Near-wall clipping and the full effect pool, including a wide canvas. */
    for(unsigned room=0;room<3;++room)for(unsigned yaw=0;yaw<8;++yaw)for(int pitch=-1;pitch<=1;++pitch) {
        sh_world_init(42);s_px=room==0?0:room==1?-47*SH_ONE/10:47*SH_ONE/10;
        s_pz=room==0?35*SH_ONE/10:room==1?-37*SH_ONE/10:37*SH_ONE/10;
        s_yaw=(uint16_t)(yaw*8192u);s_pitch=pitch*9000;
        for(unsigned i=0;i<SH_ENEMY_MAX;++i)s_enemy[i]=(sh_enemy){((int32_t)i-2)*SH_ONE, -SH_ONE,0,0,0,i*5000u,1,1};
        for(unsigned i=0;i<SH_SHARD_MAX;++i)s_shard[i]=(sh_shard){((int32_t)(i%8)-4)*SH_ONE/3,
            SH_ONE+(int32_t)(i%4)*SH_ONE/8,-SH_ONE,0,0,0,10*SH_ONE, (uint16_t)(i*1700u),1};
        for(unsigned i=0;i<SH_BULLET_MAX;++i)s_bullet[i]=(sh_bullet){((int32_t)(i%6)-3)*SH_ONE/3,
            SH_EYE,-(int32_t)(i/6)*SH_ONE/4,SH_ONE/20,0,SH_ONE/5,1,(uint8_t)(i%2)};
        fx16_mat4 proj;CHECK(!fx16_perspective(&proj,R4_FOV_BAM,1792*SH_ONE/480,R4_NEAR_Z,R4_FAR_Z));
        rast_viewport vp={0,0,1792,480};uint32_t n=sh_world_emit(&proj,&vp,records,SH_PC_RECORD_CAP);
        if(n==UINT32_MAX)fprintf(stderr,"SH budget room=%u yaw=%u pitch=%d emitted=%u\n",room,yaw,pitch,s_n);
        CHECK(n && n!=UINT32_MAX && n<=SH_PC_RECORD_CAP);if(n>peak)peak=n;
        rast_frame frame={1792,480};CHECK(rast_bin_frame(records,n,bins,sizeof bins,&frame)>=120);
    }
    fx16_mat4 proj;CHECK(!fx16_perspective(&proj,R4_FOV_BAM,4*SH_ONE/3,R4_NEAR_Z,R4_FAR_Z));
    rast_viewport vp={0,0,640,480};CHECK(sh_world_emit(&proj,&vp,records,1)==UINT32_MAX);
    printf("SH GEOMETRY PASS records_peak=%u deterministic_crc=%08x width=640/854/1792 padding\n",peak,h);
}
static void weapon_pixels(void)
{
    sh_world_init(42);s_px=0;s_pz=-37*SH_ONE/10;s_yaw=32768u;
    fx16_mat4 proj;CHECK(!fx16_perspective(&proj,R4_FOV_BAM,4*SH_ONE/3,R4_NEAR_Z,R4_FAR_Z));
    rast_viewport vp={0,0,640,480};rast_frame frame={640,480};
    uint32_t n=sh_world_emit(&proj,&vp,records,SH_PC_RECORD_CAP),split=sh_world_weapon_start();
    CHECK(n!=UINT32_MAX && split<n);
    memset(image,0,sizeof image);
    for(unsigned strip=0;strip<30;++strip) {
        rast_target target={0};target.color=image+strip*16u*640;target.depth=depth_strip;
        target.stride=640;target.strip=strip;
        memset(depth_strip,255,sizeof depth_strip);
        CHECK(!rast_ref_run_frame(records,split,&target,&frame));
        memset(depth_strip,255,sizeof depth_strip);
        CHECK(!rast_ref_run_frame(records+split,n-split,&target,&frame));
    }
    unsigned dark=0;
    for(unsigned y=300;y<430;++y)for(unsigned x=380;x<600;++x)
        if((image[y*640+x]&0xffffffu)<0x505050u)++dark;
    CHECK(dark>100);
    puts("SH VIEWMODEL PASS gun remains visible against a near wall");
}
static void screenshots(void)
{
    const char *directory=getenv("SH_SCREEN_DIR");if(!directory)return;
    for(unsigned pose=0;pose<4;++pose) {
        sh_world_init(42);s_px=0;s_pz=pose==3?-37*SH_ONE/10:0;s_yaw=32768u;
        memset(s_enemy,0,sizeof s_enemy);enemy(-SH_ONE/2,-2*SH_ONE);s_enemy[0].ph=8192;s_enemy[0].move=1;
        if(pose==1){s_recoil=SH_ONE/5;s_flash=1;}
        if(pose==2)s_reload=20;
        fx16_mat4 proj;CHECK(!fx16_perspective(&proj,R4_FOV_BAM,4*SH_ONE/3,R4_NEAR_Z,R4_FAR_Z));
        rast_viewport vp={0,0,640,480};rast_frame frame={640,480};
        uint32_t n=sh_world_emit(&proj,&vp,records,SH_PC_RECORD_CAP),split=sh_world_weapon_start();
        CHECK(n!=UINT32_MAX && split<n);
        for(unsigned i=0;i<640*480;++i)image[i]=0x00f3f5f8;
        for(unsigned strip=0;strip<30;++strip) {
            rast_target target={0};target.color=image+strip*16u*640;target.depth=depth_strip;
            target.stride=640;target.strip=strip;memset(depth_strip,255,sizeof depth_strip);
            CHECK(!rast_ref_run_frame(records,split,&target,&frame));
            memset(depth_strip,255,sizeof depth_strip);
            CHECK(!rast_ref_run_frame(records+split,n-split,&target,&frame));
        }
        char path[1024];CHECK(snprintf(path,sizeof path,"%s/facet-%u.ppm",directory,pose)>0);
        FILE *f=fopen(path,"wb");CHECK(f);fprintf(f,"P6\n640 480\n255\n");
        for(unsigned i=0;i<640*480;++i){unsigned char rgb[3]={(unsigned char)(image[i]>>16),(unsigned char)(image[i]>>8),(unsigned char)image[i]};CHECK(fwrite(rgb,1,3,f)==3);}
        CHECK(!fclose(f));
    }
}
int main(void){division_reuse_checks();flat_setup_checks();flat_fan_checks();normal_checks();projection_checks();elapsed_checks();enemy_motion_checks();simulation();mesh_checks();bin_cache_checks();range_reuse_checks();shadow_reuse_checks();view_reuse_checks();geometry();weapon_pixels();screenshots();scene_cache_checks();return 0;}
