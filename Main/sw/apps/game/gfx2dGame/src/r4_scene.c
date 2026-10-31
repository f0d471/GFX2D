/******************************************************************************
* @file r4_scene.c: 固定 30Hz 的世界逻辑与真实三维几何
* @brief 世界/自机/弹幕共享 R1 投影、setup 与 z；相机周期平移，自机独立横移。
*        墙面按世界深度分段，近缘延伸至视口外；整个物体发射或显式失败。
******************************************************************************/
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "r4.h"
#include "input.h"
#include "r4_scene.h"
#include "r4_geometry.h"
#include "r4_models.h"
#include "core/xorshift32.h"

#define CAM_STEP (4 * R4_ONE / R4_TICK_HZ)
#define MOVE_STEP (2 * R4_ONE / R4_TICK_HZ)
#define E_STEP (5 * R4_ONE / R4_TICK_HZ)
#define P_STEP (16 * R4_ONE / R4_TICK_HZ)
#define DRAW_FAR (36 * R4_ONE)
#ifndef R4_DISABLE_WALL_CACHE
#define R4_DISABLE_WALL_CACHE 0
#endif

uint32_t r4_gate_x_of(uint32_t seq, uint32_t rng0)
{
    static const int32_t pos[3] = {-R4_ONE, 0, R4_ONE};
    return (uint32_t)pos[(seq + rng0) % 3u];
}

void r4_world_init(r4_world *w, uint32_t seed)
{
    xs32 g;
    for (unsigned i=0;i<sizeof(*w);++i) ((uint8_t *)w)[i]=0;
    xs32_seed(&g,seed);w->rng=xs32_next(&g)%3u;
    w->shield=R4_SHIELD_INIT;w->bombs=2;w->ship_y=-3*R4_ONE/4;w->ladder=2;
    w->enemy=(r4_bullet){0,R4_ONE/5,-10*R4_ONE,1};w->enemy_hp=3;
    for (unsigned i=0;i<R4_GATE_SLOTS;++i) {
        w->gate[i].dist=R4_GATE_FIRST+i*R4_GATE_SPACING;
        w->gate[i].x=r4_gate_x_of(i,w->rng);
    }
    w->gate_seq=R4_GATE_SLOTS;
}

static void enemy_fire(r4_world *w,int32_t x,int32_t y,int32_t z,int spread)
{
    for(int k=-spread;k<=spread;++k) {
        for(unsigned i=0;i<R4_EBULLET_MAX;++i) if(!w->ebul[i].alive) {
            int32_t ticks=(-z-(int32_t)w->d-R4_SHIP_Z)/(CAM_STEP+E_STEP);
            if(ticks<1)ticks=1;
            w->ebul[i]=(r4_bullet){x,y,z,1};
            w->e_vx[i]=(w->cam_x-x+k*R4_ONE/2)/ticks;
            w->e_vy[i]=(w->ship_y-y+k*R4_ONE/5)/ticks;
            break;
        }
    }
}

static void hurt(r4_world *w)
{
    if(w->invulnerable || !w->shield)return;
    --w->shield;w->invulnerable=35;
    if(!w->shield)w->respawn=60;
}

/* --- 场景 2 专用逻辑 --------------------------------------------------------
   山脊滑翔：无门环（用户定案），山体=实体（撞山 hurt 掉盾重生）。
   定速 4m/s，d 仍是逐 tick 累加的确定量，回放契约不变。 */

#define RIDGE_NX 7u
#define RIDGE_NZ 10u                                 /* 大格子=大切面：低多边形读法的关键 */
#define RIDGE_HALF_X 20
#define RIDGE_Z0 1                            /* 近边贴到 1m：z=4m 时屏幕底部没有几何可画（基线的"平地"即此） */
#define RIDGE_Z1 90

/* zone 美术包：每 240m 行程换一套完整地貌。哑色调、大切面平涂，
   色板六槽 = {山体暗面, 山体亮面, 草甸, 草甸暖斑, 树木, 雾色}。 */
static const uint32_t ridge_palettes[4][6] = {
    {0xb0a184, 0xcab896, 0x7e9678, 0x8aa082, 0x41604e, 0xece4d4},   /* 晨光山脊 */
    {0xb09a6a, 0xd2bc8a, 0xa89a62, 0xb4a66e, 0x4e5c3e, 0xf2e4c2},   /* 金色草原 */
    {0xa06848, 0xc08860, 0xb08460, 0xbc9068, 0x54483a, 0xf0d0a8},   /* 红岩峡谷 */
    {0xbcc8d4, 0xe8eef4, 0xdde6ec, 0xe6edf2, 0x3a4c44, 0xe4ecf2},   /* 雪线 */
};
const char *const ridge_zone_names[4] = {
    "MORNING RIDGE", "GOLDEN STEPPE", "RED CANYON", "SNOWLINE"};
const char *const ridge_zone_tags[4] = {
    "GLIDE THE VALLEY / MIND THE RIDGE",
    "HOLD THE LINE / RIDE THE WIND",
    "LOW IS FAST / HIGH IS SAFE",
    "QUIET SKIES / CLEAN LINES"};
static uint32_t ridge_zone(const r4_world *w) { return (uint32_t)(w->d >> 16) / 240u & 3u; }
uint32_t r4_scene2_zone(const r4_world *w) { return ridge_zone(w); }

/* 山脊高度函数在文件后部定义（地形发射与碰撞共用一份，改一处两边都生效）。 */
static int32_t ridge_ground(int32_t x, uint32_t d, int32_t zahead);

/* 纯滑翔版没有门环（用户定案）；走廊 init 之后这里无事可做，留空守 sym。 */
void r4_scene2_fix_gates(r4_world *w)
{
    (void)w;
}

static void ridge_tick(r4_world *w, uint32_t keys)
{
    int32_t dir=((keys&PLATFORM_KEY_RIGHT)!=0)-((keys&PLATFORM_KEY_LEFT)!=0);
    int32_t vertical=((keys&PLATFORM_KEY_UP)!=0)-((keys&PLATFORM_KEY_DOWN)!=0);
    ++w->ticks;
    if(keys&(PLATFORM_KEY_UP|PLATFORM_KEY_DOWN|PLATFORM_KEY_LEFT|PLATFORM_KEY_RIGHT))w->manual=1;
    if(!w->manual) {
        int32_t x=fx_sin((uint16_t)(w->ticks*180u))*2;
        dir=x>w->cam_x?1:x<w->cam_x?-1:0;
        w->cam_x=x;
        /* 自动巡航飞上半区（-0.4±0.17m）：离谷地波峰的判定线留足余量。 */
        w->ship_y=-2*R4_ONE/5+fx_sin((uint16_t)(w->ticks*340u))/3;
    } else {
        w->cam_x+=dir*MOVE_STEP;
        w->ship_y+=vertical*MOVE_STEP;
    }
    if(w->cam_x>R4_ONE)w->cam_x=R4_ONE;
    if(w->cam_x< -R4_ONE)w->cam_x= -R4_ONE;
    if(w->ship_y>0)w->ship_y=0;
    if(w->ship_y< -R4_ONE)w->ship_y= -R4_ONE;
    w->strafe=dir;w->pitch=vertical;
    w->bank+=(dir*20000-w->bank)/4;
    w->d+=(uint32_t)((4u*(uint32_t)R4_ONE)/R4_TICK_HZ);   /* 定速 4m/s 滑翔 */
    if(w->invulnerable)--w->invulnerable;
    if(w->crash_ticks)--w->crash_ticks;
    if(w->respawn && !--w->respawn) {
        w->shield=R4_SHIELD_INIT;w->invulnerable=60;
        w->score=w->score>500?w->score-500:0;
    }
    /* 地形碰撞：机腹低于山面即 hurt 并托起（无敌窗盖过整段山峰，不会连扣）。 */
    {
        int32_t gh=ridge_ground(w->cam_x,w->d,(int32_t)(R4_SHIP_Z>>16));
        if(w->ship_y-R4_SHIP_HALF*3/2<gh) {
            w->ship_y=gh+R4_SHIP_HALF*3/2;
            hurt(w);w->crash_ticks=12;
        }
    }
    if(w->score>w->hiscore)w->hiscore=w->score;
}

