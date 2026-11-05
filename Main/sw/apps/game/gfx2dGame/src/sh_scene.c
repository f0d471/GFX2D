/* HALTTIME: deterministic 30 Hz, Q16.16 world, BAM yaw/pitch.
 * Player input uses real time; AI, rounds, cooldown and fragments use world time.
 * Geometry remains the project's L1 128-byte raster record path. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "r4.h"
#include "input.h"
#include "sh_scene.h"
#include "sh_mesh.h"
#include "core/fixmath.h"
#include "core/xorshift32.h"
#include "raster_setup_port.h"
#include "r4_geometry.h"

#ifndef SH_REFERENCE_MATH
#define SH_REFERENCE_MATH 0
#endif
#ifndef SH_DISABLE_SCENE_CACHE
#define SH_DISABLE_SCENE_CACHE 0
#endif
#ifndef SH_DISABLE_VERTEX_CACHE
#define SH_DISABLE_VERTEX_CACHE 0
#endif

#define SH_ONE R4_ONE
#define SH_EYE (155*SH_ONE/100)
#define SH_WALK (SH_ONE/12)
#define SH_TURN 520
#define SH_TS_MIN 1638
#define SH_COOL_FIRE (12*SH_ONE)
#define SH_COOL_ENEMY (44*SH_ONE)
#define SH_ESPD (SH_ONE/35)
#define SH_ENEMY_MAX 3u
#define SH_BULLET_MAX 16u
#define SH_SHARD_MAX 32u
#define SH_TRACE_MAX 6u
#define CORPSE_X (75*SH_ONE/100)
#define CORPSE_Z (-50*SH_ONE/100)

typedef struct { int32_t x,z,tx,tz,cool; uint32_t ph; uint8_t alive,move; } sh_enemy;
typedef struct { int32_t x,y,z,vx,vy,vz; uint8_t alive,player; } sh_bullet;
typedef struct { int32_t x,y,z,vx,vy,vz,life; uint16_t rot; uint8_t alive; } sh_shard;
typedef struct { int32_t ax,ay,az,bx,by,bz,life; uint8_t alive; } sh_trace;
typedef struct { int32_t x,z; uint8_t alive; } sh_drop;
typedef struct { int32_t x,z,hx,hz,top; } sh_solid;
/* Same dimensions as emitted furniture: all gameplay queries share these boxes. */
static const sh_solid s_solid[]={
    {-26*SH_ONE/10,20*SH_ONE/10,30*SH_ONE/100,30*SH_ONE/100,3*SH_ONE},
    {27*SH_ONE/10,-19*SH_ONE/10,30*SH_ONE/100,30*SH_ONE/100,3*SH_ONE},
    {-442*SH_ONE/100,25*SH_ONE/100,50*SH_ONE/100,42*SH_ONE/100,42*SH_ONE/100},
    {-458*SH_ONE/100,25*SH_ONE/100,12*SH_ONE/100,42*SH_ONE/100,91*SH_ONE/100},
    {-31*SH_ONE/10,25*SH_ONE/100,26*SH_ONE/100,26*SH_ONE/100,46*SH_ONE/100},
    {19*SH_ONE/10,17*SH_ONE/10,34*SH_ONE/100,34*SH_ONE/100,62*SH_ONE/100},
    {-14*SH_ONE/10,-16*SH_ONE/10,36*SH_ONE/100,36*SH_ONE/100,54*SH_ONE/100}
};
static sh_enemy s_enemy[SH_ENEMY_MAX];
static sh_bullet s_bullet[SH_BULLET_MAX];
static sh_shard s_shard[SH_SHARD_MAX];
static sh_trace s_trace[SH_TRACE_MAX];
static sh_drop s_drop[SH_ENEMY_MAX];
static int32_t s_px,s_pz,s_pitch,s_ts=SH_TS_MIN,s_cool,s_spawn,s_recoil,s_look;
static uint16_t s_yaw=32768u;
static uint32_t s_ticks,s_score,s_shield,s_rng,s_spawned,s_prev,s_seed;
static uint32_t s_kills,s_hits,s_drop_cursor,s_reload;
static unsigned s_ammo,s_flash,s_hitmark;
static uint8_t s_failed,s_viewmodel;
static uint32_t s_n;
static uint32_t s_weapon_start;
static fx16_mat4 s_view;
static const fx16_mat4 *s_proj;
static const rast_viewport *s_mesh_vp;
#if !SH_DISABLE_SCENE_CACHE
static struct {
    int valid;int32_t x,z,pitch;uint16_t yaw;
    fx16_mat4 projection;rast_viewport vp;uint32_t count,version;
    rast_record *records;
} s_room_cache;
static struct {
    int valid;int32_t x,z,swing;uint32_t version,count;
    rast_record *records;
} s_actor_cache[SH_ENEMY_MAX];
#endif
#if !SH_DISABLE_SCENE_CACHE
typedef struct {
    uint32_t valid,count,age;int32_t recoil;uint8_t reload,flash;
    fx16_mat4 projection;rast_viewport vp;
    rast_record records[256];
} sh_weapon_cache;
static sh_weapon_cache *s_weapon_cache;static uint32_t s_weapon_cache_age;
#endif
void sh_world_cache_bind(void *memory,uint32_t bytes)
{
#if !SH_DISABLE_SCENE_CACHE
    const uint32_t weapon_bytes=(uint32_t)((SH_WEAPON_CACHE_SLOTS*sizeof(sh_weapon_cache)+127u)&~127u);
    const uint32_t required=weapon_bytes+(1024u+3u*512u)*RAST_RECORD_BYTES;
    s_weapon_cache=memory && bytes>=required?memory:NULL;s_weapon_cache_age=0;
    s_room_cache.valid=0;s_room_cache.records=NULL;
    for(unsigned i=0;i<SH_ENEMY_MAX;++i){s_actor_cache[i].valid=0;s_actor_cache[i].records=NULL;}
    if(s_weapon_cache) {
        for(unsigned i=0;i<SH_WEAPON_CACHE_SLOTS;++i)s_weapon_cache[i].valid=0;
        uint8_t *storage=(uint8_t *)memory+weapon_bytes;
        s_room_cache.records=(rast_record *)(void *)storage;storage+=1024u*RAST_RECORD_BYTES;
        for(unsigned i=0;i<SH_ENEMY_MAX;++i){s_actor_cache[i].records=(rast_record *)(void *)storage;storage+=512u*RAST_RECORD_BYTES;}
    }
#else
    (void)memory;(void)bytes;
#endif
}
static uint32_t s_elapsed_accum,s_elapsed_started;

#ifndef SH_PROFILE
#define SH_PROFILE 0
#endif
#if SH_PROFILE
static uint32_t (*s_clock)(void);
static uint32_t s_profile[12];
#define PROF_START uint32_t profile_start=s_clock?s_clock():0
#define PROF_END(slot) do {if(s_clock){++s_profile[(slot)*2];s_profile[(slot)*2+1]+=s_clock()-profile_start;}}while(0)
void sh_profile_clock(uint32_t (*clock_fn)(void)){s_clock=clock_fn;}
void sh_profile_read(uint32_t out[12]){memcpy(out,s_profile,sizeof s_profile);}
#else
#define PROF_START
#define PROF_END(slot) ((void)0)
void sh_profile_clock(uint32_t (*clock_fn)(void)){(void)clock_fn;}
void sh_profile_read(uint32_t out[12]){memset(out,0,12*sizeof *out);}
#endif

static int32_t sh_sin(uint16_t a) {return fx_sin(a)*4;}
static int32_t sh_cos(uint16_t a) {return fx_cos(a)*4;}
static int32_t sh_mul(int32_t a,int32_t b) {return (int32_t)((int64_t)a*b/SH_ONE);}
static uint32_t sh_rgb(unsigned r,unsigned g,unsigned b) {return r*65536u+g*256u+b;}
/* Bounded binary integer square root: the old increment loop took >300k steps/face. */
static int32_t sh_isqrt(int64_t value)
{
    uint64_t n=value>0?(uint64_t)value:0,r=0,bit=UINT64_C(1)<<62;
    while(bit>n)bit>>=2;
    while(bit) {if(n>=r+bit){n-=r+bit;r=(r>>1)+bit;}else r>>=1;bit>>=2;}
    return (int32_t)r;
}
#if !SH_REFERENCE_MATH
static uint32_t sh_isqrt32(uint32_t n)
{
    uint32_t r=0,bit=1u<<30;
    while(bit>n)bit>>=2;
    while(bit){if(n>=r+bit){n-=r+bit;r=(r>>1)+bit;}else r>>=1;bit>>=2;}
    return r;
}
#endif
static uint32_t sh_fog(int64_t d2,uint32_t col)
{
    if(s_viewmodel)return col;
#if SH_REFERENCE_MATH
    const int32_t distance=sh_isqrt(d2);
#else
    /* Room-space distance in Q8: <1/256m error, no RV32 software u64 sqrt/div. */
    const int32_t distance=((uint64_t)d2>>16)>UINT32_MAX?sh_isqrt(d2):
        (int32_t)(sh_isqrt32((uint32_t)((uint64_t)d2>>16))*256u);
#endif
    int32_t f=(distance-SH_ONE*8/5)/8;
    unsigned cr=(col>>16)&255u,cg=(col>>8)&255u,cb=col&255u;
    if(f<=0)return col;
    if(f>26000)f=26000;
    /* Cancel the common factor 1000 exactly; the product now fits 32 bits. */
    f=f*(int32_t)(1020u+6u*((cr+cg+cb)/3u))/2550;
    return sh_rgb((unsigned)((int)cr+(252-(int)cr)*f/SH_ONE),
                  (unsigned)((int)cg+(253-(int)cg)*f/SH_ONE),
                  (unsigned)((int)cb+(255-(int)cb)*f/SH_ONE));
}
static uint32_t sh_rnd(void)
{s_rng^=s_rng<<13;s_rng^=s_rng>>17;s_rng^=s_rng<<5;return s_rng;}

