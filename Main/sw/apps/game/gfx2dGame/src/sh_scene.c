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
#include "core/fixmath.h"
#include "core/xorshift32.h"
#include "raster_setup_port.h"

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
static uint32_t sh_fog(int64_t d2,uint32_t col)
{
    int64_t f=((int64_t)sh_isqrt(d2)-SH_ONE*8/5)*SH_ONE/(SH_ONE*8);
    unsigned cr=(col>>16)&255u,cg=(col>>8)&255u,cb=col&255u;
    if(f<=0 || s_viewmodel)return col;
    if(f>26000)f=26000;
    f=f*(1020000u+6000u*((cr+cg+cb)/3u))/2550000u;
    return sh_rgb((unsigned)(cr+(252-(int)cr)*f/SH_ONE),
                  (unsigned)(cg+(253-(int)cg)*f/SH_ONE),(unsigned)(cb+(255-(int)cb)*f/SH_ONE));
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
    s_seed=seed?seed:1u;s_rng=s_seed;
    memset(s_enemy,0,sizeof s_enemy);memset(s_bullet,0,sizeof s_bullet);
    memset(s_shard,0,sizeof s_shard);memset(s_trace,0,sizeof s_trace);memset(s_drop,0,sizeof s_drop);
    s_px=0;s_pz=26*SH_ONE/10;s_yaw=32768u;s_pitch=0;s_ts=SH_TS_MIN;
    s_spawned=0;s_shield=1;s_ammo=6;s_cool=s_spawn=s_recoil=s_look=0;
    s_flash=s_hitmark=0;
    sh_spawn_enemy();sh_spawn_enemy();sh_spawn_enemy();
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
            e->alive=0;
            if(sh_free(e->x+mx,e->z,SH_ONE/4,1))e->x+=mx;
            if(sh_free(e->x,e->z+mz,SH_ONE/4,1))e->z+=mz;
            e->alive=1;e->ph+=(uint32_t)(wdt/10);
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
uint32_t sh_score(void){return s_score;}
uint32_t sh_shield(void){return s_shield;}
uint32_t sh_ticks(void){return s_ticks;}
uint32_t sh_world_weapon_start(void){return s_weapon_start;}

/* ---- 发射 ---- */

static int sh_poly(const rast_view_vertex *v, unsigned n, const rast_viewport *vp,
                    rast_record **out, uint32_t *cap)
{
    rast_view_vertex view[4];
    int r;
    if (n > 4u || *cap < n) { if (*cap < n) s_failed = 1; return -1; }
    /* 契约:gfx_setup_view 吃视空间顶点(近/远裁剪按它做),投影矩阵单独传。
       世界系输入在此经视图矩阵变换,与走廊/山脊同口径。 */
    for (unsigned i = 0; i < n; ++i) {
        const fx16_vec4 in = {{v[i].x, v[i].y, v[i].z, FX16_ONE}};
        fx16_vec4 o;
        fx16_mat_vec(&s_view, &in, &o);
        view[i] = (rast_view_vertex){o.v[0], o.v[1], o.v[2], v[i].u, v[i].v, v[i].color};
    }
    {
        const rast_frame frame = {(unsigned)(vp->x + vp->w) > RAST_WIDTH
                                      ? (unsigned)(vp->x + vp->w) : RAST_WIDTH, RAST_HEIGHT};
        /* 场景面是手写绕向,不保证一致:首试被背面剔除即翻绕序重试(ridge_poly 同款)。 */
        rast_setup_stats st = {0};
        const uint32_t before = st.backface;
        r = gfx_setup_view(view, n, s_proj, R4_NEAR_Z, R4_FAR_Z, vp, &frame,
                           RAST_VCOLOR_EN, *out, (unsigned)*cap, &st);
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
    return r;
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

/* Continuous lofted skin: shared joint rings replace independent square bones.
 * The red material uses anatomical normals, fine triangular crystal facets and
 * a fixed window light. All output uses the existing interpolated RGB records. */
typedef struct {int x,y,z,rx,rz;} sh_ring;
typedef struct {int32_t p[3],n[3];uint32_t color;} sh_skin_vertex;
static void sh_normalize(int32_t v[3])
{
    int32_t len=sh_isqrt((int64_t)v[0]*v[0]+(int64_t)v[1]*v[1]+(int64_t)v[2]*v[2]);
    if(!len){v[0]=0;v[1]=SH_ONE;v[2]=0;return;}
    /* 一次倒数换掉三次变除数的 64 位除法:len<Q2^21 时 inv=(1<<42)/len 在
       int32 内,顶点坐标 <2^20,v[k]*inv<2^42 不溢 int64;右移 26 回 Q16.16,
       误差 <1/64 LSB。RV32 上变除数 64 位除法走 libgcc 软除法,是热路径。 */
    const int32_t inv=(int32_t)(INT64_C(1)<<42/(int64_t)len);
    for(unsigned k=0;k<3;++k)v[k]=(int32_t)(((int64_t)v[k]*inv)>>26);
}
static uint32_t sh_material(const int32_t p[3],const int32_t n[3],unsigned material)
{
    static const unsigned base[4][3]={{238,20,7},{195,9,4},{31,38,48},{235,239,243}};
    const int32_t light[3]={-34000,49000,23000};
    int32_t diffuse=0,half[3];
    for(unsigned k=0;k<3;++k)diffuse+=sh_mul(n[k],light[k]);
    if(diffuse<0)diffuse=0;
    const int32_t eye[3]={s_viewmodel?0:s_px,s_viewmodel?0:SH_EYE,s_viewmodel?0:s_pz};
    for(unsigned k=0;k<3;++k)half[k]=eye[k]-p[k];
    sh_normalize(half);
    for(unsigned k=0;k<3;++k)half[k]+=light[k];
    sh_normalize(half);
    int32_t spec=0;
    for(unsigned k=0;k<3;++k)spec+=sh_mul(n[k],half[k]);
    if(spec<0)spec=0;
    if(spec>SH_ONE)spec=SH_ONE;
    for(unsigned i=0;i<4;++i)spec=sh_mul(spec,spec); /* narrow polished facets */
    int32_t brightness=material==3?38000+sh_mul(diffuse,25000):19000+sh_mul(diffuse,44000);
    unsigned c[3];
    for(unsigned k=0;k<3;++k) {
        int32_t highlight=material<2?(k==0?190:k==1?90:28):125;
        int32_t value=(int32_t)((int64_t)base[material][k]*brightness/SH_ONE)+sh_mul(spec,highlight);
        c[k]=(unsigned)(value>255?255:value<0?0:value);
    }
    int64_t dx=p[0]-s_px,dy=p[1]-SH_EYE,dz=p[2]-s_pz;
    return sh_fog(dx*dx+dy*dy+dz*dz,sh_rgb(c[0],c[1],c[2]));
}
static void sh_skin_triangle(const sh_skin_vertex *a,const sh_skin_vertex *b,const sh_skin_vertex *c,
                             unsigned facet,const rast_viewport *vp,rast_record **out,uint32_t *cap)
{
    const sh_skin_vertex *t[3]={a,b,c};
    int64_t visible=0;
    const int32_t eye[3]={s_viewmodel?0:s_px,s_viewmodel?0:SH_EYE,s_viewmodel?0:s_pz};
    for(unsigned k=0;k<3;++k)visible+=(int64_t)(a->n[k]+b->n[k]+c->n[k])*
                                      (eye[k]-(a->p[k]+b->p[k]+c->p[k])/3);
    if(visible<=0)return;
    rast_view_vertex v[3];
    unsigned factor=94u+(facet*17u%13u);
    for(unsigned i=0;i<3;++i) {
        uint32_t col=t[i]->color;
        unsigned r=((col>>16)&255u)*factor/100u,g=((col>>8)&255u)*factor/100u,bv=(col&255u)*factor/100u;
        v[i]=(rast_view_vertex){t[i]->p[0],t[i]->p[1],t[i]->p[2],0,0,
            sh_rgb(r>255?255:r,g>255?255:g,bv>255?255:bv)};
    }
    (void)sh_poly(v,3,vp,out,cap);
}
static void sh_loft(const sh_enemy *e,int32_t sn,int32_t cs,const sh_ring *r,unsigned count,
                    unsigned sides,unsigned material,const rast_viewport *vp,rast_record **out,uint32_t *cap)
{
    sh_skin_vertex v[8][12];
    if(count<2 || count>8 || sides<4 || sides>12){s_failed=1;return;}
    for(unsigned i=0;i<count;++i) {
        const sh_ring *before=&r[i?i-1:i],*after=&r[i+1<count?i+1:i];
        int32_t tangent[3]={(after->x-before->x)*SH_ONE/100,(after->y-before->y)*SH_ONE/100,
                            (after->z-before->z)*SH_ONE/100};
        sh_normalize(tangent);
        int32_t u[3]={SH_ONE,0,0};
        if(tangent[0]>58000 || tangent[0]< -58000){u[0]=0;u[2]=SH_ONE;}
        int32_t dot=0;for(unsigned k=0;k<3;++k)dot+=sh_mul(u[k],tangent[k]);
        for(unsigned k=0;k<3;++k)u[k]-=sh_mul(tangent[k],dot);
        sh_normalize(u);
        int32_t w[3]={sh_mul(tangent[1],u[2])-sh_mul(tangent[2],u[1]),
                       sh_mul(tangent[2],u[0])-sh_mul(tangent[0],u[2]),
                       sh_mul(tangent[0],u[1])-sh_mul(tangent[1],u[0])};
        for(unsigned j=0;j<sides;++j) {
            uint16_t angle=(uint16_t)(j*65536u/sides);
            int32_t c=sh_cos(angle),n=sh_sin(angle),local[3],normal[3];
            const int32_t center[3]={r[i].x*SH_ONE/100,r[i].y*SH_ONE/100,r[i].z*SH_ONE/100};
            for(unsigned k=0;k<3;++k) {
                local[k]=center[k]+sh_mul(sh_mul(u[k],c),r[i].rx*SH_ONE/100)+
                                        sh_mul(sh_mul(w[k],n),r[i].rz*SH_ONE/100);
                normal[k]=sh_mul(u[k],c)+sh_mul(w[k],n);
            }
            v[i][j].p[0]=e->x+sh_mul(local[0],cs)+sh_mul(local[2],sn);
            v[i][j].p[1]=local[1];v[i][j].p[2]=e->z-sh_mul(local[0],sn)+sh_mul(local[2],cs);
            v[i][j].n[0]=sh_mul(normal[0],cs)+sh_mul(normal[2],sn);
            v[i][j].n[1]=normal[1];v[i][j].n[2]=-sh_mul(normal[0],sn)+sh_mul(normal[2],cs);
            v[i][j].color=sh_material(v[i][j].p,v[i][j].n,material);
        }
    }
    for(unsigned i=0;i+1<count;++i)for(unsigned j=0;j<sides;++j) {
        unsigned next=(j+1)%sides,facet=i*sides+j;
        sh_skin_triangle(&v[i][j],&v[i+1][j],&v[i+1][next],facet,vp,out,cap);
        sh_skin_triangle(&v[i][j],&v[i+1][next],&v[i][next],facet+3,vp,out,cap);
    }
    /* Rounded end rings close using their own vertices, no detached joint cubes. */
    for(unsigned end=0;end<2;++end) {
        unsigned i=end?count-1:0;
        unsigned near=end?i-1:1;
        int32_t normal[3]={(r[i].x-r[near].x)*SH_ONE/100,(r[i].y-r[near].y)*SH_ONE/100,
                           (r[i].z-r[near].z)*SH_ONE/100};
        sh_normalize(normal);
        sh_skin_vertex capv[12];
        for(unsigned j=0;j<sides;++j) {
            capv[j]=v[i][j];
            capv[j].n[0]=sh_mul(normal[0],cs)+sh_mul(normal[2],sn);
            capv[j].n[1]=normal[1];capv[j].n[2]=-sh_mul(normal[0],sn)+sh_mul(normal[2],cs);
            capv[j].color=sh_material(capv[j].p,capv[j].n,material);
        }
        for(unsigned j=1;j+1<sides;++j)sh_skin_triangle(&capv[0],&capv[j],&capv[j+1],j,vp,out,cap);
    }
}
static void sh_bevel(int32_t x,int32_t y,int32_t z,int hx,int hy,int hz,unsigned material,
                     const rast_viewport *vp,rast_record **out,uint32_t *cap)
{
    sh_enemy origin={0};origin.x=x;origin.z=z;
    const int yc=(int)((int64_t)y*100/SH_ONE),b=2;
    sh_ring rings[4]={{0,yc-hy,0,hx-b,hz-b},{0,yc-hy+b,0,hx,hz},
                      {0,yc+hy-b,0,hx,hz},{0,yc+hy,0,hx-b,hz-b}};
    sh_loft(&origin,0,SH_ONE,rings,4,8,material,vp,out,cap);
}
static void sh_enemy_draw(const sh_enemy *e,const rast_viewport *vp,rast_record **out,uint32_t *cap)
{
    int32_t dx=s_px-e->x,dz=s_pz-e->z,len=sh_isqrt((int64_t)dx*dx+(int64_t)dz*dz);
    int32_t sn=len?(int32_t)((int64_t)dx*SH_ONE/len):0,cs=len?(int32_t)((int64_t)dz*SH_ONE/len):SH_ONE;
    int swing=e->move?fx_sin((uint16_t)e->ph)*16/16384:0;
    int bob=e->move?(swing<0?-swing:swing)/8:0;
    /* Pelvis -> waist -> rib cage -> clavicles -> neck is one connected mesh. */
    sh_ring torso[]={{0,86+bob,0,14,9},{0,96+bob,0,15,10},{0,109+bob,1,13,9},
        {0,126+bob,1,19,11},{0,137+bob,0,22,10},{0,143+bob,0,10,7},{0,148+bob,0,5,5}};
    sh_ring head[]={{0,148+bob,1,5,5},{0,153+bob,3,8,7},{0,163+bob,2,9,8},
        {0,171+bob,0,7,6},{0,174+bob,0,2,2}};
    sh_loft(e,sn,cs,torso,7,10,0,vp,out,cap);sh_loft(e,sn,cs,head,5,12,0,vp,out,cap);
    for(int side=-1;side<=1;side+=2) {
        int step=swing*side;
        sh_ring leg[]={{side*9,91+bob,0,8,8},{side*10,73,step/2,8,7},
            {side*10,47,step,6,6},{side*10,29,step/2,6,5},{side*10,8,-step/2,4,4}};
        sh_ring foot[]={{side*10,5,-step/2+3,5,10},{side*10,9,-step/2+1,5,8},{side*10,14,-step/2,4,4}};
        sh_loft(e,sn,cs,leg,5,8,1,vp,out,cap);sh_loft(e,sn,cs,foot,3,8,1,vp,out,cap);
    }
    /* Both elbows bend toward a two-handed grip; wrists meet the hands. */
    sh_ring right[]={{20,136+bob,0,7,7},{24,126+bob,8,7,7},{25,119+bob,20,6,6},
        {16,133+bob,33,5,5},{11,142+bob,40,4,4}};
    sh_ring left[]={{-20,136+bob,0,7,7},{-24,125+bob,8,7,7},{-23,117+bob,18,6,6},
        {-11,132+bob,33,5,5},{5,141+bob,39,4,4}};
    sh_loft(e,sn,cs,right,5,8,0,vp,out,cap);sh_loft(e,sn,cs,left,5,8,0,vp,out,cap);
    sh_ring hand[]={{11,142+bob,39,4,4},{10,143+bob,45,5,4},{9,142+bob,48,3,3}};
    sh_loft(e,sn,cs,hand,3,8,0,vp,out,cap);
    sh_ring slide[]={{10,146+bob,41,4,4},{10,146+bob,63,4,4}};
    sh_loft(e,sn,cs,slide,2,8,2,vp,out,cap);
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
    s_proj = projection;
    s_failed = 0; s_n = 0;s_viewmodel=0;

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
    /* ---- 落影→敌人→弹→碎块→曳光 ---- */
    for (unsigned i = 0; i < SH_ENEMY_MAX; ++i) if (s_enemy[i].alive) {
        const int32_t x = s_enemy[i].x, z = s_enemy[i].z;
        sh_ground_shadow(x+SH_ONE/15,z+SH_ONE/15,SH_ONE*42/100,SH_ONE*30/100,vp,&o,&left);
        sh_enemy_draw(&s_enemy[i], vp, &o, &left);
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
    {
        s_weapon_start=s_n;
        fx16_mat4 world_view=s_view;
        memset(&s_view,0,sizeof s_view);
        s_view.v[0]=s_view.v[5]=s_view.v[10]=s_view.v[15]=SH_ONE;s_viewmodel=1;
        const int32_t kick=s_recoil/25,lower=s_reload?SH_ONE/8:0;
        sh_bevel(24*SH_ONE/100,-27*SH_ONE/100+kick-lower,-55*SH_ONE/100+kick,
                 4,4,15,2,vp,&o,&left);
        sh_enemy hand_origin={0};
        sh_ring grip[]={{24,-29, -45,3,4},{24,-37,-41,3,4},{24,-43,-39,3,3}};
        sh_ring hand[]={{24,-38,-41,5,5},{25,-44,-35,6,6},{27,-50,-27,7,7}};
        for(unsigned i=0;i<3;++i){grip[i].y+=(int)(kick*100/SH_ONE)-(int)(lower*100/SH_ONE);
                                 hand[i].y+=(int)(kick*100/SH_ONE)-(int)(lower*100/SH_ONE);}
        sh_loft(&hand_origin,0,SH_ONE,grip,3,8,2,vp,&o,&left);
        sh_loft(&hand_origin,0,SH_ONE,hand,3,8,2,vp,&o,&left);
        sh_box(24*SH_ONE/100,-22*SH_ONE/100+kick-lower,-66*SH_ONE/100,
            SH_ONE/90,SH_ONE/100,SH_ONE/90,SH_GUN,vp,&o,&left);
        if(s_flash) {
            const uint32_t flash[6]={0xffcc73u,0xffcc73u,0xffffedu,0xffdf9du,0xffffdau,0xffb74au};
            sh_box(24*SH_ONE/100,-26*SH_ONE/100,-75*SH_ONE/100,
                SH_ONE/20,SH_ONE/16,SH_ONE/20,flash,vp,&o,&left);
        }
        s_view=world_view;s_viewmodel=0;
    }
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