void r4_world_tick(r4_world *w,uint32_t keys,uint32_t *fail)
{
    int32_t dir,vertical;
    if(w->scene==2u) { ridge_tick(w,keys); return; }
    dir=((keys&PLATFORM_KEY_RIGHT)!=0)-((keys&PLATFORM_KEY_LEFT)!=0);
    vertical=((keys&PLATFORM_KEY_UP)!=0)-((keys&PLATFORM_KEY_DOWN)!=0);
    (void)fail;
    ++w->ticks;
    if(keys&(PLATFORM_KEY_UP|PLATFORM_KEY_DOWN|PLATFORM_KEY_LEFT|PLATFORM_KEY_RIGHT))w->manual=1;
    if(!w->manual) {
        int32_t x=fx_sin((uint16_t)(w->ticks*180u))*2;
        dir=x>w->cam_x?1:x<w->cam_x?-1:0;
        w->cam_x=x;
        w->ship_y=-3*R4_ONE/4+fx_sin((uint16_t)(w->ticks*340u))/3;
    } else {
        w->cam_x+=dir*MOVE_STEP;
        w->ship_y+=vertical*MOVE_STEP;
    }
    if(w->cam_x>R4_ONE)w->cam_x=R4_ONE;
    if(w->cam_x< -R4_ONE)w->cam_x= -R4_ONE;
    if(w->ship_y>0)w->ship_y=0;
    if(w->ship_y< -R4_ONE)w->ship_y= -R4_ONE;
    w->strafe=dir;w->pitch=vertical;
    w->bank+=(dir*20000-w->bank)/4;
    /* Exactly 4m/s at 30Hz. The same world-grid phase repeats every 30 ticks. */
    w->d=(uint32_t)(((uint64_t)w->ticks*4u*R4_ONE)/R4_TICK_HZ);
    if(w->invulnerable)--w->invulnerable;
    if(w->enemy_hit)--w->enemy_hit;
    if(w->blast_ticks)--w->blast_ticks;
    if(w->respawn && !--w->respawn) {
        w->shield=R4_SHIELD_INIT;w->invulnerable=60;
        w->score=w->score>500?w->score-500:0;
    }
    if((keys&PLATFORM_KEY_ACTION) && !(w->last_keys&PLATFORM_KEY_ACTION) && w->bombs) {
        --w->bombs;w->invulnerable=60;
        for(unsigned i=0;i<R4_EBULLET_MAX;++i)w->ebul[i].alive=0;
        w->blast=(r4_bullet){w->cam_x,w->ship_y,-(int32_t)w->d-4*R4_ONE,1};
        w->blast_ticks=20;
    }
    w->last_keys=keys;
    if(w->pcooldown)--w->pcooldown;
    if(!w->pcooldown && w->shield) {
        for(unsigned i=0;i<R4_PBULLET_MAX;++i)if(!w->pbul[i].alive) {
            w->pbul[i]=(r4_bullet){w->cam_x,w->ship_y+R4_ONE/30,
                -(int32_t)w->d-R4_SHIP_Z-R4_ONE/5,1};break;
        }
        w->pcooldown=3;
    }
    for(unsigned i=0;i<R4_GATE_SLOTS;++i) {
        r4_gate *g=w->gate+i;
        if(!g->dist)continue;
        if(!g->fired && g->dist>w->d+5u*R4_ONE && g->dist<w->d+15u*R4_ONE) {
            g->fired=1;
            enemy_fire(w,(int32_t)g->x,R4_ONE/2,-(int32_t)g->dist,1);
        }
        /* The ship crosses the gate before the camera does. */
        if(!g->passed && w->d+R4_SHIP_Z>=g->dist) {
            int32_t dx=w->cam_x-(int32_t)g->x;
            g->passed=1;
            if(dx>R4_GATE_OPEN-R4_SHIP_HALF || dx< -R4_GATE_OPEN+R4_SHIP_HALF ||
               w->ship_y< -R4_HALF_H+R4_SHIP_HALF)hurt(w);
            else w->score+=50;
        }
        if(g->dist<w->d) {
            g->dist=R4_GATE_FIRST+w->gate_seq*R4_GATE_SPACING;
            g->x=r4_gate_x_of(w->gate_seq++,w->rng);g->passed=0;g->fired=0;
        }
    }
    if(w->ladder<2)w->enemy.alive=0;
    else if(w->enemy.alive) {
        w->enemy.x=fx_sin((uint16_t)(w->ticks*370u))*2;
        w->enemy.y=R4_ONE/5+fx_sin((uint16_t)(w->ticks*240u))/2;
        if(w->ticks%24u==0u)enemy_fire(w,w->enemy.x,w->enemy.y,w->enemy.z,0);
        if(w->enemy.z+(int32_t)w->d>= -R4_SHIP_Z)w->enemy.alive=0;
    } else if(w->ticks%45u==0u) {
        w->enemy=(r4_bullet){0,R4_ONE/5,-(int32_t)w->d-18*R4_ONE,1};w->enemy_hp=3;
    }
    for(unsigned i=0;i<R4_EBULLET_MAX;++i) {
        r4_bullet *b=w->ebul+i;
        if(!b->alive)continue;
        b->x+=w->e_vx[i];b->y+=w->e_vy[i];b->z+=E_STEP;
        if(b->z+(int32_t)w->d>= -R4_SHIP_Z) {
            int32_t dx=b->x-w->cam_x,dy=b->y-w->ship_y;
            b->alive=0;
            if(dx<R4_GRAZE_DIST && dx> -R4_GRAZE_DIST && dy<R4_GRAZE_DIST && dy> -R4_GRAZE_DIST) {
                if(dx<R4_HIT_DIST && dx> -R4_HIT_DIST && dy<R4_HIT_DIST && dy> -R4_HIT_DIST)hurt(w);
                else {++w->graze;w->score+=10;}
            }
        }
    }
    for(unsigned i=0;i<R4_PBULLET_MAX;++i) {
        r4_bullet *b=w->pbul+i;
        int32_t old=b->z;
        if(!b->alive)continue;
        b->z-=P_STEP;
        if(b->z+(int32_t)w->d< -DRAW_FAR)b->alive=0;
        if(w->enemy.alive && old>=w->enemy.z && b->z<=w->enemy.z) {
            int32_t dx=b->x-w->enemy.x,dy=b->y-w->enemy.y;
            if(dx<R4_ONE/2 && dx> -R4_ONE/2 && dy<R4_ONE/2 && dy> -R4_ONE/2) {
                b->alive=0;w->enemy_hit=5;
                if(w->enemy_hp && !--w->enemy_hp) {
                    w->enemy.alive=0;w->score+=100;
                    w->blast=w->enemy;w->blast_ticks=15;
                }
            }
        }
    }
    if(w->score>w->hiscore)w->hiscore=w->score;
}