/* Segment/slab intersection. t in [0,ONE]; swept tests cannot tunnel at full speed. */
static int32_t sh_segment(int32_t ax,int32_t ay,int32_t az,int32_t bx,int32_t by,int32_t bz,
                         int32_t cx,int32_t cy,int32_t cz,int32_t hx,int32_t hy,int32_t hz)
{
    const int32_t a[3]={ax,ay,az},b[3]={bx,by,bz},c[3]={cx,cy,cz},h[3]={hx,hy,hz};
    int64_t lo=0,hi=SH_ONE;
    for(unsigned i=0;i<3;++i) {
        const int64_t d=(int64_t)b[i]-a[i],v=(int64_t)a[i]-c[i];
        if(!d){if(v< -h[i] || v>h[i])return -1;continue;}
        int64_t t0=(-h[i]-v)*SH_ONE/d,t1=(h[i]-v)*SH_ONE/d;
        if(t0>t1){int64_t t=t0;t0=t1;t1=t;}
        if(t0>lo)lo=t0;
        if(t1<hi)hi=t1;
        if(lo>hi)return -1;
    }
    return (int32_t)lo;
}
static int32_t sh_cover(int32_t ax,int32_t ay,int32_t az,int32_t bx,int32_t by,int32_t bz)
{
    int32_t best=SH_ONE+1;
    for(unsigned i=0;i<sizeof s_solid/sizeof *s_solid;++i) {
        const sh_solid *s=&s_solid[i];
        int32_t t=sh_segment(ax,ay,az,bx,by,bz,s->x,s->top/2,s->z,s->hx,s->top/2,s->hz);
        if(t>=0 && t<best)best=t;
    }
    return best;
}
static int sh_free(int32_t x,int32_t z,int32_t radius,int enemies)
{
    if(x< -5*SH_ONE+radius || x>5*SH_ONE-radius || z< -4*SH_ONE+radius || z>4*SH_ONE-radius)return 0;
    for(unsigned i=0;i<sizeof s_solid/sizeof *s_solid;++i) {
        const sh_solid *s=&s_solid[i];
        if(x>s->x-s->hx-radius && x<s->x+s->hx+radius && z>s->z-s->hz-radius && z<s->z+s->hz+radius)return 0;
    }
    if(enemies)for(unsigned i=0;i<SH_ENEMY_MAX;++i)if(s_enemy[i].alive) {
        int64_t dx=x-s_enemy[i].x,dz=z-s_enemy[i].z;
        if(dx*dx+dz*dz<(int64_t)(radius+SH_ONE/4)*(radius+SH_ONE/4))return 0;
    }
    return 1;
}
static int sh_enemy_free(int32_t x,int32_t z)
{
    int64_t dx=x-s_px,dz=z-s_pz;
    return dx*dx+dz*dz>=(int64_t)(SH_ONE*45/100)*(SH_ONE*45/100) && sh_free(x,z,SH_ONE/4,1);
}
static int sh_spawn_enemy(void)
{
    static const int16_t pos[6][2]={{-32,-34},{32,-34},{0,-34},{40,5},{-32,-25},{20,-33}};
    for(unsigned i=0;i<SH_ENEMY_MAX;++i)if(!s_enemy[i].alive) {
        /* Try every entrance: camping one door cannot stop the endless stream. */
        for(unsigned trial=0;trial<6;++trial) {
            const unsigned k=(s_spawned+trial)%6u;
            int32_t x=pos[k][0]*SH_ONE/10,z=pos[k][1]*SH_ONE/10;
            if(!sh_free(x,z,SH_ONE/4,1))continue;
            int64_t dx=x-s_px,dz=z-s_pz;
            if(dx*dx+dz*dz<(int64_t)SH_ONE*SH_ONE*4)continue;
            s_enemy[i]=(sh_enemy){x,z,0,0,-(int32_t)(i*8u*SH_ONE),sh_rnd()&65535u,1,0};
            ++s_spawned;return 1;
        }
        return 0;
    }
    return 0;
}
void sh_world_init(uint32_t seed)
{
    s_ticks=s_score=s_prev=s_kills=s_hits=s_drop_cursor=s_reload=0;
    s_elapsed_accum=s_elapsed_started=0;
#if !SH_DISABLE_SCENE_CACHE
    s_room_cache.valid=0;
    for(unsigned i=0;i<SH_ENEMY_MAX;++i)s_actor_cache[i].valid=0;
#endif
    s_seed=seed?seed:1u;s_rng=s_seed;
    memset(s_enemy,0,sizeof s_enemy);memset(s_bullet,0,sizeof s_bullet);
    memset(s_shard,0,sizeof s_shard);memset(s_trace,0,sizeof s_trace);memset(s_drop,0,sizeof s_drop);
    s_px=0;s_pz=26*SH_ONE/10;s_yaw=32768u;s_pitch=0;s_ts=SH_TS_MIN;
    s_spawned=0;s_shield=1;s_ammo=6;s_cool=s_spawn=s_recoil=s_look=0;
    s_flash=s_hitmark=0;
    sh_spawn_enemy();sh_spawn_enemy();sh_spawn_enemy();
#ifdef SH_BOARD_WALL
    /* Isolated board-capture build; default gameplay always keeps its normal spawn. */
    s_px=0;s_pz=-37*SH_ONE/10;s_yaw=32768u;
#endif
}
void sh_world_look(int32_t dx,int32_t dy)
{
    if(dx>500)dx=500;
    if(dx< -500)dx= -500;
    if(dy>500)dy=500;
    if(dy< -500)dy= -500;
    s_yaw=(uint16_t)(s_yaw-dx*26);s_pitch-=dy*26;
    if(s_pitch>10000)s_pitch=10000;
    if(s_pitch< -10000)s_pitch= -10000;
    s_look+=(dx<0?-dx:dx)+(dy<0?-dy:dy);
}
/* Training mode: still consume collision and count hits; player never dies. */
static void sh_player_hit(void) {if(s_hits<UINT32_MAX)++s_hits;s_shield=1;}
static void sh_kill_enemy(unsigned bi)
{
    sh_enemy *e=&s_enemy[bi];e->alive=0;
    if(s_score<=UINT32_MAX-100u)s_score+=100;
    if(s_kills<UINT32_MAX)++s_kills;
    s_hitmark=7;
    unsigned drop=s_drop_cursor++%SH_ENEMY_MAX;
    for(unsigned i=0;i<SH_ENEMY_MAX;++i)if(!s_drop[i].alive){drop=i;break;}
    s_drop[drop]=(sh_drop){e->x,e->z,1};
    for(unsigned k=0;k<18;++k)for(unsigned j=0;j<SH_SHARD_MAX;++j)if(!s_shard[j].alive) {
        uint32_t a=sh_rnd();
        s_shard[j]=(sh_shard){e->x,SH_ONE/2+(int32_t)(a%SH_ONE),e->z,
            ((int32_t)(a&4095u)-2048)*4,SH_ONE/8+(int32_t)((a>>18)&1023u)*12,
            ((int32_t)((a>>12)&4095u)-2048)*4,30*SH_ONE+(int32_t)(a%12u)*SH_ONE,(uint16_t)(a>>8),1};
        break;
    }
}
static void sh_fire(void)
{
    if(!s_ammo || s_cool || s_reload)return;
    for(unsigned i=0;i<SH_BULLET_MAX;++i)if(!s_bullet[i].alive) {
        const int32_t cp=sh_cos((uint16_t)s_pitch),sp=sh_sin((uint16_t)s_pitch);
        s_bullet[i]=(sh_bullet){s_px,SH_EYE,s_pz,
            sh_mul(sh_sin(s_yaw),cp)*3/5,sp*3/5,sh_mul(sh_cos(s_yaw),cp)*3/5,1,1};
        --s_ammo;s_cool=SH_COOL_FIRE;s_flash=3;s_recoil=SH_ONE;s_ts=SH_ONE;break;
    }
}
void sh_world_tick(uint32_t keys,uint32_t *fail)
{
    (void)fail;++s_ticks;
    const uint32_t pressed=keys&~s_prev;s_prev=keys;
    if((pressed&PLATFORM_KEY_RESTART) && s_ammo<6 && !s_reload)s_reload=27;
    if(s_reload && --s_reload==0)s_ammo=6;
    const int fwd=(keys&PLATFORM_KEY_UP?1:0)-(keys&PLATFORM_KEY_DOWN?1:0);
    const int strafe=(keys&PLATFORM_KEY_STRAFE_R?1:0)-(keys&PLATFORM_KEY_STRAFE_L?1:0);
    const int turn=((keys&PLATFORM_KEY_RIGHT) && !(keys&PLATFORM_KEY_STRAFE_R)?1:0)-
                   ((keys&PLATFORM_KEY_LEFT) && !(keys&PLATFORM_KEY_STRAFE_L)?1:0);
    s_yaw=(uint16_t)(s_yaw-turn*SH_TURN);
    int32_t speed=fwd && strafe?SH_WALK*181/256:SH_WALK;
    const int32_t dx=sh_mul(sh_sin(s_yaw),speed)*fwd-sh_mul(sh_cos(s_yaw),speed)*strafe;
    const int32_t dz=sh_mul(sh_cos(s_yaw),speed)*fwd+sh_mul(sh_sin(s_yaw),speed)*strafe;
    const int32_t ox=s_px,oz=s_pz;
    if(sh_free(s_px+dx,s_pz,SH_ONE/5,1))s_px+=dx;
    if(sh_free(s_px,s_pz+dz,SH_ONE/5,1))s_pz+=dz;
    int32_t target=(s_px!=ox || s_pz!=oz)?SH_ONE:turn?26000:SH_TS_MIN+s_look*180;
    if(s_reload && target<20000)target=20000;
    if(target>SH_ONE)target=SH_ONE;
    s_look=0;s_ts+=(target-s_ts)/3;
    if(s_ts<SH_TS_MIN+3)s_ts=SH_TS_MIN;
    if(s_cool>0){s_cool-=s_ts;if(s_cool<0)s_cool=0;}
    if(s_flash)--s_flash;
    if(s_hitmark)--s_hitmark;
    s_recoil=s_recoil*3/4;
    if(keys&PLATFORM_KEY_ACTION)sh_fire();
    if(!s_ammo && !s_reload)s_reload=27;
    if(keys&PLATFORM_KEY_PICKUP)for(unsigned i=0;i<SH_ENEMY_MAX;++i)if(s_drop[i].alive) {
        int64_t x=s_drop[i].x-s_px,z=s_drop[i].z-s_pz;
        if(x*x+z*z<(int64_t)SH_ONE*SH_ONE*2){s_ammo=6;s_reload=0;s_drop[i].alive=0;break;}
    }
    const int32_t wdt=s_ts;
    unsigned alive=0;
    for(unsigned i=0;i<SH_ENEMY_MAX;++i)alive+=s_enemy[i].alive;
    /* Replenishment uses real ticks so standing still never stalls a cleared room. */
    if(alive<SH_ENEMY_MAX){if(s_spawn<24)++s_spawn;if(s_spawn>=24 && sh_spawn_enemy())s_spawn=0;}
    else s_spawn=0;
    for(unsigned i=0;i<SH_ENEMY_MAX;++i) {
        sh_enemy *e=&s_enemy[i];if(!e->alive)continue;
        int32_t tx=s_px-e->x,tz=s_pz-e->z;
        int32_t len=sh_isqrt((int64_t)tx*tx+(int64_t)tz*tz);
        const int blocked=sh_cover(e->x,SH_EYE,e->z,s_px,SH_EYE,s_pz)<=SH_ONE;
        e->move=len>SH_ONE*5/2 || blocked;
        if(e->move && len) {
            int32_t mx=(int32_t)((int64_t)tx*SH_ESPD/len*wdt/SH_ONE);
            int32_t mz=(int32_t)((int64_t)tz*SH_ESPD/len*wdt/SH_ONE);
            int32_t before_x=e->x,before_z=e->z;e->alive=0;
            if(sh_enemy_free(e->x+mx,e->z))e->x+=mx;
            if(sh_enemy_free(e->x,e->z+mz))e->z+=mz;
            if(e->x==before_x && e->z==before_z) {
                /* Slide along cover rather than walking in place forever. */
                int sign=(i&1u)?1:-1;
                for(unsigned attempt=0;attempt<2;++attempt,sign=-sign)
                    if(sh_enemy_free(e->x-mz*sign,e->z+mx*sign)) {
                        e->x-=mz*sign;e->z+=mx*sign;break;
                    }
            }
            e->alive=1;e->move=e->x!=before_x || e->z!=before_z;
            if(e->move)e->ph+=(uint32_t)(wdt/10);
        }
        if(len<SH_ONE*3/5)sh_player_hit();
        e->cool+=wdt;
        if(e->cool>=SH_COOL_ENEMY && !blocked && len) {
            for(unsigned j=0;j<SH_BULLET_MAX;++j)if(!s_bullet[j].alive) {
                s_bullet[j]=(sh_bullet){e->x,SH_EYE,e->z,
                    (int32_t)((int64_t)tx*(SH_ONE/5)/len),0,
                    (int32_t)((int64_t)tz*(SH_ONE/5)/len),1,0};
                e->cool=0;break;
            }
        }
    }
    for(unsigned i=0;i<SH_BULLET_MAX;++i) {
        sh_bullet *b=&s_bullet[i];if(!b->alive)continue;
        int32_t x=b->x+sh_mul(b->vx,wdt),y=b->y+sh_mul(b->vy,wdt),z=b->z+sh_mul(b->vz,wdt);
        int32_t best=sh_cover(b->x,b->y,b->z,x,y,z),hit=-1;
        if(b->player)for(unsigned j=0;j<SH_ENEMY_MAX;++j)if(s_enemy[j].alive) {
            int32_t t=sh_segment(b->x,b->y,b->z,x,y,z,s_enemy[j].x,SH_ONE*9/10,s_enemy[j].z,
                                 SH_ONE/4,SH_ONE*9/10,SH_ONE/4);
            if(t>=0 && t<best){best=t;hit=(int32_t)j;}
        }
        if(!b->player) {
            int32_t t=sh_segment(b->x,b->y,b->z,x,y,z,s_px,SH_ONE*9/10,s_pz,
                                 SH_ONE*16/100,SH_ONE*8/10,SH_ONE*16/100);
            if(t>=0 && t<best){best=t;hit=100;}
        }
        if(best<=SH_ONE){b->alive=0;if(hit==100)sh_player_hit();else if(hit>=0)sh_kill_enemy((unsigned)hit);}
        b->x=x;b->y=y;b->z=z;
        if(x< -5*SH_ONE || x>5*SH_ONE || z< -4*SH_ONE || z>4*SH_ONE || y<0 || y>3*SH_ONE)b->alive=0;
    }
    for(unsigned i=0;i<SH_SHARD_MAX;++i) {
        sh_shard *s=&s_shard[i];if(!s->alive)continue;
        s->x+=sh_mul(s->vx,wdt);s->y+=sh_mul(s->vy,wdt);s->z+=sh_mul(s->vz,wdt);
        s->vy-=sh_mul(SH_ONE/140,wdt);s->rot=(uint16_t)(s->rot+sh_mul(160,wdt));
        s->life-=wdt;if(s->life<=0 || s->y<SH_ONE/50)s->alive=0;
    }

}
uint32_t sh_world_advance(uint32_t keys,uint32_t milliseconds,uint32_t *fail)
{
    if(!s_elapsed_started){s_elapsed_started=1;sh_world_tick(keys,fail);return 1;}
    /* Bounded catch-up: render stalls do not stretch movement or reload time. */
    if(milliseconds>1000u)milliseconds=1000u;
    s_elapsed_accum+=milliseconds*30u;
    uint32_t steps=s_elapsed_accum/1000u;s_elapsed_accum%=1000u;
    for(unsigned i=0;i<steps;++i)sh_world_tick(keys,fail);
    return steps;
}
void sh_hud_read(uint32_t state[4])
{state[0]=s_kills;state[1]=s_ammo;state[2]=s_reload;state[3]=(uint32_t)s_ts;}

