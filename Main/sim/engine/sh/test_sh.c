/* Exercise the actual scene state and production raster path, including private
 * collision helpers. No test-only behavior is compiled into the scene. */
#include "../../../sw/apps/game/gfx2dGame/src/sh_scene.c"
#include <stdlib.h>
#include "gfx/rast_ref.h"

#define CHECK(x) do {if(!(x)){fprintf(stderr,"SH FAIL line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static uint32_t fail;
static void tick(uint32_t keys) {sh_world_tick(keys,&fail);CHECK(!fail);}
static void empty(void)
{
    sh_world_init(1234);memset(s_enemy,0,sizeof s_enemy);s_spawn=0;
    memset(s_bullet,0,sizeof s_bullet);s_px=s_pz=0;
}
static void enemy(int x,int z)
{s_enemy[0]=(sh_enemy){x, z,0,0,-100*SH_ONE,0,1,0};}
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
static uint32_t hash(uint32_t h,const void *data,size_t n)
{const uint8_t *p=data;while(n--)h=(h^*p++)*16777619u;return h;}
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
int main(void){simulation();geometry();weapon_pixels();return 0;}