static int emit_failed;
typedef struct {
    rast_record records[144];
    rast_setup_stats stats;
    int32_t phase;
    uint32_t count;
    uint8_t valid;
} wall_cache_entry;
#define WALL_PHASES 120u
static wall_cache_entry wall_cache[WALL_PHASES];
static fx16_mat4 wall_projection;
static rast_viewport wall_viewport;
static uint32_t wall_ladder;
static void vertex(rast_view_vertex *v,int32_t x,int32_t y,int32_t z,
                   uint16_t u,uint16_t t,uint32_t color)
{
    *v=(rast_view_vertex){x,y,z,u,t,color};
}
static uint32_t polygon(const rast_view_vertex *v,unsigned n,const rast_viewport *vp,
                        uint32_t flags,const fx16_mat4 *p,rast_record *out,
                        uint32_t cap,rast_setup_stats *st)
{
    rast_vertex screen[8];
    const rast_frame frame = {
        (unsigned)(vp->x+vp->w)>RAST_WIDTH?(unsigned)(vp->x+vp->w):RAST_WIDTH,
        RAST_HEIGHT};
    int r;
    if(!R4_DISABLE_VERTEX_CACHE && n<=8 &&
       !r4_project_inside(v,n,p,R4_NEAR_Z,R4_FAR_Z,vp,screen))
        r=gfx_setup_polygon(screen,n,vp,&frame,flags,out,cap,st);
    else r=gfx_setup_view(v,n,p,R4_NEAR_Z,R4_FAR_Z,vp,&frame,flags,out,cap,st);
    if(r<0)emit_failed=1;
    return r>0?(uint32_t)r:0;
}
static uint32_t facing(int32_t x,int32_t y,int32_t z,int32_t hw,int32_t hh,
                       uint32_t c,unsigned u,unsigned t,uint32_t flags,
                       const rast_viewport *vp,const fx16_mat4 *p,
                       rast_record *out,uint32_t cap,rast_setup_stats *st)
{
    rast_view_vertex v[4];
    vertex(v,x-hw,y+hh,z,u,t,c);vertex(v+1,x+hw,y+hh,z,u+496,t,c);
    vertex(v+2,x+hw,y-hh,z,u+496,t+496,c);vertex(v+3,x-hw,y-hh,z,u,t+496,c);
    return polygon(v,4,vp,flags,p,out,cap,st);
}

static uint32_t wall_color(int32_t z,unsigned face)
{
    /* 亮蓝+亮紫双色墙面:侧墙(1,2,5,6)偏紫,顶底(0,3,4,7)偏蓝;
       乘数平台 200,整幅提亮;远端淡入雾幕(雾幕封底见 r4_world_emit)。 */
    uint32_t distance=(uint32_t)(-z);
    unsigned shade=200u+(uint32_t)(55u*9u*R4_ONE)/(9u*R4_ONE+distance);
    const unsigned light[8]={210,255,196,182,188,240,200,196};
    shade=shade*light[face]/255u;
    unsigned r,g,b;
    if(face==1||face==2||face==5||face==6){ r=shade*172u/255u; g=shade*142u/255u; b=shade; }
    else { r=shade*148u/255u; g=shade*198u/255u; b=shade; }
    return 0xff000000u|(r<<16)|(g<<8)|b;
}

static uint32_t wall(unsigned face,int32_t near_z,int32_t far_z,uint32_t color,
                     const rast_viewport *vp,const fx16_mat4 *p,unsigned flags,
                     rast_record *out,uint32_t cap,rast_setup_stats *st)
{
    rast_view_vertex v[4];
    const int32_t cut=R4_ONE/2;
    const int32_t corners[8][2]={{-R4_HALF_W+cut,R4_HALF_H},{-R4_HALF_W,R4_HALF_H-cut},
        {-R4_HALF_W,-R4_HALF_H+cut},{-R4_HALF_W+cut,-R4_HALF_H},
        {R4_HALF_W-cut,-R4_HALF_H},{R4_HALF_W,-R4_HALF_H+cut},
        {R4_HALF_W,R4_HALF_H-cut},{R4_HALF_W-cut,R4_HALF_H}};
    unsigned a=face,b=(face+1)%8;
    (void)color;
    vertex(v,corners[a][0],corners[a][1],near_z,0,0,wall_color(near_z,face));
    vertex(v+1,corners[a][0],corners[a][1],far_z,0,496,wall_color(far_z,face));
    vertex(v+2,corners[b][0],corners[b][1],far_z,496,496,wall_color(far_z,face));
    vertex(v+3,corners[b][0],corners[b][1],near_z,496,0,wall_color(near_z,face));
    return polygon(v,4,vp,flags,p,out,cap,st);
}

/* --- 场景 2：RIDGE FLIGHT（低多边形地形 + 甜甜圈门 + 纸飞机） ----------------
   只用平涂：面颜色由 CPU 按"法线·太阳 + 高度 + 距离雾"算出，和场景 0 同一条
   rast 通路。这里不发射墙、弹幕与敌机；门框仍是逻辑里那三个 gate。 */

static uint32_t ridge_mix(uint32_t a, uint32_t b, uint32_t t)   /* t: 0..256 */
{
    uint32_t r, g, bl;
    if (t > 256u) t = 256u;
    r = ((((a >> 16) & 255u) * (256u - t)) + (((b >> 16) & 255u) * t)) >> 8;
    g = ((((a >> 8) & 255u) * (256u - t)) + (((b >> 8) & 255u) * t)) >> 8;
    bl = (((a & 255u) * (256u - t)) + ((b & 255u) * t)) >> 8;
    return (r << 16) | (g << 8) | bl;
}

static uint32_t ridge_fog(uint32_t color, int32_t depth, uint32_t fogc)
{
    int32_t far = (int32_t)(RIDGE_Z1 * R4_ONE);
    int32_t t = depth * 256 / far;
    if (t < 0) t = 0;
    if (t > 190) t = 190;                  /* 空气透视加强：远处留 26% 本色 */
    return ridge_mix(color, fogc, (uint32_t)t);
}

/* 近景压暗：0m 处 ×0.85，24m 处恢复 ×1.0——和远处的雾一暗一亮拉开纵深。 */
static uint32_t ridge_near(int32_t depth)
{
    if (depth < 0) depth = 0;
    if (depth > 24 * R4_ONE) depth = 24 * R4_ONE;
    return (uint32_t)(218u + (uint32_t)((int64_t)depth * 38 / (24 * R4_ONE)));
}

/* 面法线 → 明暗系数（0.35..1.15），太阳从左上后方来。 */
static uint32_t ridge_shade_i(int64_t nx, int64_t ny, int64_t nz)
{
    const int64_t Lx = -30000, Ly = 42000, Lz = 50000;
    int64_t d = nx * Lx + ny * Ly + nz * Lz;
    int64_t l1 = (nx < 0 ? -nx : nx) + (ny < 0 ? -ny : ny) + (nz < 0 ? -nz : nz);
    int64_t s;
    if (l1 <= 0) return 160u;
    s = 150 + d * 110 / l1 / 65536;
    if (s < 120) s = 120;
    if (s > 285) s = 285;
    return (uint32_t)s;
}

static uint32_t ridge_apply(uint32_t base, uint32_t shade)
{
    uint32_t r = (((base >> 16) & 255u) * shade) >> 8;
    uint32_t g = (((base >> 8) & 255u) * shade) >> 8;
    uint32_t b = ((base & 255u) * shade) >> 8;
    if (r > 255u) r = 255u;
    if (g > 255u) g = 255u;
    if (b > 255u) b = 255u;
    return (r << 16) | (g << 8) | b;
}