uint32_t sh_score(void){return s_score;}
uint32_t sh_shield(void){return s_shield;}
uint32_t sh_ticks(void){return s_ticks;}
uint32_t sh_world_weapon_start(void){return s_weapon_start;}

/* Scene-local sparse transforms. Products are bounded by the 10m room;
 * each term retains R1's round-to-nearest, half toward positive infinity. */
static int32_t sh_round_mul(int32_t a,int32_t b)
{
    int64_t n=(int64_t)a*b;
    return (int32_t)((n>>16)+(((uint64_t)n&65535u)>=32768u));
}
static void sh_transform(const int32_t p[3],int32_t view[3])
{
    if(s_viewmodel){memcpy(view,p,3*sizeof *view);return;}
    for(unsigned i=0;i<3;++i) {
        int32_t sum=s_view.v[i*4+3];
        for(unsigned k=0;k<3;++k)if(s_view.v[i*4+k])sum+=sh_round_mul(s_view.v[i*4+k],p[k]);
        view[i]=sum;
    }
}
#if !SH_DISABLE_VERTEX_CACHE
static int32_t sh_project_div(int32_t a,uint32_t denominator,uint32_t reciprocal)
{
    int64_t q=((int64_t)a*reciprocal)>>16;
    int64_t rem=(int64_t)a*SH_ONE-q*denominator;
    while(rem<0){--q;rem+=denominator;}
    while(rem>=denominator){++q;rem-=denominator;}
    return (int32_t)(q+(rem>=(denominator+1u)/2u));
}
static int sh_project_vertex(const rast_view_vertex *v,rast_vertex *out)
{
    if(v->z> -R4_NEAR_Z || v->z< -(int32_t)R4_FAR_Z)return -1;
    const fx16_mat4 *p=s_proj;
    if(p->v[1]||p->v[2]||p->v[3]||p->v[4]||p->v[6]||p->v[7]||p->v[8]||p->v[9]||
       p->v[12]||p->v[13]||p->v[14]!= -SH_ONE||p->v[15])
        return r4_project_inside(v,1,p,R4_NEAR_Z,R4_FAR_Z,s_mesh_vp,out);
    uint32_t w=(uint32_t)-v->z;
    uint32_t reciprocal=UINT32_MAX/w+(UINT32_MAX%w==w-1u);
    int32_t x=sh_project_div(sh_round_mul(p->v[0],v->x),w,reciprocal);
    int32_t y=sh_project_div(sh_round_mul(p->v[5],v->y),w,reciprocal);
    int32_t z=sh_project_div(sh_round_mul(p->v[10],v->z)+p->v[11],w,reciprocal);
    int64_t depth=fx16_round_div(((int64_t)z+SH_ONE)*65535,2*SH_ONE);
    out->x=s_mesh_vp->x*16+fx16_viewport(x,s_mesh_vp->w,0);
    out->y=s_mesh_vp->y*16+fx16_viewport(y,s_mesh_vp->h,1);
    out->z=(uint16_t)(depth<0?0:depth>65535?65535:depth);out->u=v->u;out->v=v->v;out->color=v->color;
    return 0;
}

/* ---- 发射 ---- */

#endif
/* Exact n*2^20/d with RV32 DIV/REM only. Each fractional digit keeps r<<bits
   in u32; signed half ties still round toward +infinity, as R1 requires. */
static inline int sh_gradient20(int32_t n,uint32_t d,int32_t *result)
{
    uint32_t magnitude=n<0?(uint32_t)(-(int64_t)n):(uint32_t)n;
    uint32_t q=magnitude/d,r=magnitude%d;
    if(q>=1024u)return 0;
    unsigned chunk=d<=4095u?20u:d<=65535u?16u:d<=1048575u?12u:d<=16777215u?8u:4u;
    for(unsigned left=20u;left;) {
        unsigned bits=left<chunk?left:chunk;
        r<<=bits;q=(q<<bits)+r/d;r%=d;left-=bits;
    }
    q+=n<0?r>d/2u:r>=(d+1u)/2u;
    if(q>=(1u<<30))return 0;
    *result=n<0?-(int32_t)q:(int32_t)q;
    return 1;
}

/* Existing 128B ABI, specialized only for an entirely inside, single-color
   triangle. -3 asks the caller to use the full clipping/setup reference. */
static inline int sh_flat_setup(const rast_vertex input[3],const rast_viewport *vp,
                                rast_record *out)
{
    rast_vertex v[3]={input[0],input[1],input[2]};
    for(unsigned i=0;i<3;++i)
        if(v[i].x<(int32_t)vp->x*16 || v[i].x>=(int32_t)(vp->x+vp->w)*16 ||
           v[i].y<(int32_t)vp->y*16 || v[i].y>=(int32_t)(vp->y+vp->h)*16 ||
           v[i].color!=v[0].color || v[i].u || v[i].v)return -3;
    int64_t area=(int64_t)(v[1].x-v[0].x)*(v[2].y-v[0].y)-
                 (int64_t)(v[2].x-v[0].x)*(v[1].y-v[0].y);
    if(!area)return 0;
    if(area<0){rast_vertex t=v[1];v[1]=v[2];v[2]=t;area=-area;}
    if(area>=(1<<28))return -3;
    int64_t nx=(int64_t)((int32_t)v[1].z-v[0].z)*(v[2].y-v[0].y)-
               (int64_t)((int32_t)v[2].z-v[0].z)*(v[1].y-v[0].y);
    int64_t ny=(int64_t)(v[1].x-v[0].x)*((int32_t)v[2].z-v[0].z)-
               (int64_t)(v[2].x-v[0].x)*((int32_t)v[1].z-v[0].z);
    if(nx<INT32_MIN || nx>INT32_MAX || ny<INT32_MIN || ny>INT32_MAX)return -3;
    int32_t dzdx,dzdy;
    if(!sh_gradient20((int32_t)nx,(uint32_t)area,&dzdx) ||
       !sh_gradient20((int32_t)ny,(uint32_t)area,&dzdy))return 0;
    rast_record r;memset(&r,0,sizeof r);rast_put32(&r,RAST_O_FLAGS,RAST_VCOLOR_EN);
    int32_t lo[2]={v[0].x,v[0].y},hi[2]={v[0].x,v[0].y};
    for(unsigned i=0;i<3;++i) {
        const rast_vertex *a=&v[i],*b=&v[(i+1)%3];
        int32_t dx=b->x-a->x,dy=b->y-a->y;
        rast_put32(&r,4+i*12,(uint32_t)(-dy*16));rast_put32(&r,8+i*12,(uint32_t)(dx*16));
        rast_put32(&r,12+i*12,(uint32_t)((int64_t)dy*a->x-(int64_t)dx*a->y));
        if(a->x<lo[0])lo[0]=a->x;
        if(a->x>hi[0])hi[0]=a->x;
        if(a->y<lo[1])lo[1]=a->y;
        if(a->y>hi[1])hi[1]=a->y;
        rast_put32(&r,RAST_O_COLOR+i*4,v[0].color);
    }
    for(unsigned k=0;k<2;++k) {
        lo[k]=(lo[k]+15)>>4;hi[k]>>=4;
        if(lo[k]>hi[k])return 0;
        rast_put16(&r,RAST_O_BBOX+k*4,(uint16_t)lo[k]);
        rast_put16(&r,RAST_O_BBOX+k*4+2,(uint16_t)hi[k]);
    }
    rast_put16(&r,RAST_O_Z0,v[0].z);rast_put16(&r,RAST_O_ORIGIN,(uint16_t)v[0].x);
    rast_put16(&r,RAST_O_ORIGIN+2,(uint16_t)v[0].y);
    rast_put32(&r,RAST_O_DZDX,(uint32_t)dzdx);rast_put32(&r,RAST_O_DZDY,(uint32_t)dzdy);
    memcpy(out,&r,sizeof r);return 1;
}

static int sh_poly(const rast_view_vertex *v, unsigned n, const rast_viewport *vp,
                    rast_record **out, uint32_t *cap)
{
    PROF_START;
    rast_view_vertex view[4];
    int r;
    if (n > 4u || *cap < n) { if (*cap < n) s_failed = 1; return -1; }
    /* 契约:gfx_setup_view 吃视空间顶点(近/远裁剪按它做),投影矩阵单独传。
       世界系输入在此经视图矩阵变换,与走廊/山脊同口径。 */
    for (unsigned i = 0; i < n; ++i) {
        const int32_t p[3]={v[i].x,v[i].y,v[i].z};int32_t transformed[3];
        sh_transform(p,transformed);
        view[i]=(rast_view_vertex){transformed[0],transformed[1],transformed[2],v[i].u,v[i].v,v[i].color};
    }
    {
        const rast_frame frame = {(unsigned)(vp->x + vp->w) > RAST_WIDTH
                                      ? (unsigned)(vp->x + vp->w) : RAST_WIDTH, RAST_HEIGHT};
        /* 场景面是手写绕向,不保证一致:首试被背面剔除即翻绕序重试(ridge_poly 同款)。 */
        rast_setup_stats st = {0};
        const uint32_t before = st.backface;
        int projected=0;
#if !SH_DISABLE_VERTEX_CACHE
        rast_vertex screen[4];projected=1;
        for(unsigned i=0;i<n;++i)if(sh_project_vertex(&view[i],&screen[i]))projected=0;
        if(projected)r=gfx_setup_polygon(screen,n,vp,&frame,RAST_VCOLOR_EN,*out,*cap,&st);
        else
#endif
        r = gfx_setup_view(view, n, s_proj, R4_NEAR_Z, R4_FAR_Z, vp, &frame,
                           RAST_VCOLOR_EN, *out, (unsigned)*cap, &st);
        (void)projected;
        if (r == 0 && st.backface != before) {
            rast_view_vertex t = view[1];
            view[1] = view[n - 1u];
            view[n - 1u] = t;
            st.backface = before;
            r = gfx_setup_view(view, n, s_proj, R4_NEAR_Z, R4_FAR_Z, vp, &frame,
                               RAST_VCOLOR_EN, *out, (unsigned)*cap, &st);
        }
    }
    if (r < 0) s_failed = 1;
    else if (r > 0) { *out += (unsigned)r; *cap -= (uint32_t)r; s_n += (uint32_t)r; }
    PROF_END(3);return r;
}

/* 世界系四边形:法线朝向剔除+距离雾 */
static void sh_quad(const int32_t p[4][3], uint32_t col, const rast_viewport *vp,
                    rast_record **out, uint32_t *cap)
{
    const int64_t cx = ((int64_t)p[0][0] + p[2][0]) >> 1,
                  cy = ((int64_t)p[0][1] + p[2][1]) >> 1,
                  cz = ((int64_t)p[0][2] + p[2][2]) >> 1;
    const int64_t dx = cx - s_px, dy = cy - SH_EYE, dz = cz - s_pz;
    rast_view_vertex v[4];
    col=sh_fog(dx*dx+dy*dy+dz*dz,col);
    /* 不做世界空间背面剔除:手写绕向不一致,交给管线屏幕空间剔除+sh_poly 翻绕重试。 */
    for (unsigned i = 0; i < 4u; ++i)
        v[i] = (rast_view_vertex){p[i][0], p[i][1], p[i][2], 0, 0, col};
    (void)sh_poly(v, 4, vp, out, cap);
}

/* Convex six-face solids. Cull hidden faces before setup to keep records bounded. */
static void sh_hexa(const int32_t p[8][3],const uint32_t col[6],const rast_viewport *vp,
                    rast_record **out,uint32_t *cap)
{
    static const uint8_t f[6][4]={{1,5,6,2},{4,0,3,7},{3,7,6,2},{0,4,5,1},{4,5,6,7},{0,1,2,3}};
    int32_t center[3]={0};
    for(unsigned i=0;i<8;++i)for(unsigned k=0;k<3;++k)center[k]+=p[i][k]/8;
    const int32_t eye[3]={s_viewmodel?0:s_px,s_viewmodel?0:SH_EYE,s_viewmodel?0:s_pz};
    for(unsigned i=0;i<6;++i) {
        int32_t q[4][3],v[3],w[3],c[3];int64_t n[3];
        for(unsigned j=0;j<4;++j)for(unsigned k=0;k<3;++k)q[j][k]=p[f[i][j]][k];
        for(unsigned k=0;k<3;++k){v[k]=(q[1][k]-q[0][k])/256;w[k]=(q[2][k]-q[0][k])/256;c[k]=(q[0][k]+q[2][k])/2;}
        n[0]=(int64_t)v[1]*w[2]-(int64_t)v[2]*w[1];
        n[1]=(int64_t)v[2]*w[0]-(int64_t)v[0]*w[2];
        n[2]=(int64_t)v[0]*w[1]-(int64_t)v[1]*w[0];
        int64_t orient=0,visible=0;
        for(unsigned k=0;k<3;++k){orient+=n[k]*((c[k]-center[k])/256);visible+=n[k]*((eye[k]-c[k])/256);}
        if((orient>=0 && visible<=0) || (orient<0 && visible>=0))continue;
        sh_quad(q,col[i],vp,out,cap);
    }
}
static void sh_box(int32_t cx,int32_t cy,int32_t cz,int32_t hx,int32_t hy,int32_t hz,
                   const uint32_t col[6],const rast_viewport *vp,rast_record **out,uint32_t *cap)
{
    const int32_t p[8][3]={{cx-hx,cy-hy,cz-hz},{cx+hx,cy-hy,cz-hz},
        {cx+hx,cy+hy,cz-hz},{cx-hx,cy+hy,cz-hz},
        {cx-hx,cy-hy,cz+hz},{cx+hx,cy-hy,cz+hz},
        {cx+hx,cy+hy,cz+hz},{cx-hx,cy+hy,cz+hz}};
    sh_hexa(p,col,vp,out,cap);
}

/* 红结晶人:腿×2+裙+胸+头+臂+枪,六面三档红 */
static const uint32_t SH_GUN[6] = {0x1a1e26u, 0x1a1e26u, 0x383e4au, 0x0a0b0eu, 0x222832u, 0x0d0f13u};