static int32_t ridge_ground(int32_t x, uint32_t d, int32_t zahead)
{
    /* 谷地剖面：中央草甸平缓（±0.5m 级），|x|>8m 起谷壁，14m 外山体高耸（+9m 级）。
       fx_sin 是 Q1.14，fx16_mul 后实际振幅=常数/4，常数按 4 倍目标幅度写。 */
    uint32_t zz = (uint32_t)((int32_t)(d >> 16) + zahead);
    int32_t xm = x / R4_ONE;
    int32_t ax = x < 0 ? -x : x;
    int32_t h = -3 * R4_ONE / 2;                  /* 谷底 -1.5m */
    h += fx16_mul(fx_sin((uint16_t)(zz * 2184u)), 2 * R4_ONE);              /* ±0.5m 纵向缓波 */
    h += fx16_mul(fx_sin((uint16_t)(zz * 3855u + (uint32_t)(xm * 1307u))),
                  6 * R4_ONE / 5);                                          /* ±0.3m 斜向碎波 */
    if (ax > 8 * R4_ONE) {
        int32_t t = (ax - 8 * R4_ONE) / R4_ONE;   /* 米 */
        int32_t rise = t * t * R4_ONE / 16;       /* 抛物线谷壁：t=12m → +9m */
        rise += fx16_mul(fx_sin((uint16_t)(zz * 1456u + (uint32_t)(xm * 2621u))),
                         6 * R4_ONE);             /* 山体纵向起伏 ±1.5m */
        h += rise;
    }
    return h;
}

/* 平涂面的绕向自适应：先按给定向序发射，被背面剔除就换向重试一次。
   场景 2 的几何是程序生成的，绕向按数学还是按屏幕算容易出错，这里一次兜住。 */
static uint32_t ridge_poly(const rast_view_vertex *v, unsigned n,
                           const rast_viewport *vp, const fx16_mat4 *p,
                           rast_record *out, uint32_t cap, rast_setup_stats *st)
{
    rast_view_vertex copy[4];
    uint32_t before = st->backface, got;
    unsigned i;
    if (n > 4u) return 0u;
    for (i = 0u; i < n; ++i) copy[i] = v[i];
    got = polygon(copy, n, vp, RAST_VCOLOR_EN, p, out, cap, st);
    if (got == 0u && st->backface != before) {
        rast_view_vertex t = copy[1];
        copy[1] = copy[n - 1u];
        copy[n - 1u] = t;
        got = polygon(copy, n, vp, RAST_VCOLOR_EN, p, out, cap, st);
    }
    return got;
}
static uint32_t ridge_terrain_emit(const r4_world *w, const fx16_mat4 *projection,
                                   const rast_viewport *vp, rast_record *out,
                                   uint32_t cap, rast_setup_stats *st)
{
    uint32_t n = 0u;
    const uint32_t *pal = ridge_palettes[ridge_zone(w)];
    int32_t dx = (RIDGE_HALF_X * 2 * R4_ONE) / (int32_t)RIDGE_NX;
    int32_t dz = ((RIDGE_Z1 - RIDGE_Z0) * R4_ONE) / (int32_t)RIDGE_NZ;
    int32_t hl[RIDGE_NZ + 1u];
    for (uint32_t j = 0; j <= RIDGE_NZ; ++j)
        hl[j] = ridge_ground(-RIDGE_HALF_X * R4_ONE, w->d,
                             RIDGE_Z0 + (int32_t)j * (dz / R4_ONE));
    for (uint32_t i = 0; i < RIDGE_NX; ++i) {
        int32_t x0 = -RIDGE_HALF_X * R4_ONE + (int32_t)i * dx;
        int32_t x1 = x0 + dx;
        int32_t hr[RIDGE_NZ + 1u];
        for (uint32_t j = 0; j <= RIDGE_NZ; ++j)
            hr[j] = ridge_ground(x1, w->d, RIDGE_Z0 + (int32_t)j * (dz / R4_ONE));
        for (uint32_t j = 0; j < RIDGE_NZ; ++j) {
            int32_t z0 = -(RIDGE_Z0 * R4_ONE + (int32_t)j * dz);
            int32_t z1 = z0 - dz;
            int32_t h0 = hl[j], h1 = hr[j], h2 = hr[j + 1u], h3 = hl[j + 1u];
            int32_t mid = (h0 + h1 + h2 + h3) / 4;
            int32_t mid_ax = (x0 < 0 ? -x0 : x0) < (x1 < 0 ? -x1 : x1) ?
                             (x0 < 0 ? -x0 : x0) : (x1 < 0 ? -x1 : x1);
            int64_t nx = ((int64_t)(h1 - h0) * dz);
            int64_t nz = ((int64_t)dx * (h3 - h0));
            int64_t ny = (int64_t)dx * dz;
            uint32_t shade = ridge_shade_i(-nx, ny, -nz);
            /* 大切面平涂：一面一色（四角同色），法线光照给亮暗台阶，
               草甸两色斑驳（确定性 hash 挑色），叠 ±3% 抖动——晶体感在这几行。 */
            uint32_t pick = i * 7u + j * 13u + ridge_zone(w) * 11u;
            uint32_t base = mid_ax > 8 * R4_ONE
                ? (mid > 3 * R4_ONE ? pal[1] : pal[0])
                : ((pick & 7u) < 5u ? pal[2] : pal[3]);
            uint32_t jitter = 246u + (pick & 7u);
            uint32_t col = ridge_fog(ridge_apply(base,
                (shade * jitter * ridge_near(-z0)) >> 16), -z0, pal[5]);
            rast_view_vertex v[4];
            vertex(v, x0, h0, z0, 0, 0, col);
            vertex(v + 1, x1, h1, z0, 0, 0, col);
            vertex(v + 2, x1, h2, z1, 0, 0, col);
            vertex(v + 3, x0, h3, z1, 0, 0, col);
            { rast_view_vertex tmp = v[1]; v[1] = v[3]; v[3] = tmp; }
            n += ridge_poly(v, 4, vp, projection, out + n, cap - n, st);
        }
        for (uint32_t j = 0; j <= RIDGE_NZ; ++j) hl[j] = hr[j];
    }
    return n;
}

/* 云：水平云板（贴着 x-z 平面的扁平板），从下往上看读作云层而不是纸片；
   双板错位叠出厚度。世界锚定 96m 窗循环，回卷点在雾里不可见。 */