/* Baked indexed meshes: one face color, shared projected vertices, no runtime loft. */
static void sh_normalize(int32_t v[3])
{
    PROF_START;
    uint32_t maximum=0;
    for(unsigned k=0;k<3;++k) {
        uint32_t a=v[k]<0?(uint32_t)(-(int64_t)v[k]):(uint32_t)v[k];
        if(a>maximum)maximum=a;
    }
    if(!maximum){v[0]=0;v[1]=SH_ONE;v[2]=0;PROF_END(0);return;}
    int shift=0;
    while(maximum>16383u){maximum>>=1;++shift;}
    while(maximum<8192u){maximum<<=1;--shift;}
#if SH_REFERENCE_MATH
    /* Correct reference: no shift/divide precedence ambiguity or s32 reciprocal overflow. */
    int32_t x[3];
    for(unsigned k=0;k<3;++k)x[k]=shift<0?v[k]*(int32_t)(1u<<-shift):v[k];
    const int32_t length=sh_isqrt((int64_t)x[0]*x[0]+(int64_t)x[1]*x[1]+(int64_t)x[2]*x[2]);
    for(unsigned k=0;k<3;++k)v[k]=(int32_t)((int64_t)x[k]*SH_ONE/length);
#else
    /* Scale to a 14-bit mantissa. Squares, reciprocal and final multiply fit
       u32/s32, so RV32 uses hardware DIV/MUL. Direction error is <16 Q16 LSB. */
    int32_t x[3];
    for(unsigned k=0;k<3;++k)x[k]=shift<0?v[k]*(int32_t)(1u<<-shift):v[k]>>shift;
    uint32_t square=(uint32_t)(x[0]*x[0]+x[1]*x[1]+x[2]*x[2]);
    uint32_t length=sh_isqrt32(square),inverse=(1u<<30)/length;
    for(unsigned k=0;k<3;++k)v[k]=(x[k]*(int32_t)inverse)/16384;
#endif
    PROF_END(0);
}

typedef struct {int32_t world[3];rast_vertex screen;uint8_t ready,inside;} sh_mesh_vertex;

static uint32_t sh_face_color(const int8_t normal[3],unsigned material,const int32_t light[3],
                              int32_t fog_distance)
{
    PROF_START;
    static const unsigned base[4][3]={{238,20,7},{176,10,5},{31,38,48},{235,239,243}};
    int32_t diffuse=(normal[0]*light[0]+normal[1]*light[1]+normal[2]*light[2])/127;
    if(diffuse<0)diffuse=0;
    int32_t brightness=28000+sh_mul(diffuse,35000);
    int32_t front=normal[2]>0?normal[2]:0;
    int32_t spec=front*front/127;spec=spec*spec/127;
    unsigned c[3];
    for(unsigned k=0;k<3;++k) {
        int32_t shine=material<2?(k==0?70:k==1?45:30):34;
        int32_t value=(int32_t)base[material][k]*brightness/SH_ONE+spec*shine/127;
        c[k]=(unsigned)(value>255?255:value<0?0:value);
    }
    uint32_t col=sh_rgb(c[0],c[1],c[2]);
    if(fog_distance<=SH_ONE*8/5){PROF_END(1);return col;}
    int32_t f=(fog_distance-SH_ONE*8/5)/8;
    if(f>26000)f=26000;
    f=f*(int32_t)(1020u+6u*((c[0]+c[1]+c[2])/3u))/2550;
    col=sh_rgb(c[0]+(252-(int32_t)c[0])*f/SH_ONE,
                  c[1]+(253-(int32_t)c[1])*f/SH_ONE,
                  c[2]+(255-(int32_t)c[2])*f/SH_ONE);
    PROF_END(1);return col;
}

static void sh_mesh_draw(const int16_t (*positions)[3],unsigned vertices,
                         const uint8_t (*faces)[3],const int8_t (*normals)[3],
                         const uint8_t *materials,unsigned count,int32_t x,int32_t y,int32_t z,
                         int32_t sn,int32_t cs,const rast_viewport *vp,rast_record **out,uint32_t *cap)
{
    PROF_START;
    sh_mesh_vertex v[SH_MESH_VERTICES];
    if(vertices>SH_MESH_VERTICES){s_failed=1;return;}
    const int32_t eye[3]={s_viewmodel?0:s_px,s_viewmodel?0:SH_EYE,s_viewmodel?0:s_pz};
    /* Rotate the light into model space once. Baked face normals need no normalization. */
    int32_t light[3]={sh_mul(-34000,cs)-sh_mul(23000,sn),49000,
                      sh_mul(-34000,sn)+sh_mul(23000,cs)};
    sh_normalize(light);
    int32_t fog_distance=0;
    if(!s_viewmodel){int64_t dx=x-s_px,dz=z-s_pz;
        fog_distance=sh_isqrt(dx*dx+dz*dz+(int64_t)(SH_EYE-SH_ONE)*(SH_EYE-SH_ONE));}
    for(unsigned i=0;i<vertices;++i) {
        int32_t px=positions[i][0]*64,py=positions[i][1]*64,pz=positions[i][2]*64;
        v[i].world[0]=x+sh_mul(px,cs)+sh_mul(pz,sn);v[i].world[1]=y+py;
        v[i].world[2]=z-sh_mul(px,sn)+sh_mul(pz,cs);v[i].ready=0;
    }
    const rast_frame frame={(unsigned)(vp->x+vp->w)>640u?(unsigned)(vp->x+vp->w):640u,480};
    for(unsigned f=0;f<count;++f) {
        const int8_t *n=normals[f];
        int32_t nx=sh_mul(n[0]*SH_ONE,cs)+sh_mul(n[2]*SH_ONE,sn);
        int32_t nz=-sh_mul(n[0]*SH_ONE,sn)+sh_mul(n[2]*SH_ONE,cs);
        const int32_t *a=v[faces[f][0]].world;
        int64_t facing=(int64_t)nx*(eye[0]-a[0])+(int64_t)n[1]*SH_ONE*(eye[1]-a[1])+
                       (int64_t)nz*(eye[2]-a[2]);
        if(facing<=0)continue;
        uint32_t color=sh_face_color(n,materials[f],light,fog_distance);
        rast_vertex screen[3];rast_view_vertex world[3];int inside=1;
        for(unsigned j=0;j<3;++j) {
            sh_mesh_vertex *t=&v[faces[f][j]];
            world[j]=(rast_view_vertex){t->world[0],t->world[1],t->world[2],0,0,color};
#if !SH_DISABLE_VERTEX_CACHE
            if(!t->ready) {
                PROF_START;
                int32_t q[3];sh_transform(t->world,q);
                rast_view_vertex view={q[0],q[1],q[2],0,0,0};
                t->inside=sh_project_vertex(&view,&t->screen)==0;t->ready=1;
                PROF_END(2);
            }
            screen[j]=t->screen;screen[j].color=color;inside&=t->inside;
#else
            inside=0;
#endif
        }
        if(!inside){sh_poly(world,3,vp,out,cap);continue;}
        if(*cap<3u){s_failed=1;return;}
        uint32_t setup_started=0;
#if SH_PROFILE
        setup_started=s_clock?s_clock():0;
#endif
        rast_setup_stats stats={0};
        int result=sh_flat_setup(screen,vp,*out);
        if(result== -3)result=gfx_setup_polygon(screen,3,vp,&frame,RAST_VCOLOR_EN,*out,*cap,&stats);
        if(result==0 && stats.backface){rast_vertex tmp=screen[1];screen[1]=screen[2];screen[2]=tmp;
            result=gfx_setup_polygon(screen,3,vp,&frame,RAST_VCOLOR_EN,*out,*cap,&stats);}
        if(result<0){s_failed=1;return;}
        *out+=(unsigned)result;*cap-=(unsigned)result;s_n+=(unsigned)result;
#if SH_PROFILE
        if(s_clock){++s_profile[6];s_profile[7]+=s_clock()-setup_started;}
#else
        (void)setup_started;
#endif
    }
    PROF_END(5);
}

static void sh_enemy_draw(const sh_enemy *e,const rast_viewport *vp,rast_record **out,uint32_t *cap)
{
    const fx16_vec4 center={{e->x,SH_ONE*9/10,e->z,SH_ONE}};
    fx16_vec4 camera;fx16_mat_vec(&s_view,&center,&camera);
    const int32_t radius=SH_ONE*3/2,forward=-camera.v[2];
    if(forward< -radius || forward>(int32_t)R4_FAR_Z+radius)return;
    int64_t ax=camera.v[0]<0?-(int64_t)camera.v[0]:camera.v[0];
    int64_t ay=camera.v[1]<0?-(int64_t)camera.v[1]:camera.v[1];
    if(ax*s_proj->v[0]>(int64_t)forward*SH_ONE+(int64_t)radius*(s_proj->v[0]+SH_ONE) ||
       ay*s_proj->v[5]>(int64_t)forward*SH_ONE+(int64_t)radius*(s_proj->v[5]+SH_ONE))return;
    int32_t dx=s_px-e->x,dz=s_pz-e->z,len=sh_isqrt((int64_t)dx*dx+(int64_t)dz*dz);
    int32_t sn=len?(int32_t)((int64_t)dx*SH_ONE/len):0,cs=len?(int32_t)((int64_t)dz*SH_ONE/len):SH_ONE;
    int swing=e->move?fx_sin((uint16_t)e->ph)*16/16384:0;
    if(swing< -16)swing=-16;
    if(swing>16)swing=16;
    sh_mesh_draw(sh_mesh_vertices[swing+16],SH_MESH_VERTICES,sh_mesh_faces,sh_mesh_normals[swing+16],
                 sh_mesh_materials,SH_MESH_FACES,e->x,0,e->z,sn,cs,vp,out,cap);
}

/* Furniture keeps its bright bevels with four-sided, flat-shaded rings. */
static void sh_bevel(int32_t x,int32_t y,int32_t z,int hx,int hy,int hz,unsigned material,
                     const rast_viewport *vp,rast_record **out,uint32_t *cap)
{
    int32_t rings[4][4][3];
    static const int signs[4][2]={{-1,-1},{1,-1},{1,1},{-1,1}};
    static const uint32_t colors[4]={0xcdd2d9u,0xe7ebefu,0xf7f9fbu,0xffffffu};
    for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j) {
        int bevel=(i==0u || i==3u)?2:0;
        rings[i][j][0]=x+signs[j][0]*(hx-bevel)*SH_ONE/100;
        rings[i][j][1]=y+(i==0u?-hy:i==1u?-hy+2:i==2u?hy-2:hy)*SH_ONE/100;
        rings[i][j][2]=z+signs[j][1]*(hz-bevel)*SH_ONE/100;
    }
    (void)material;
    for(unsigned i=0;i<3;++i)for(unsigned j=0;j<4;++j) {
        unsigned next=(j+1u)%4u;
        int32_t q[4][3];
        memcpy(q[0],rings[i][j],sizeof q[0]);memcpy(q[1],rings[i+1][j],sizeof q[0]);
        memcpy(q[2],rings[i+1][next],sizeof q[0]);memcpy(q[3],rings[i][next],sizeof q[0]);
        sh_quad(q,colors[i==2u?3u:j%2u+1u],vp,out,cap);
    }
    sh_quad(rings[0],colors[0],vp,out,cap);sh_quad(rings[3],colors[3],vp,out,cap);
}