static uint32_t ridge_clouds_emit(const r4_world *w, const fx16_mat4 *projection,
                                  const rast_viewport *vp, rast_record *out,
                                  uint32_t cap, rast_setup_stats *st)
{
    static const int32_t offs[4] = {0, 23, 51, 74};
    static const int32_t cxs[4] = {-9, 11, -3, 7};
    static const int32_t cys[4] = {6, 8, 5, 7};
    uint32_t n = 0u;
    const uint32_t fog = ridge_palettes[ridge_zone(w)][5];
    const int32_t m = (int32_t)(w->d >> 16);
    for (uint32_t i = 0u; i < 4u; ++i) {
        if (cap - n < 10u) break;                  /* 容量边缘断点 */
        int32_t ahead = ((m + offs[i]) % 96) + 6;
        int32_t z = -ahead * R4_ONE;
        uint32_t col = ridge_fog(0xf8fbff, (int32_t)ahead * R4_ONE, fog);
        uint32_t col2 = ridge_fog(0xeef4fa, (int32_t)ahead * R4_ONE, fog);
        rast_view_vertex v[4];
        int32_t cx = cxs[i] * R4_ONE, cy = cys[i] * R4_ONE;
        /* 上板（亮）：长条平板 */
        vertex(v, cx - 16 * R4_ONE / 5, cy, z - 9 * R4_ONE / 5, 0, 0, col);
        vertex(v + 1, cx + 16 * R4_ONE / 5, cy, z - 9 * R4_ONE / 5, 0, 0, col);
        vertex(v + 2, cx + 9 * R4_ONE / 5, cy, z + 9 * R4_ONE / 5, 0, 0, col);
        vertex(v + 3, cx - 9 * R4_ONE / 5, cy, z + 9 * R4_ONE / 5, 0, 0, col);
        { rast_view_vertex tmp = v[1]; v[1] = v[3]; v[3] = tmp; }
        n += ridge_poly(v, 4, vp, projection, out + n, cap - n, st);
        /* 下板（暗一点，错位）：厚度感 */
        vertex(v, cx - 9 * R4_ONE / 5, cy - R4_ONE / 4, z - 11 * R4_ONE / 5, 0, 0, col2);
        vertex(v + 1, cx + 11 * R4_ONE / 5, cy - R4_ONE / 4, z - 11 * R4_ONE / 5, 0, 0, col2);
        vertex(v + 2, cx + 6 * R4_ONE / 5, cy - R4_ONE / 4, z + 12 * R4_ONE / 5, 0, 0, col2);
        vertex(v + 3, cx - 5 * R4_ONE / 5, cy - R4_ONE / 4, z + 12 * R4_ONE / 5, 0, 0, col2);
        { rast_view_vertex tmp = v[1]; v[1] = v[3]; v[3] = tmp; }
        n += ridge_poly(v, 4, vp, projection, out + n, cap - n, st);
    }
    return n;
}

/* 地面影子：贴着山面的一块暗斑（0.56m×0.72m），四角各自取 ridge_ground+0.08m，
   随坡面起伏；颜色取谷底色压暗一半，随地貌走。 */
static uint32_t ridge_shadow_emit(const r4_world *w, const fx16_mat4 *projection,
                                  const rast_viewport *vp, rast_record *out,
                                  uint32_t cap, rast_setup_stats *st)
{
    uint32_t n = 0u;
    const int32_t zahead = (int32_t)(R4_SHIP_Z >> 16);
    const int32_t cx = w->cam_x;
    const int32_t hw = 28 * R4_ONE / 100, hl = 36 * R4_ONE / 100;
    const uint32_t *pal = ridge_palettes[ridge_zone(w)];
    const uint32_t fog = pal[5];
    int32_t g0 = ridge_ground(cx - hw, w->d, zahead);
    int32_t g1 = ridge_ground(cx + hw, w->d, zahead);
    int32_t g2 = ridge_ground(cx + hw, w->d, zahead + 1);
    int32_t g3 = ridge_ground(cx - hw, w->d, zahead + 1);
    uint32_t col = ridge_fog(ridge_apply(pal[3], 130u), R4_SHIP_Z, fog);
    rast_view_vertex v[4];
    vertex(v, cx - hw, g0 + R4_ONE / 12, -(int32_t)R4_SHIP_Z, 0, 0, col);
    vertex(v + 1, cx + hw, g1 + R4_ONE / 12, -(int32_t)R4_SHIP_Z, 0, 0, col);
    vertex(v + 2, cx + hw, g2 + R4_ONE / 12, -(int32_t)R4_SHIP_Z - hl, 0, 0, col);
    vertex(v + 3, cx - hw, g3 + R4_ONE / 12, -(int32_t)R4_SHIP_Z - hl, 0, 0, col);
    { rast_view_vertex tmp = v[1]; v[1] = v[3]; v[3] = tmp; }
    n += ridge_poly(v, 4, vp, projection, out + n, cap - n, st);
    return n;
}

/* 纸飞机：0.42m 半机长，横滚/俯仰与走廊自机同一套约定；
   米白纸色+暖折痕+红尾翼，受击无敌窗闪白。 */
static uint32_t ridge_plane_emit(const r4_world *w, const fx16_mat4 *projection,
                                 const rast_viewport *vp, rast_record *out,
                                 uint32_t cap, rast_setup_stats *st)
{
    uint32_t n = 0u;
    const int32_t S = 42 * R4_ONE / 100;
    int32_t cx = w->cam_x, cy = w->ship_y, cz = -(int32_t)R4_SHIP_Z;
    const int32_t A[3] = {0, 10 * S / 100, 70 * S / 100};
    const int32_t B[3] = {0, 16 * S / 100, -48 * S / 100};
    const int32_t C[3] = {-40 * S / 100, 2 * S / 100, -40 * S / 100};
    const int32_t D[3] = {40 * S / 100, 2 * S / 100, -40 * S / 100};
    const int32_t E[3] = {-6 * S / 100, 3 * S / 100, -50 * S / 100};
    const int32_t F[3] = {6 * S / 100, 3 * S / 100, -50 * S / 100};
    const int32_t G[3] = {0, -14 * S / 100, -6 * S / 100};
    const int32_t K[3] = {0, 34 * S / 100, -44 * S / 100};    /* 垂直尾翼顶点（红） */
    const int32_t *faces[10][3] = {
        {A, C, B}, {A, B, D}, {A, C, G}, {A, G, D},
        {C, E, B}, {B, F, D}, {A, E, F}, {E, G, F},
        {E, F, K}, {E, K, G}};
    static const uint32_t cols[10] = {0xfaf6ec, 0xefe8d6, 0xdcd2bc, 0xdcd2bc,
                                      0xe8ddc8, 0xe8ddc8, 0xe6dbc6, 0xbfb49e,
                                      0xd05038, 0xb04028};
    int32_t roll = w->bank;
    int32_t cosine = FX16_ONE - fx16_mul(roll, roll) / 2;
    uint32_t blink = (w->invulnerable && (w->ticks & 4u)) ? 0xffe8f8u : 0u;
    for (uint32_t f = 0u; f < 10u; ++f) {
        rast_view_vertex v[3];
        for (uint32_t k = 0u; k < 3u; ++k) {
            const int32_t *p = faces[f][k];
            int32_t lx = fx16_mul(p[0], cosine) - fx16_mul(p[1], roll);
            int32_t ly = fx16_mul(p[0], roll) + fx16_mul(p[1], cosine);
            int32_t lz = p[2] + w->pitch * p[1] / 5 + w->strafe * p[0] / 6;
            vertex(v + k, cx + lx, cy + ly, cz + lz, 0, 0, blink ? blink : cols[f]);
        }
        { rast_view_vertex tmp = v[1]; v[1] = v[2]; v[2] = tmp; }
        n += ridge_poly(v, 3, vp, projection, out + n, cap - n, st);
    }
    return n;
}

/* 低模松树/岩石：谷缘世界锚定点位，随前进掠过。树=两层四棱锥，岩=单锥。
   朝日面亮、背日面暗，远树单层省记录。 */