static void sh_ground_shadow(int32_t x,int32_t z,int32_t rx,int32_t rz,
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

uint32_t sh_world_emit(const fx16_mat4 *projection, const rast_viewport *vp,
                       rast_record *out, uint32_t cap)
{
#if SH_PROFILE
    memset(s_profile,0,sizeof s_profile);
#endif
    const int32_t sy = sh_sin(s_yaw), cy = sh_cos(s_yaw);
    fx16_mat4 view;
    rast_record *o = out;
    uint32_t left = cap;

    /* 视图(无俯仰):行0=(-cy,0,sy) 行1=(0,1,0) 行2=(-sy,0,-cy);平移=-行·相机 */
    view.v[0] = -cy; view.v[1] = 0; view.v[2] = sy;
    const int32_t cp=sh_cos((uint16_t)s_pitch),sp=sh_sin((uint16_t)s_pitch);
    view.v[4] = -sh_mul(sy,sp); view.v[5] = cp; view.v[6] = -sh_mul(cy,sp);
    view.v[8] = -sh_mul(sy,cp); view.v[9] = -sp; view.v[10] = -sh_mul(cy,cp);
    view.v[12] = 0; view.v[13] = 0; view.v[14] = 0; view.v[15] = FX16_ONE;
    view.v[3] = (int32_t)(((int64_t)cy * s_px - (int64_t)sy * s_pz) >> 16);
    view.v[7] = -sh_mul(view.v[4],s_px)-sh_mul(cp,SH_EYE)-sh_mul(view.v[6],s_pz);
    view.v[11] = -sh_mul(view.v[8],s_px)+sh_mul(sp,SH_EYE)-sh_mul(view.v[10],s_pz);
    s_view = view;
    s_proj = projection;s_mesh_vp=vp;
    s_failed = 0; s_n = 0;s_viewmodel=0;

#if !SH_DISABLE_SCENE_CACHE
    int room_hit=s_room_cache.valid && s_room_cache.x==s_px && s_room_cache.z==s_pz &&
        s_room_cache.yaw==s_yaw && s_room_cache.pitch==s_pitch &&
        !memcmp(&s_room_cache.projection,projection,sizeof *projection) &&
        !memcmp(&s_room_cache.vp,vp,sizeof *vp);
    if(room_hit) {
        if(left<s_room_cache.count)return UINT32_MAX;
        memcpy(o,s_room_cache.records,s_room_cache.count*sizeof *o);
        o+=s_room_cache.count;left-=s_room_cache.count;s_n=s_room_cache.count;
    } else {
        ++s_room_cache.version;
#endif
    /* ---- 房间:地板棋盘/天花板/横梁+灯带/四墙/门洞内芯/窗带/黑挂板 ---- */
    for (int i = 0; i < 5; ++i) for (int j = 0; j < 4; ++j) {
        const int32_t x0 = (-5 + i * 2) * SH_ONE, z0 = (-4 + j * 2) * SH_ONE;
        const int32_t q[4][3] = {{x0, 0, z0}, {x0, 0, z0 + 2 * SH_ONE},
                                 {x0 + 2 * SH_ONE, 0, z0 + 2 * SH_ONE}, {x0 + 2 * SH_ONE, 0, z0}};
        const unsigned t = (unsigned)((i + j) & 1u);
        const unsigned edge = (i == 0 || i == 4 || j == 0 || j == 3) ? 20u : 0u;
        sh_quad(q, sh_rgb((t ? 240u : 237u) - edge/2, (t ? 242u : 239u) - edge/2,
                          (t ? 245u : 242u) - edge/2), vp, &o, &left);
    }
    { const int32_t q[4][3] = {{-5 * SH_ONE, 3 * SH_ONE, -4 * SH_ONE},
                               {-5 * SH_ONE, 3 * SH_ONE, 4 * SH_ONE},
                               {5 * SH_ONE, 3 * SH_ONE, 4 * SH_ONE},
                               {5 * SH_ONE, 3 * SH_ONE, -4 * SH_ONE}};
      sh_quad(q, sh_rgb(236, 239, 243), vp, &o, &left); }
    for (unsigned k = 0; k < 2u; ++k) {
        const int32_t bx = (k ? 16 : -16) * SH_ONE / 10;
        const int32_t bot[4][3] = {{bx - 14 * SH_ONE / 100, 284 * SH_ONE / 100, -4 * SH_ONE},
                                   {bx + 14 * SH_ONE / 100, 284 * SH_ONE / 100, -4 * SH_ONE},
                                   {bx + 14 * SH_ONE / 100, 284 * SH_ONE / 100, 4 * SH_ONE},
                                   {bx - 14 * SH_ONE / 100, 284 * SH_ONE / 100, 4 * SH_ONE}};
        const int32_t wl[4][3] = {{bx - 14 * SH_ONE / 100, 284 * SH_ONE / 100, -4 * SH_ONE},
                                  {bx - 14 * SH_ONE / 100, 3 * SH_ONE, -4 * SH_ONE},
                                  {bx - 14 * SH_ONE / 100, 3 * SH_ONE, 4 * SH_ONE},
                                  {bx - 14 * SH_ONE / 100, 284 * SH_ONE / 100, 4 * SH_ONE}};
        const int32_t wr[4][3] = {{bx + 14 * SH_ONE / 100, 284 * SH_ONE / 100, 4 * SH_ONE},
                                  {bx + 14 * SH_ONE / 100, 3 * SH_ONE, 4 * SH_ONE},
                                  {bx + 14 * SH_ONE / 100, 3 * SH_ONE, -4 * SH_ONE},
                                  {bx + 14 * SH_ONE / 100, 284 * SH_ONE / 100, -4 * SH_ONE}};
        const int32_t lt[4][3] = {{bx - 9 * SH_ONE / 100, 2838 * SH_ONE / 1000, -4 * SH_ONE},
                                  {bx + 9 * SH_ONE / 100, 2838 * SH_ONE / 1000, -4 * SH_ONE},
                                  {bx + 9 * SH_ONE / 100, 2838 * SH_ONE / 1000, 4 * SH_ONE},
                                  {bx - 9 * SH_ONE / 100, 2838 * SH_ONE / 1000, 4 * SH_ONE}};
        sh_quad(bot, sh_rgb(228, 231, 236), vp, &o, &left);
        sh_quad(wl, sh_rgb(220, 223, 228), vp, &o, &left);
        sh_quad(wr, sh_rgb(222, 225, 230), vp, &o, &left);
        sh_quad(lt, sh_rgb(251, 252, 255), vp, &o, &left);
    }
    { /* 四墙(中段平面)+门洞内芯+窗带+黑挂板 */
        const int32_t wn[4][3] = {{-5 * SH_ONE, 0, -4 * SH_ONE}, {5 * SH_ONE, 0, -4 * SH_ONE},
                                  {5 * SH_ONE, 3 * SH_ONE, -4 * SH_ONE}, {-5 * SH_ONE, 3 * SH_ONE, -4 * SH_ONE}};
        const int32_t ws[4][3] = {{5 * SH_ONE, 0, 4 * SH_ONE}, {-5 * SH_ONE, 0, 4 * SH_ONE},
                                  {-5 * SH_ONE, 3 * SH_ONE, 4 * SH_ONE}, {5 * SH_ONE, 3 * SH_ONE, 4 * SH_ONE}};
        const int32_t ww[4][3] = {{-5 * SH_ONE, 0, 4 * SH_ONE}, {-5 * SH_ONE, 0, -4 * SH_ONE},
                                  {-5 * SH_ONE, 3 * SH_ONE, -4 * SH_ONE}, {-5 * SH_ONE, 3 * SH_ONE, 4 * SH_ONE}};
        const int32_t we[4][3] = {{5 * SH_ONE, 0, -4 * SH_ONE}, {5 * SH_ONE, 0, 4 * SH_ONE},
                                  {5 * SH_ONE, 3 * SH_ONE, 4 * SH_ONE}, {5 * SH_ONE, 3 * SH_ONE, -4 * SH_ONE}};
        sh_quad(wn, sh_rgb(238, 241, 245), vp, &o, &left);
        sh_quad(ws, sh_rgb(221, 224, 229), vp, &o, &left);
        sh_quad(ww, sh_rgb(232, 235, 240), vp, &o, &left);
        sh_quad(we, sh_rgb(226, 229, 234), vp, &o, &left);
        const int32_t doors[3][4][3] = {
            {{-382 * SH_ONE / 100, 0, -394 * SH_ONE / 100}, {-258 * SH_ONE / 100, 0, -394 * SH_ONE / 100},
             {-258 * SH_ONE / 100, 225 * SH_ONE / 100, -394 * SH_ONE / 100},
             {-382 * SH_ONE / 100, 225 * SH_ONE / 100, -394 * SH_ONE / 100}},
            {{258 * SH_ONE / 100, 0, -394 * SH_ONE / 100}, {382 * SH_ONE / 100, 0, -394 * SH_ONE / 100},
             {382 * SH_ONE / 100, 225 * SH_ONE / 100, -394 * SH_ONE / 100},
             {258 * SH_ONE / 100, 225 * SH_ONE / 100, -394 * SH_ONE / 100}},
            {{494 * SH_ONE / 100, 0, -12 * SH_ONE / 100}, {494 * SH_ONE / 100, 0, 112 * SH_ONE / 100},
             {494 * SH_ONE / 100, 225 * SH_ONE / 100, 112 * SH_ONE / 100},
             {494 * SH_ONE / 100, 225 * SH_ONE / 100, -12 * SH_ONE / 100}}};
        for (unsigned d = 0; d < 3u; ++d) sh_quad(doors[d], sh_rgb(161, 170, 181), vp, &o, &left);
        for (unsigned w2 = 0; w2 < 2u; ++w2) {
            const int32_t wz = (w2 ? 150 : -130) * SH_ONE / 100;
            const int32_t f[4][3] = {{-4985 * SH_ONE / 1000, 110 * SH_ONE / 100, wz - 55 * SH_ONE / 100},
                                     {-4985 * SH_ONE / 1000, 110 * SH_ONE / 100, wz + 55 * SH_ONE / 100},
                                     {-4985 * SH_ONE / 1000, 240 * SH_ONE / 100, wz + 55 * SH_ONE / 100},
                                     {-4985 * SH_ONE / 1000, 240 * SH_ONE / 100, wz - 55 * SH_ONE / 100}};
            const int32_t g[4][3] = {{-496 * SH_ONE / 100, 119 * SH_ONE / 100, wz - 47 * SH_ONE / 100},
                                     {-496 * SH_ONE / 100, 119 * SH_ONE / 100, wz + 47 * SH_ONE / 100},
                                     {-496 * SH_ONE / 100, 231 * SH_ONE / 100, wz + 47 * SH_ONE / 100},
                                     {-496 * SH_ONE / 100, 231 * SH_ONE / 100, wz - 47 * SH_ONE / 100}};
            sh_quad(f, sh_rgb(202, 208, 216), vp, &o, &left);
            sh_quad(g, sh_rgb(250, 252, 255), vp, &o, &left);
        }
        { const int32_t p[4][3] = {{15 * SH_ONE / 100, 126 * SH_ONE / 100, -393 * SH_ONE / 100},
                                   {65 * SH_ONE / 100, 126 * SH_ONE / 100, -393 * SH_ONE / 100},
                                   {65 * SH_ONE / 100, 198 * SH_ONE / 100, -393 * SH_ONE / 100},
                                   {15 * SH_ONE / 100, 198 * SH_ONE / 100, -393 * SH_ONE / 100}};
          sh_quad(p, sh_rgb(26, 28, 32), vp, &o, &left); }
        { const int32_t p[4][3] = {{493 * SH_ONE / 100, 122 * SH_ONE / 100, 180 * SH_ONE / 100},
                                   {493 * SH_ONE / 100, 122 * SH_ONE / 100, 280 * SH_ONE / 100},
                                   {493 * SH_ONE / 100, 188 * SH_ONE / 100, 280 * SH_ONE / 100},
                                   {493 * SH_ONE / 100, 188 * SH_ONE / 100, 180 * SH_ONE / 100}};
          sh_quad(p, sh_rgb(24, 26, 30), vp, &o, &left); }
    }
    for(unsigned i=0;i<sizeof s_solid/sizeof *s_solid;++i)
        sh_ground_shadow(s_solid[i].x+SH_ONE/12,s_solid[i].z+SH_ONE/12,
                         s_solid[i].hx+SH_ONE/8,s_solid[i].hz+SH_ONE/8,vp,&o,&left);
    /* ---- 家具:方柱×2/沙发/茶几×3 ---- */
    { const uint32_t side[6] = {0xd8dce2u, 0xd8dce2u, 0xf0f2f5u, 0xc0c4cau, 0xeceef2u, 0xdfe2e8u};
      const uint32_t soft[6] = {0xe4e7ecu, 0xe4e7ecu, 0xf4f6f9u, 0xb8bcc4u, 0xe8ebf0u, 0xd4d8deu};
      sh_box(-26 * SH_ONE / 10, 15 * SH_ONE / 10, 20 * SH_ONE / 10,
             30 * SH_ONE / 100, 15 * SH_ONE / 10, 30 * SH_ONE / 100, side, vp, &o, &left);
      sh_box(27 * SH_ONE / 10, 15 * SH_ONE / 10, -19 * SH_ONE / 10,
             30 * SH_ONE / 100, 15 * SH_ONE / 10, 30 * SH_ONE / 100, side, vp, &o, &left);
      sh_box(-442 * SH_ONE / 100, 21 * SH_ONE / 100, 25 * SH_ONE / 100,
             50 * SH_ONE / 100, 21 * SH_ONE / 100, 42 * SH_ONE / 100, soft, vp, &o, &left);
      sh_box(-458 * SH_ONE / 100, 56 * SH_ONE / 100, 25 * SH_ONE / 100,
             12 * SH_ONE / 100, 35 * SH_ONE / 100, 42 * SH_ONE / 100, soft, vp, &o, &left);
      sh_bevel(-31*SH_ONE/10,23*SH_ONE/100,25*SH_ONE/100,26,23,26,3,vp,&o,&left);
      sh_bevel(19*SH_ONE/10,31*SH_ONE/100,17*SH_ONE/10,34,31,34,3,vp,&o,&left);
      sh_bevel(-14*SH_ONE/10,27*SH_ONE/100,-16*SH_ONE/10,36,27,36,3,vp,&o,&left);
    }
    /* ---- 吊灯×3:黑杆+黑锥罩+暖光底 ---- */
    { const uint32_t lc[6] = {0x1e2026u, 0x1e2026u, 0x363a40u, 0x0e0f12u, 0x2a2d34u, 0x141519u};
      static const int16_t lx[3] = {-19, 16, 2}, lz[3] = {-12, 5, 23};
      static const uint8_t ld[3] = {55, 75, 40};
      for (unsigned i = 0; i < 3u; ++i) {
          const int32_t x = lx[i] * SH_ONE / 10, z = lz[i] * SH_ONE / 10;
          const int32_t top = 3 * SH_ONE - ld[i] * SH_ONE / 100;
          sh_box(x, (3 * SH_ONE + top) / 2, z, SH_ONE / 80, (3 * SH_ONE - top) / 2, SH_ONE / 80,
                 lc, vp, &o, &left);
          sh_box(x, top - 13 * SH_ONE / 100, z, 9 * SH_ONE / 100, 13 * SH_ONE / 100, 9 * SH_ONE / 100,
                 lc, vp, &o, &left);
          { const int32_t g[4][3] = {
                {x - 8 * SH_ONE / 100, top - 26 * SH_ONE / 100, z - 8 * SH_ONE / 100},
                {x + 8 * SH_ONE / 100, top - 26 * SH_ONE / 100, z - 8 * SH_ONE / 100},
                {x + 8 * SH_ONE / 100, top - 26 * SH_ONE / 100, z + 8 * SH_ONE / 100},
                {x - 8 * SH_ONE / 100, top - 26 * SH_ONE / 100, z + 8 * SH_ONE / 100}};
            sh_quad(g, sh_rgb(255, 244, 220), vp, &o, &left); }
      }
    }
#if !SH_DISABLE_SCENE_CACHE
        s_room_cache.valid=s_room_cache.records && s_n<=1024u && !s_failed;
        if(s_room_cache.valid) {
            s_room_cache.x=s_px;s_room_cache.z=s_pz;s_room_cache.yaw=s_yaw;s_room_cache.pitch=s_pitch;
            s_room_cache.projection=*projection;s_room_cache.vp=*vp;s_room_cache.count=s_n;
            memcpy(s_room_cache.records,out,s_n*sizeof *out);
        }
    }
#endif
    /* ---- 落影→敌人→弹→碎块→曳光 ---- */
    for (unsigned i = 0; i < SH_ENEMY_MAX; ++i) if (s_enemy[i].alive) {
#if !SH_DISABLE_SCENE_CACHE
        const int32_t pose=s_enemy[i].move?fx_sin((uint16_t)s_enemy[i].ph)*16/16384:0;
        if(s_actor_cache[i].valid && s_actor_cache[i].x==s_enemy[i].x && s_actor_cache[i].z==s_enemy[i].z &&
           s_actor_cache[i].swing==pose && s_actor_cache[i].version==s_room_cache.version) {
            uint32_t n=s_actor_cache[i].count;if(n>left)return UINT32_MAX;
            memcpy(o,s_actor_cache[i].records,n*sizeof *o);o+=n;left-=n;s_n+=n;continue;
        }
        rast_record *actor_begin=o;uint32_t before=s_n;
#endif
        const int32_t x = s_enemy[i].x, z = s_enemy[i].z;
        sh_ground_shadow(x+SH_ONE/15,z+SH_ONE/15,SH_ONE*42/100,SH_ONE*30/100,vp,&o,&left);
        sh_enemy_draw(&s_enemy[i], vp, &o, &left);
#if !SH_DISABLE_SCENE_CACHE
        uint32_t n=s_n-before;s_actor_cache[i].valid=s_actor_cache[i].records && n<=512u && !s_failed;
        if(s_actor_cache[i].valid) {
            s_actor_cache[i].x=s_enemy[i].x;s_actor_cache[i].z=s_enemy[i].z;
            s_actor_cache[i].swing=pose;s_actor_cache[i].version=s_room_cache.version;s_actor_cache[i].count=n;
            memcpy(s_actor_cache[i].records,actor_begin,n*sizeof *actor_begin);
        }
#endif
    }
    for(unsigned i=0;i<SH_ENEMY_MAX;++i)if(s_drop[i].alive) {
        const sh_drop *d=&s_drop[i];
        sh_box(d->x,SH_ONE/12,d->z,SH_ONE/6,SH_ONE/22,SH_ONE/16,SH_GUN,vp,&o,&left);
        sh_box(d->x-SH_ONE/10,SH_ONE/18,d->z+SH_ONE/15,SH_ONE/22,SH_ONE/25,SH_ONE/10,SH_GUN,vp,&o,&left);
    }
    for(unsigned i=0;i<SH_BULLET_MAX;++i)if(s_bullet[i].alive) {
        const sh_bullet *b=&s_bullet[i];
        const int32_t rx=-cy*2/100,rz=sy*2/100;
        /* Velocity-aligned trail with a camera-facing cross section, visible at every yaw. */
        const int32_t q[4][3]={{b->x+rx,b->y+SH_ONE/100,b->z+rz},
            {b->x-rx,b->y-SH_ONE/100,b->z-rz},
            {b->x-b->vx*2-rx,b->y-b->vy*2-SH_ONE/100,b->z-b->vz*2-rz},
            {b->x-b->vx*2+rx,b->y-b->vy*2+SH_ONE/100,b->z-b->vz*2+rz}};
        sh_quad(q,b->player?0x272b32u:0xbe180cu,vp,&o,&left);
        const int32_t head[4][3]={{b->x+rx,b->y,b->z+rz},{b->x,b->y+SH_ONE/25,b->z},
            {b->x-rx,b->y,b->z-rz},{b->x,b->y-SH_ONE/25,b->z}};
        sh_quad(head,0xff9f4au,vp,&o,&left);
    }
    for (unsigned i = 0; i < SH_SHARD_MAX; ++i) if (s_shard[i].alive) {
        /* Cosmetic shards yield their records to gameplay geometry and the gun.
           Collision pools keep updating; no enemy or live round is omitted. */
        if(left<64u)break;
        const sh_shard *s = &s_shard[i];
        const int32_t c = sh_sin(s->rot), n2 = sh_cos(s->rot);
        const rast_view_vertex vv[3] = {
            {s->x + (int32_t)((int64_t)c * 6 / 100), s->y,
             s->z + (int32_t)((int64_t)n2 * 6 / 100), 0, 0, sh_rgb(224, 42, 26)},
            {s->x - (int32_t)((int64_t)c * 4 / 100), s->y + 4 * SH_ONE / 100,
             s->z - (int32_t)((int64_t)n2 * 4 / 100), 0, 0, sh_rgb(150, 20, 12)},
            {s->x, s->y - 3 * SH_ONE / 100, s->z, 0, 0, sh_rgb(236, 52, 30)}};
        sh_poly(vv, 3, vp, &o, &left);
    }
    for (unsigned i = 0; i < SH_TRACE_MAX; ++i) if (s_trace[i].alive) {
        const sh_trace *t = &s_trace[i];
        const int32_t rx = -sh_sin(s_yaw) * 3 / 100, rz = sh_cos(s_yaw) * 3 / 100;
        const int32_t q[4][3] = {{t->ax + rx, t->ay, t->az + rz},
                                 {t->ax - rx, t->ay, t->az - rz},
                                 {t->bx - rx, t->by, t->bz - rz},
                                 {t->bx + rx, t->by, t->bz + rz}};
        sh_quad(q, sh_rgb(255, 80, 50), vp, &o, &left);
    }
    s_weapon_start=s_n;
#if !SH_DISABLE_SCENE_CACHE
    sh_weapon_cache *save=NULL;
    if(s_weapon_cache) {
        uint32_t oldest=UINT32_MAX;
        for(unsigned i=0;i<SH_WEAPON_CACHE_SLOTS;++i) {
            sh_weapon_cache *entry=&s_weapon_cache[i];
            if(entry->valid && entry->recoil==s_recoil && entry->reload==(s_reload!=0) && entry->flash==(s_flash!=0) &&
               !memcmp(&entry->projection,projection,sizeof *projection) && !memcmp(&entry->vp,vp,sizeof *vp)) {
                if(entry->count>left)return UINT32_MAX;
                memcpy(o,entry->records,entry->count*sizeof *o);s_n+=entry->count;
                entry->age=++s_weapon_cache_age;return s_failed?UINT32_MAX:s_n;
            }
            if(!entry->valid){save=entry;oldest=0;}
            else if(oldest && entry->age<oldest){oldest=entry->age;save=entry;}
        }
    }
    rast_record *weapon_begin=o;
#endif
    {
        s_weapon_start=s_n;
        fx16_mat4 world_view=s_view;
        memset(&s_view,0,sizeof s_view);
        s_view.v[0]=s_view.v[5]=s_view.v[10]=s_view.v[15]=SH_ONE;s_viewmodel=1;
        const int32_t kick=s_recoil/25,lower=s_reload?SH_ONE/8:0;
        sh_mesh_draw(sh_gun_vertices,SH_GUN_VERTICES,sh_gun_faces,sh_gun_normals,
                     sh_gun_materials,SH_GUN_FACES,0,kick-lower,kick,0,SH_ONE,vp,&o,&left);
        if(s_flash) {
            const uint32_t flash[6]={0xffcc73u,0xffcc73u,0xffffedu,0xffdf9du,0xffffdau,0xffb74au};
            sh_box(24*SH_ONE/100,-26*SH_ONE/100,-75*SH_ONE/100,
                SH_ONE/20,SH_ONE/16,SH_ONE/20,flash,vp,&o,&left);
        }
        s_view=world_view;s_viewmodel=0;
    }
#if !SH_DISABLE_SCENE_CACHE
    if(save && !s_failed && s_n-s_weapon_start<=256u) {
        save->count=s_n-s_weapon_start;save->recoil=s_recoil;save->reload=s_reload!=0;save->flash=s_flash!=0;
        save->projection=*projection;save->vp=*vp;save->age=++s_weapon_cache_age;
        memcpy(save->records,weapon_begin,save->count*sizeof *weapon_begin);save->valid=1;
    }
#endif
    return s_failed ? UINT32_MAX : s_n;
}

/* Small bitmap HUD belongs to this scene. It writes only visible software pixels. */
static const uint8_t sh_font[36][7]={
 {14,17,19,21,25,17,14},{4,12,4,4,4,4,14},{14,17,1,2,4,8,31},{30,1,1,14,1,1,30},
 {2,6,10,18,31,2,2},{31,16,16,30,1,1,30},{14,16,16,30,17,17,14},{31,1,2,4,8,8,8},
 {14,17,17,14,17,17,14},{14,17,17,15,1,1,14},
 {14,17,17,31,17,17,17},{30,17,17,30,17,17,30},{14,17,16,16,16,17,14},
 {30,17,17,17,17,17,30},{31,16,16,30,16,16,31},{31,16,16,30,16,16,16},
 {14,17,16,23,17,17,15},{17,17,17,31,17,17,17},{14,4,4,4,4,4,14},
 {7,2,2,2,18,18,12},{17,18,20,24,20,18,17},{16,16,16,16,16,16,31},
 {17,27,21,21,17,17,17},{17,25,21,19,17,17,17},{14,17,17,17,17,17,14},
 {30,17,17,30,16,16,16},{14,17,17,17,21,18,13},{30,17,17,30,20,18,17},
 {15,16,16,14,1,1,30},{31,4,4,4,4,4,4},{17,17,17,17,17,17,14},
 {17,17,17,17,17,10,4},{17,17,17,21,21,21,10},{17,17,10,4,10,17,17},
 {17,17,10,4,4,4,4},{31,1,2,4,8,16,31}
};
static void sh_rect(uint32_t *p,unsigned w,unsigned h,unsigned stride,int x,int y,int rw,int rh,uint32_t c)
{
    for(int j=0;j<rh;++j)if(y+j>=0 && (unsigned)(y+j)<h)
        for(int i=0;i<rw;++i)if(x+i>=0 && (unsigned)(x+i)<w)p[(size_t)(y+j)*stride+(unsigned)(x+i)]=c;
}
static void sh_text(uint32_t *p,unsigned w,unsigned h,unsigned stride,int x,int y,int scale,const char *s,uint32_t c)
{
    for(;*s;++s,x+=6*scale) {
        int idx=*s>='0' && *s<='9'?*s-'0':*s>='A' && *s<='Z'?*s-'A'+10:-1;
        if(idx>=0)for(int row=0;row<7;++row)for(int col=0;col<5;++col)
            if(sh_font[idx][row]&(16u>>col))sh_rect(p,w,h,stride,x+col*scale,y+row*scale,scale,scale,c);
        if(*s=='.')sh_rect(p,w,h,stride,x,y+6*scale,scale,scale,c);
    }
}
void sh_world_overlay(uint32_t *p,unsigned w,unsigned h,unsigned stride)
{
    char text[48];
    if(!p || w>stride || w<320 || h<200)return;
    const uint32_t ink=0x24272du,red=0xc62d1du;
    sh_text(p,w,h,stride,24,22,3,"HALTTIME",ink);
    snprintf(text,sizeof text,"ENDLESS  KILLS %u  SCORE %u",(unsigned)s_kills,(unsigned)s_score);
    sh_text(p,w,h,stride,25,51,1,text,ink);
    sh_rect(p,w,h,stride,25,70,132,4,0xcbd0d5u);
    sh_rect(p,w,h,stride,25,70,(int)((int64_t)s_ts*132/SH_ONE),4,red);
    snprintf(text,sizeof text,"TIME %u.%02u",(unsigned)(s_ts/SH_ONE),(unsigned)((int64_t)s_ts*100/SH_ONE%100));
    sh_text(p,w,h,stride,25,82,1,text,ink);
    snprintf(text,sizeof text,"AMMO %u",s_ammo);
    sh_text(p,w,h,stride,(int)w-122,24,2,text,ink);
    for(unsigned i=0;i<6;++i)sh_rect(p,w,h,stride,(int)w-121+(int)i*15,45,8,18,i<s_ammo?ink:0xb8bec6u);
    {
        const int cx=(int)w/2,cy=(int)h/2;
        sh_rect(p,w,h,stride,cx-7,cy-1,15,3,0xffffffu);
        sh_rect(p,w,h,stride,cx-1,cy-7,3,15,0xffffffu);
        sh_rect(p,w,h,stride,cx-6,cy,4,1,s_hitmark?red:ink);
        sh_rect(p,w,h,stride,cx+3,cy,4,1,s_hitmark?red:ink);
        sh_rect(p,w,h,stride,cx,cy-6,1,4,s_hitmark?red:ink);
        sh_rect(p,w,h,stride,cx,cy+3,1,4,s_hitmark?red:ink);
        if(s_reload)sh_text(p,w,h,stride,cx-54,cy+33,2,"RELOADING",ink);
    }
    sh_rect(p,w,h,stride,0,(int)h-40,(int)w,40,0xf0f1f3u);
    sh_text(p,w,h,stride,24,(int)h-31,1,"WASD MOVE   MOUSE AIM   HOLD CLICK FIRE   R RELOAD   E PICK UP GUN",ink);
    sh_text(p,w,h,stride,24,(int)h-16,1,"INVINCIBLE   ENDLESS ENEMIES   RIGHT CLICK RELEASE MOUSE   ESC MENU",ink);
    sh_text(p,w,h,stride,(int)w-122,78,1,"INVINCIBLE",red);
}