static uint32_t ridge_props_emit(const r4_world *w, const fx16_mat4 *projection,
                                 const rast_viewport *vp, rast_record *out,
                                 uint32_t cap, rast_setup_stats *st)
{
    static const int32_t offs[8] = {5, 17, 31, 44, 58, 66, 79, 84};
    static const int32_t txs[8] = {-6, 5, -4, 7, 3, -7, 5, -5};
    static const int32_t typ[8] = {2, 2, 1, 2, 0, 0, 1, 2};   /* 0=岩 1=单层树 2=双层树 */
    uint32_t n = 0u;
    const uint32_t *pal = ridge_palettes[ridge_zone(w)];
    const int32_t m = (int32_t)(w->d >> 16);
    for (uint32_t i = 0u; i < 8u; ++i) {
        int32_t ahead = ((m + offs[i]) % 88) + 6;
        int32_t z = -ahead * R4_ONE;
        int32_t x = txs[i] * R4_ONE;
        int32_t g = ridge_ground(x, w->d, ahead);
        if (cap - n < 20u) break;                 /* 容量边缘断点：绝不把帧顶爆 */
        uint32_t fog = pal[5];
        uint32_t lit, dark;
        int32_t r, top, base2;
        if (ridge_zone(w) == 2u && typ[i] != 0u && (i & 1u)) continue;   /* 峡谷树减半 */
        if (typ[i] == 0u) {                       /* 岩石 */
            r = 32 * R4_ONE / 100; top = g + 3 * R4_ONE / 10;
            lit = ridge_fog(ridge_apply(pal[0], 210u), ahead * R4_ONE, fog);
            dark = ridge_fog(ridge_apply(pal[0], 150u), ahead * R4_ONE, fog);
        } else {                                  /* 树 */
            r = 55 * R4_ONE / 100; top = g + (typ[i] == 2u ? 22 : 15) * R4_ONE / 10;
            lit = ridge_fog(ridge_apply(pal[4], 235u), ahead * R4_ONE, fog);
            dark = ridge_fog(ridge_apply(pal[4], 140u), ahead * R4_ONE, fog);
        }
        base2 = typ[i] == 2u ? g + 8 * R4_ONE / 10 : 0;
        {
            const int32_t px[4] = {0, r, 0, -r};
            const int32_t pz[4] = {-r, 0, r, 0};
            for (uint32_t k = 0u; k < 4u; ++k) {   /* 下层锥/岩：四个侧面 */
                uint32_t col = (k & 1u) ? dark : lit;
                rast_view_vertex v[3];
                vertex(v, x + px[k], g, z + pz[k], 0, 0, col);
                vertex(v + 1, x + px[(k + 1u) & 3u], g, z + pz[(k + 1u) & 3u], 0, 0, col);
                vertex(v + 2, x, typ[i] == 0u ? top : (typ[i] == 1u ? top : base2), z, 0, 0, col);
                { rast_view_vertex tmp = v[1]; v[1] = v[2]; v[2] = tmp; }
                n += ridge_poly(v, 3, vp, projection, out + n, cap - n, st);
            }
            if (typ[i] == 2u) {                    /* 上层小锥 */
                int32_t r2 = 36 * R4_ONE / 100;
                const int32_t qx[4] = {0, r2, 0, -r2};
                const int32_t qz[4] = {-r2, 0, r2, 0};
                for (uint32_t k = 0u; k < 4u; ++k) {
                    uint32_t col = (k & 1u) ? dark : lit;
                    rast_view_vertex v[3];
                    vertex(v, x + qx[k], base2, z + qz[k], 0, 0, col);
                    vertex(v + 1, x + qx[(k + 1u) & 3u], base2, z + qz[(k + 1u) & 3u], 0, 0, col);
                    vertex(v + 2, x, top, z, 0, 0, col);
                    { rast_view_vertex tmp = v[1]; v[1] = v[2]; v[2] = tmp; }
                    n += ridge_poly(v, 3, vp, projection, out + n, cap - n, st);
                }
            }
        }
    }
    return n;
}

static uint32_t ridge_emit(const r4_world *w, const fx16_mat4 *projection,
                           const rast_viewport *vp, rast_record *out,
                           uint32_t cap, rast_setup_stats *st)
{
    uint32_t n = 0u;
    if (cap < 300u) return UINT32_MAX;
    n += ridge_terrain_emit(w, projection, vp, out + n, cap - n, st);
    n += ridge_shadow_emit(w, projection, vp, out + n, cap - n, st);
    n += ridge_plane_emit(w, projection, vp, out + n, cap - n, st);
    /* 点缀物是加分项：核心几何之外容量富余才画，预算永不顶死。 */
    if (cap - n >= 24u)
        n += ridge_props_emit(w, projection, vp, out + n, cap - n, st);
    if (cap - n >= 16u)
        n += ridge_clouds_emit(w, projection, vp, out + n, cap - n, st);
    return n;
}

uint32_t r4_world_emit(const r4_world *w,const fx16_mat4 *projection,
                       const rast_viewport *vp,uint32_t ladder,
                       rast_record *out,uint32_t cap,rast_setup_stats *counts)
{
    rast_setup_stats st={0};
    uint32_t n=0,flat=RAST_VCOLOR_EN;
    uint32_t walls=flat|(ladder>=2?RAST_TEX_EN:0);
    uint32_t keyed=ladder>=2?(RAST_TEX_EN|RAST_KEY_EN|RAST_VCOLOR_EN):flat;
    if (w->scene == 2u)
        return ridge_emit(w, projection, vp, out, cap, &st);
    fx16_mat4 camera_projection=*projection;
    uint16_t camera_angle=(uint16_t)((w->ticks%WALL_PHASES)*65536u/WALL_PHASES);
    /* Trigonometric outputs are Q1.14; convert to Q16.16 before metre offsets. */
    int32_t camera_x=fx_sin(camera_angle)*4/24;
    int32_t camera_y=fx_sin((uint16_t)(camera_angle*2u))*4/64;
    int32_t phase=(int32_t)(w->d%R4_SEG_LEN);
    const int32_t planes[6]={R4_NEAR_PAD,2*R4_ONE,5*R4_ONE-phase/3,
                            10*R4_ONE-phase/3,18*R4_ONE-phase,34*R4_ONE-phase};
    emit_failed=0;
    if(!w || !projection || !vp || !out || cap<256)return UINT32_MAX;
    const rast_frame frame={(unsigned)(vp->x+vp->w)>RAST_WIDTH?(unsigned)(vp->x+vp->w):RAST_WIDTH,
                            RAST_HEIGHT};
    if(wall_ladder!=ladder || memcmp(&wall_projection,projection,sizeof *projection) ||
       memcmp(&wall_viewport,vp,sizeof *vp)) {
        for(unsigned i=0;i<WALL_PHASES;++i)wall_cache[i].valid=0;
        wall_ladder=ladder;wall_projection=*projection;wall_viewport=*vp;
    }
    /* A four-second physical camera drift gives genuine depth-dependent
       parallax. Its 120 poses and 30 forward phases have a common 120 period;
       all world objects use this same view, without rebuilding walls per tick. */
    camera_projection.v[3]=fx16_sat((int64_t)projection->v[3]-fx16_mul(projection->v[0],camera_x));
    camera_projection.v[7]=fx16_sat((int64_t)projection->v[7]-fx16_mul(projection->v[5],camera_y));
    projection=&camera_projection;
    wall_cache_entry *entry=wall_cache+w->ticks%WALL_PHASES;
    if(!R4_DISABLE_WALL_CACHE && entry->valid && entry->phase==phase) {
        n=entry->count;memcpy(out,entry->records,n*sizeof *out);st=entry->stats;
    } else {
        for(unsigned band=0;band<5;++band)for(unsigned face=0;face<8;++face) {
            n+=wall(face,-planes[band],-planes[band+1],0,vp,projection,walls,out+n,cap-n,&st);
        }
        if(!emit_failed && n<=144) {
            memcpy(entry->records,out,n*sizeof *out);entry->count=n;entry->phase=phase;
            entry->stats=st;entry->valid=1;
        }
    }
    /* A nearest gate receives real inner walls; distant gates retain the cheap
       front rim. All parts share the world depth test. */
    /* 雾幕封底:走廊尽头的发光霾墙(亮蓝紫),盖住 33m 外的一切——
       远端分段回绕的接缝由此不可见,"到头重播"感消失。 */
    n+=facing(0,0,-33*R4_ONE,R4_HALF_W,R4_HALF_H,0xff5a64ff,0,0,flat,vp,projection,out+n,cap-n,&st);
    unsigned nearest=R4_GATE_SLOTS;
    int32_t nearest_z=-DRAW_FAR;
    for(unsigned i=0;i<R4_GATE_SLOTS;++i) {
        int32_t z=(int32_t)w->d-(int32_t)w->gate[i].dist;
        if(w->gate[i].dist && z<=-R4_SHIP_Z-R4_ONE/3 && z>nearest_z) {
            nearest=i;nearest_z=z;
        }
    }
    /* All visible gates participate in z, including gates behind enemies/bullets. */
    for(unsigned i=0;i<R4_GATE_SLOTS;++i) {
        const r4_gate *g=w->gate+i;
        int32_t z=(int32_t)w->d-(int32_t)g->dist;
        int32_t o=R4_GATE_OPEN,t=R4_GATE_THICK,x=(int32_t)g->x;
        int32_t h=R4_HALF_H*4/5;
        /* 青色=引导门,品红=危险门(每三道);与 2D 身份色一致。 */
        uint32_t danger=(g->dist/R4_GATE_SPACING)%3u==0;
        uint32_t c=danger?0xffff4fd8u:0xff35f2ffu;
        if(!g->dist || z> -R4_SHIP_Z-R4_ONE/3 || z< -DRAW_FAR)continue;
        n+=facing(x-o,0,z,t,h,c,512,0,walls,vp,projection,out+n,cap-n,&st);
        n+=facing(x+o,0,z,t,h,c,512,0,walls,vp,projection,out+n,cap-n,&st);
        n+=facing(x,h,z,o,t,c,512,0,walls,vp,projection,out+n,cap-n,&st);
        n+=facing(x,-h,z,o,t,c,512,0,walls,vp,projection,out+n,cap-n,&st);
        if(i==nearest && z>-11*R4_ONE) {
            const int32_t edge[3]={x-o+t,x+o-t,h-t};
            for(unsigned side=0;side<3;++side) {
                rast_view_vertex v[4];int32_t back=z-2*R4_ONE/5;
                uint32_t shade=side==0?0xff518d9e:side==1?0xff31596e:0xff224359;
                if(side<2) {
                    vertex(v,edge[side],h,z,512,0,shade);
                    vertex(v+1,edge[side],h,back,1008,0,shade);
                    vertex(v+2,edge[side],-h,back,1008,496,shade);
                    vertex(v+3,edge[side],-h,z,512,496,shade);
                } else {
                    vertex(v,x-o,edge[side],z,512,0,shade);
                    vertex(v+1,x+o,edge[side],z,1008,0,shade);
                    vertex(v+2,x+o,edge[side],back,1008,496,shade);
                    vertex(v+3,x-o,edge[side],back,512,496,shade);
                }
                if(side==1){rast_view_vertex tmp=v[1];v[1]=v[3];v[3]=tmp;}
                n+=polygon(v,4,vp,walls,projection,out+n,cap-n,&st);
            }
        }
    }
    /* 速度线:贴两侧墙的细亮条,世界系固定距离,随前进自然扑面。
       距离相位取 (d+off) 对 12m 窗取模,确定式,无随机数。 */
    {
        const int32_t offs[6]={0,7*R4_ONE/2,5*R4_ONE,17*R4_ONE/2,10*R4_ONE,27*R4_ONE/2};
        for(unsigned i=0;i<6;++i) {
            uint32_t ahead=((w->d+(uint32_t)offs[i])%(12u*R4_ONE))+2u*R4_ONE;
            int32_t z=(int32_t)w->d-(int32_t)ahead;
            if(z>=-(R4_SHIP_Z+2*R4_ONE) || z< -DRAW_FAR)continue;
            int32_t z1=z-5*R4_ONE/2;
            int32_t sx=((i&1u)?1:-1)*(R4_HALF_W-R4_ONE/4);
            int32_t y0=-R4_HALF_H+ (int32_t)(i+1u)*(R4_HALF_H/4);
            int32_t y1=y0+R4_HALF_H/5;
            uint32_t c=(i&1u)?0xffd8f4ffu:0xff9adfffu;
            rast_view_vertex v[4];
            vertex(v,sx,y0,z,0,0,c);vertex(v+1,sx,y0,z1,0,0,c);
            vertex(v+2,sx,y1,z1,0,0,c);vertex(v+3,sx,y1,z,0,0,c);
            n+=polygon(v,4,vp,flat,projection,out+n,cap-n,&st);
        }
    }
    /* 顶灯光带:走廊轴向每 4m 一条,亮度按帧脉动(青白),读作环境在发光。 */
    {
        uint32_t seg=w->d/(4u*R4_ONE);
        for(unsigned j=0;j<4;++j) {
            uint32_t W=(seg+j)*4u*R4_ONE+3u*R4_ONE/2;
            int32_t z=(int32_t)w->d-(int32_t)W;
            if(z>=-(R4_SHIP_Z+R4_ONE) || z< -DRAW_FAR)continue;
            unsigned pl=170u+85u*fx_sin((uint16_t)(w->ticks*1093u+j*8192u))/16384;
            uint32_t c=0xff000000u|((pl*190u/255)<<16)|((pl*235u/255)<<8)|pl;
            rast_view_vertex v[4];
            vertex(v,-2*R4_ONE,R4_HALF_H-R4_ONE/64,z,0,0,c);
            vertex(v+1,2*R4_ONE,R4_HALF_H-R4_ONE/64,z,0,0,c);
            vertex(v+2,2*R4_ONE,R4_HALF_H-R4_ONE/64,z-R4_ONE/3,0,0,c);
            vertex(v+3,-2*R4_ONE,R4_HALF_H-R4_ONE/64,z-R4_ONE/3,0,0,c);
            n+=polygon(v,4,vp,flat,projection,out+n,cap-n,&st);
        }
    }
    if(ladder>=2 && w->enemy.alive) {
        uint32_t c=w->enemy_hit?0xffffffffu:0xffffd890u;
        int32_t z=(int32_t)w->d+w->enemy.z;
        if(z< -8*R4_ONE) n+=facing(w->enemy.x,w->enemy.y,z,
                R4_ONE/2,R4_ONE/2,c,0,512,keyed,vp,projection,out+n,cap-n,&st);
        else {
            /* Kenney spaceCraft1(CC0)减面版:36 顶点 76 面,横滚绕前进轴。 */
            const int16_t bank=fx_cos((uint16_t)(w->ticks*370u))*4/5;
            const int32_t cosine=FX16_ONE-fx16_mul(bank,bank)/2;
            const int16_t elight[3]={-9000,14000,-8500};
            const int16_t ehalf[3]={-4000,6500,-14500};
            rast_view_vertex v[R4_ENEMY_VERTICES];rast_vertex screen[R4_ENEMY_VERTICES];
            for(unsigned j=0;j<R4_ENEMY_VERTICES;++j) {
                const int16_t *local=r4_enemy_vertices[j];
                int32_t x=fx16_mul(local[0],cosine)-fx16_mul(local[1],bank);
                int32_t y=fx16_mul(local[0],bank)+fx16_mul(local[1],cosine);
                vertex(v+j,w->enemy.x+x,w->enemy.y+y,z+local[2],0,0,0);
            }
            int cached=!R4_DISABLE_VERTEX_CACHE && !r4_project_inside(v,R4_ENEMY_VERTICES,projection,
                R4_NEAR_Z,R4_FAR_Z,vp,screen);
            for(unsigned j=0;j<R4_ENEMY_FACES;++j) {
                rast_view_vertex tri[3];rast_vertex projected[3];
                uint32_t mat=r4_enemy_material[j];
                for(unsigned k=0;k<3;++k){
                    unsigned idx=r4_enemy_faces[j][k];
                    tri[k]=v[idx];
                    tri[k].color=w->enemy_hit?0xffffffffu:
                        r4_light_color(r4_enemy_normals[idx],elight,ehalf,mat);
                    if(cached){projected[k]=screen[idx];projected[k].color=tri[k].color;}
                }
                int emitted=cached?gfx_setup_polygon(projected,3,vp,&frame,flat,out+n,cap-n,&st):
                    gfx_setup_view(tri,3,projection,R4_NEAR_Z,R4_FAR_Z,vp,&frame,flat,out+n,cap-n,&st);
                if(emitted<0)emit_failed=1;else n+=(uint32_t)emitted;
            }
        }
    }
    /* Shared projection, smooth fuselage, hard metal wings and glass cockpit. */
    if(w->shield) {
        rast_view_vertex v[R4_SHIP_VERTICES];rast_vertex screen[R4_SHIP_VERTICES];
        uint32_t colors[4][R4_SHIP_VERTICES];
        int32_t roll=w->bank;
        int32_t cosine=FX16_ONE-fx16_mul(roll,roll)/2;
        int16_t light[3]={(int16_t)((fx16_mul(-32000,cosine)+fx16_mul(44000,roll))/4),
            (int16_t)((fx16_mul(44000,cosine)-fx16_mul(-32000,roll))/4),-8500};
        int16_t half[3]={-4000,6500,-14500};
        const uint32_t material[4]={0x00b9d7e5,0x002a3d55,0x00367fc5,0x00ff4fd8};
        for(unsigned i=0;i<R4_SHIP_VERTICES;++i) {
            const int16_t *local=r4_ship_vertices[i];
            int32_t x=fx16_mul(local[0],cosine)-fx16_mul(local[1],roll);
            int32_t y=fx16_mul(local[0],roll)+fx16_mul(local[1],cosine);
            int32_t z=local[2]+w->pitch*local[1]/5+w->strafe*local[0]/6;
            for(unsigned m=0;m<4;++m)colors[m][i]=r4_light_color(r4_ship_normals[i],light,half,material[m]);
            vertex(v+i,x+w->cam_x,y+w->ship_y,z-R4_SHIP_Z,0,0,colors[0][i]);
        }
        int cached=!R4_DISABLE_VERTEX_CACHE && !r4_project_inside(v,R4_SHIP_VERTICES,projection,
            R4_NEAR_Z,R4_FAR_Z,vp,screen);
        for(unsigned i=0;i<R4_SHIP_FACES;++i) {
            rast_view_vertex tri[3];rast_vertex projected[3];
            for(unsigned j=0;j<3;++j) {
                unsigned index=r4_ship_faces[i][j];tri[j]=v[index];
                tri[j].color=colors[r4_ship_material[i]][index];
                if(w->invulnerable && (w->ticks&4u))tri[j].color=0xffe0f8ff;
                if(cached){projected[j]=screen[index];projected[j].color=tri[j].color;}
            }
            int emitted=cached?gfx_setup_polygon(projected,3,vp,&frame,flat,out+n,cap-n,&st):
                gfx_setup_view(tri,3,projection,R4_NEAR_Z,R4_FAR_Z,vp,&frame,flat,out+n,cap-n,&st);
            if(emitted<0)emit_failed=1;else n+=(uint32_t)emitted;
        }
        for(unsigned engine=0;engine<2;++engine) {
            const rast_view_vertex *origin=v+(engine?8:7);
            rast_view_vertex flame[3];
            vertex(flame,origin->x-1200,origin->y,origin->z+1500,0,0,0xffd8ffff);
            vertex(flame+1,origin->x+1200,origin->y,origin->z+1500,0,0,0xffa0edff);
            vertex(flame+2,origin->x,origin->y-500,origin->z+10000+(int32_t)(w->ticks%4u)*750,
                0,0,0xff124a88);
            n+=polygon(flame,3,vp,flat,projection,out+n,cap-n,&st);
        }
    }
    for(unsigned i=0;i<R4_PBULLET_MAX;++i) {
        const r4_bullet *b=w->pbul+i;
        if(!b->alive)continue;
        /* 自机弹=青色光矢:十字双片沿 z 拉长,任意视角可见。 */
        int32_t z=(int32_t)w->d+b->z,z1=z-4*R4_ONE/5;
        for(unsigned axis=0;axis<2;++axis) {
            rast_view_vertex v[4];
            int32_t dx=axis?0:R4_ONE/48,dy=axis?R4_ONE/48:0;
            vertex(v,b->x-dx,b->y-dy,z,0,0,0xffbdf6ff);
            vertex(v+1,b->x+dx,b->y+dy,z,0,0,0xff35f2ff);
            vertex(v+2,b->x+dx,b->y+dy,z1,0,0,0xff35f2ff);
            vertex(v+3,b->x-dx,b->y-dy,z1,0,0,0xffbdf6ff);
            n+=polygon(v,4,vp,flat,projection,out+n,cap-n,&st);
        }
    }
    for(unsigned i=0;i<R4_EBULLET_MAX;++i) {
        const r4_bullet *b=w->ebul+i;
        if(!b->alive)continue;
        n+=facing(b->x,b->y,(int32_t)w->d+b->z,R4_BULLET_HALF,R4_BULLET_HALF,
            0xffffffff,512,512,keyed,vp,projection,out+n,cap-n,&st);
    }
    if(w->blast_ticks)for(unsigned i=0;i<6;++i) {
        const int32_t directions[6][2]={{-65536,0},{-32768,56756},{32768,56756},
            {65536,0},{32768,-56756},{-32768,-56756}};
        int32_t radius=(20-(int32_t)w->blast_ticks)*R4_ONE/24;
        int32_t x=w->blast.x+fx16_mul(directions[i][0],radius);
        int32_t y=w->blast.y+fx16_mul(directions[i][1],radius)-radius/3;
        int32_t z=(int32_t)w->d+w->blast.z+(int32_t)(i%3u)*R4_ONE/7;
        uint32_t c=i%2?0xffffc888:0xffff92aa;
        rast_view_vertex v[3];
        vertex(v,x-R4_ONE/18,y+R4_ONE/20,z,0,0,c);
        vertex(v+1,x+R4_ONE/13,y,z,0,0,0xfffff0d0);
        vertex(v+2,x-R4_ONE/25,y-R4_ONE/12,z,0,0,0xff77385c);
        n+=polygon(v,3,vp,flat,projection,out+n,cap-n,&st);
    }
    if(counts){counts->backface+=st.backface;counts->sliver+=st.sliver;
        counts->clipped+=st.clipped;counts->emitted+=st.emitted;}
    return emit_failed?UINT32_MAX:n;
}

int r4_world_prewarm(const fx16_mat4 *projection,const rast_viewport *vp,
                     uint32_t ladder,rast_record *scratch,uint32_t cap)
{
    r4_world w;r4_world_init(&w,R4_SEED);
    for(unsigned i=0;i<WALL_PHASES;++i) {
        w.ticks=i;w.d=(uint32_t)(((uint64_t)i*4u*R4_ONE)/R4_TICK_HZ);
        if(r4_world_emit(&w,projection,vp,ladder,scratch,cap,NULL)==UINT32_MAX)return -1;
    }
    return 0;
}
