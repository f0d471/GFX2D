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
#ifndef R4_DISABLE_FLIGHT_CACHE
#define R4_DISABLE_FLIGHT_CACHE 0
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
    /* 门环整体删除(用户定案:视觉与判定一起走,不留隐形墙;火力走敌机自身循环)。 */
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

/* 补光下限:方向光照不到的半球按 base×44% 兜底(逐通道 max)。
   相机固定在后下方,船腹/敌机腹全在暗半球,没有兜底就是黑剪影。
   纯 normal 的函数,hw/sw 同几何同像素,契约不受影响。 */
static uint32_t r4_fill_color(const int16_t normal[3],const int16_t light[3],
                              const int16_t halfv[3],uint32_t base)
{
    uint32_t c=r4_light_color(normal,light,halfv,base);
    uint32_t fr=((base>>16)&255u)*112u/255u,fg=((base>>8)&255u)*112u/255u,fb=(base&255u)*112u/255u;
    uint32_t r=(c>>16)&255u,g=(c>>8)&255u,b=c&255u;
    if(r<fr)r=fr;
    if(g<fg)g=fg;
    if(b<fb)b=fb;
    return 0xff000000u|(r<<16)|(g<<8)|b;
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
    /* 深灰蓝哑面(回退);墙上的蓝描线由 atlas 纹素自带(低红高绿蓝),
       过本乘色后读作青蓝曲线。近处稍暗、向远端渐亮接雾幕光。 */
    uint32_t distance=(uint32_t)(-z);
    unsigned shade=150u+(uint32_t)(60u*distance)/(9u*R4_ONE+distance);
    const unsigned light[8]={210,255,196,182,188,240,200,196};
    shade=shade*light[face]/255u;
    unsigned r,g,b;
    if(face==1||face==2||face==5||face==6){ r=shade*112u/255u; g=shade*126u/255u; b=shade*150u/255u; }
    else { r=shade*100u/255u; g=shade*114u/255u; b=shade*152u/255u; }
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
static uint32_t ridge_terrain_span(const r4_world *w, const fx16_mat4 *projection,
                                   const rast_viewport *vp, rast_record *out,
                                   uint32_t cap, rast_setup_stats *st,unsigned first,unsigned last)
{
    uint32_t n = 0u;
    const uint32_t *pal = ridge_palettes[ridge_zone(w)];
    int32_t dx = (RIDGE_HALF_X * 2 * R4_ONE) / (int32_t)RIDGE_NX;
    int32_t dz = ((RIDGE_Z1 - RIDGE_Z0) * R4_ONE) / (int32_t)RIDGE_NZ;
    int32_t hl[RIDGE_NZ + 1u];
    for (uint32_t j = 0; j <= RIDGE_NZ; ++j)
        hl[j] = ridge_ground(-RIDGE_HALF_X * R4_ONE+(int32_t)first*dx, w->d,
                             RIDGE_Z0 + (int32_t)j * (dz / R4_ONE));
    for (uint32_t i = first; i < last; ++i) {
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

static uint32_t ridge_terrain_emit(const r4_world *w,const fx16_mat4 *projection,
                                  const rast_viewport *vp,rast_record *out,
                                  uint32_t cap,rast_setup_stats *st)
{return ridge_terrain_span(w,projection,vp,out,cap,st,0,RIDGE_NX);}

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

/* 地面影子：贴着山面的一小块暗斑（0.4m×0.52m），四角各自取 ridge_ground，
   随坡面起伏；只比地面暗两成——是影子不是地毯。 */
static uint32_t ridge_shadow_emit(const r4_world *w, const fx16_mat4 *projection,
                                  const rast_viewport *vp, rast_record *out,
                                  uint32_t cap, rast_setup_stats *st)
{
    uint32_t n = 0u;
    const int32_t zahead = (int32_t)(R4_SHIP_Z >> 16);
    const int32_t cx = w->cam_x;
    const int32_t hw = 20 * R4_ONE / 100, hl = 26 * R4_ONE / 100;
    const uint32_t *pal = ridge_palettes[ridge_zone(w)];
    const uint32_t fog = pal[5];
    int32_t g0 = ridge_ground(cx - hw, w->d, zahead);
    int32_t g1 = ridge_ground(cx + hw, w->d, zahead);
    int32_t g2 = ridge_ground(cx + hw, w->d, zahead + 1);
    int32_t g3 = ridge_ground(cx - hw, w->d, zahead + 1);
    uint32_t col = ridge_fog(ridge_apply(pal[3], 205u), R4_SHIP_Z, fog);
    rast_view_vertex v[4];
    vertex(v, cx - hw, g0 + R4_ONE / 12, -(int32_t)R4_SHIP_Z, 0, 0, col);
    vertex(v + 1, cx + hw, g1 + R4_ONE / 12, -(int32_t)R4_SHIP_Z, 0, 0, col);
    vertex(v + 2, cx + hw, g2 + R4_ONE / 12, -(int32_t)R4_SHIP_Z - hl, 0, 0, col);
    vertex(v + 3, cx - hw, g3 + R4_ONE / 12, -(int32_t)R4_SHIP_Z - hl, 0, 0, col);
    { rast_view_vertex tmp = v[1]; v[1] = v[3]; v[3] = tmp; }
    n += ridge_poly(v, 4, vp, projection, out + n, cap - n, st);
    return n;
}

/* 经典纸飞机（dart 折法，用户参考图）：尖头、两片后掠翼、中央下折龙骨。
   三个三角面 + 横滚/俯仰（走廊自机同一套约定），受击无敌窗闪白。
   纯纸白双色：朝阳翼亮、背光翼暗，龙骨再暗一档出折痕。 */
static uint32_t ridge_plane_emit(const r4_world *w, const fx16_mat4 *projection,
                                 const rast_viewport *vp, rast_record *out,
                                 uint32_t cap, rast_setup_stats *st)
{
    uint32_t n = 0u;
    const int32_t S = 55 * R4_ONE / 100;
    int32_t cx = w->cam_x, cy = w->ship_y, cz = -(int32_t)R4_SHIP_Z;
    const int32_t N[3]  = {0,   2 * S / 100, -60 * S / 100};   /* 机头 */
    const int32_t TL[3] = {-34 * S / 100, 10 * S / 100, 38 * S / 100};
    const int32_t TR[3] = { 34 * S / 100, 10 * S / 100, 38 * S / 100};
    const int32_t TC[3] = {0,    4 * S / 100,  40 * S / 100};  /* 折线尾点 */
    const int32_t K[3]  = {0,  -26 * S / 100,  30 * S / 100};  /* 龙骨下缘 */
    const int32_t *faces[3][3] = {
        {N, TL, TC},   /* 左翼（朝阳，亮） */
        {N, TC, TR},   /* 右翼（背光，暗） */
        {N, TC, K}};   /* 中央龙骨（竖折面） */
    static const uint32_t cols[3] = {0xfcf9f1, 0xe9e3d3, 0xd9d3c1};
    int32_t roll = w->bank;
    int32_t cosine = FX16_ONE - fx16_mul(roll, roll) / 2;
    uint32_t blink = (w->invulnerable && (w->ticks & 4u)) ? 0xffe8f8u : 0u;
    for (uint32_t f = 0u; f < 3u; ++f) {
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

/* 低模松树/岩石：谷缘世界锚定点位，随前进掠过。
   锥体全部 45° 转向（面朝相机 → 剪影是三角不是菱形）；
   松树=树干+两层收分锥；岩石单锥。朝日面亮、背日面暗。 */
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
        if (ridge_zone(w) == 2u && typ[i] != 0u && (i & 1u)) continue;   /* 峡谷树减半 */
        if (typ[i] == 0u) {                       /* 岩石：单锥 45° 转向 */
            const int32_t r = 32 * R4_ONE / 100;
            const int32_t r707 = fx16_mul(r, 46341);
            const int32_t top = g + 3 * R4_ONE / 10;
            const uint32_t lit = ridge_fog(ridge_apply(pal[0], 215u), ahead * R4_ONE, fog);
            const uint32_t dark = ridge_fog(ridge_apply(pal[0], 150u), ahead * R4_ONE, fog);
            const int32_t px[4] = {r707, -r707, -r707, r707};
            const int32_t pz[4] = {r707, r707, -r707, -r707};
            for (uint32_t k = 0u; k < 4u; ++k) {
                uint32_t col = (k == 0u || k == 3u) ? lit : dark;
                rast_view_vertex v[3];
                vertex(v, x + px[k], g, z + pz[k], 0, 0, col);
                vertex(v + 1, x + px[(k + 1u) & 3u], g, z + pz[(k + 1u) & 3u], 0, 0, col);
                vertex(v + 2, x, top, z, 0, 0, col);
                { rast_view_vertex tmp = v[1]; v[1] = v[2]; v[2] = tmp; }
                n += ridge_poly(v, 3, vp, projection, out + n, cap - n, st);
            }
            continue;
        }
        {                                         /* 松树：树干 + 一/两层收分锥 */
            const uint32_t lit = ridge_fog(ridge_apply(pal[4], 245u), ahead * R4_ONE, fog);
            const uint32_t dark = ridge_fog(ridge_apply(pal[4], 150u), ahead * R4_ONE, fog);
            const uint32_t trunk = ridge_fog(0x6b4f3a, ahead * R4_ONE, fog);
            const int32_t tr = 7 * R4_ONE / 100;
            const int32_t tpx[4] = {tr, tr, -tr, -tr};
            const int32_t tpz[4] = {tr, -tr, -tr, tr};
            const int32_t tiers = (typ[i] == 2u) ? 2u : 1u;
            int32_t r1 = 50 * R4_ONE / 100, r2 = 36 * R4_ONE / 100;
            int32_t b1 = g + R4_ONE / 5, a1 = b1 + 11 * R4_ONE / 20;
            int32_t b2 = g + 9 * R4_ONE / 20, a2 = b2 + 9 * R4_ONE / 20;
            int32_t rr = r1, bb = b1, aa = a1;
            for (uint32_t k = 0u; k < 4u; ++k) {  /* 树干（小方柱锥） */
                rast_view_vertex v[3];
                vertex(v, x + tpx[k], g - R4_ONE / 20, z + tpz[k], 0, 0, trunk);
                vertex(v + 1, x + tpx[(k + 1u) & 3u], g - R4_ONE / 20, z + tpz[(k + 1u) & 3u], 0, 0, trunk);
                vertex(v + 2, x, g + R4_ONE / 4, z, 0, 0, trunk);
                { rast_view_vertex tmp = v[1]; v[1] = v[2]; v[2] = tmp; }
                n += ridge_poly(v, 3, vp, projection, out + n, cap - n, st);
            }
            for (uint32_t t = 0u; t < (uint32_t)tiers; ++t) {
                const int32_t r707 = fx16_mul(rr, 46341);
                const int32_t px[4] = {r707, -r707, -r707, r707};
                const int32_t pz[4] = {r707, r707, -r707, -r707};
                for (uint32_t k = 0u; k < 4u; ++k) {
                    uint32_t col = (k == 0u || k == 3u) ? lit : dark;
                    rast_view_vertex v[3];
                    vertex(v, x + px[k], bb, z + pz[k], 0, 0, col);
                    vertex(v + 1, x + px[(k + 1u) & 3u], bb, z + pz[(k + 1u) & 3u], 0, 0, col);
                    vertex(v + 2, x, aa, z, 0, 0, col);
                    { rast_view_vertex tmp = v[1]; v[1] = v[2]; v[2] = tmp; }
                    n += ridge_poly(v, 3, vp, projection, out + n, cap - n, st);
                }
                rr = r2; bb = b2; aa = a2;
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

/* Environment depends on integer distance, projection and viewport only.
 * Two fixed pages hold current/next metre; shadow and plane stay dynamic.
 * Future emitters that depend on tick/input must extend this key or opt out. */
typedef struct {
    rast_record terrain[320],props[128],clouds[64];
    uint32_t metre,nt,np,nc;
    unsigned column;
    int usable,ready;
} flight_page;
static flight_page flight_pages[2];
static rast_record flight_scratch[R4_RECORD_CAP];
static fx16_mat4 flight_projection;
static rast_viewport flight_viewport;
static unsigned flight_current;
static int flight_key_valid;
static r4_flight_cache_stats flight_stats;

void r4_flight_cache_reset(void)
{
    flight_key_valid=0;
    memset(&flight_stats,0,sizeof flight_stats);
}

r4_flight_cache_stats r4_flight_cache_status(void) {return flight_stats;}

static void flight_page_begin(flight_page *page,uint32_t metre)
{
    page->metre=metre;page->nt=page->np=page->nc=0;
    page->column=0;page->usable=1;page->ready=0;
}

static void flight_page_step(flight_page *page,const r4_world *world,
                             const fx16_mat4 *projection,const rast_viewport *vp)
{
    if(page->ready)return;
    r4_world sample=*world;sample.d=page->metre<<16;
    rast_setup_stats st={0};unsigned column=page->column;
    uint32_t n=ridge_terrain_span(&sample,projection,vp,flight_scratch,
                                 R4_RECORD_CAP,&st,column,column+1u);
    if(n>320u-page->nt)page->usable=0;
    else if(page->usable) {
        memcpy(page->terrain+page->nt,flight_scratch,n*sizeof *flight_scratch);page->nt+=n;
    }
    if(column==0u) {
        n=ridge_props_emit(&sample,projection,vp,flight_scratch,R4_RECORD_CAP,&st);
        if(n>128u)page->usable=0;
        else {memcpy(page->props,flight_scratch,n*sizeof *flight_scratch);page->np=n;}
    }
    if(column==1u) {
        n=ridge_clouds_emit(&sample,projection,vp,flight_scratch,R4_RECORD_CAP,&st);
        if(n>64u)page->usable=0;
        else {memcpy(page->clouds,flight_scratch,n*sizeof *flight_scratch);page->nc=n;}
    }
    ++page->column;++flight_stats.columns;
    page->ready=page->column==RIDGE_NX;
}

static flight_page *flight_select(const r4_world *world,const fx16_mat4 *projection,
                                  const rast_viewport *vp)
{
    uint32_t metre=world->d>>16;
#ifdef R4_INJECT_FLIGHT_STALE_VIEW
    int same=flight_key_valid;
#else
    int same=flight_key_valid && !memcmp(projection,&flight_projection,sizeof *projection) &&
             !memcmp(vp,&flight_viewport,sizeof *vp);
#endif
    if(!same || (flight_pages[flight_current].metre!=metre &&
                 flight_pages[flight_current^1u].metre!=metre)) {
        flight_projection=*projection;flight_viewport=*vp;flight_current=0;flight_key_valid=1;
        flight_page_begin(&flight_pages[0],metre);
        flight_page_begin(&flight_pages[1],(metre+1u)&65535u);
        for(unsigned p=0;p<2u;++p)while(!flight_pages[p].ready)
            flight_page_step(&flight_pages[p],world,projection,vp);
        ++flight_stats.cold;
    }
    if(flight_pages[flight_current].metre!=metre) {
        flight_current^=1u;
        flight_page *current=&flight_pages[flight_current];
        if(!current->ready) {
            ++flight_stats.late;
            while(!current->ready)flight_page_step(current,world,projection,vp);
        }
        flight_page_begin(&flight_pages[flight_current^1u],(metre+1u)&65535u);
    }
    return &flight_pages[flight_current];
}

int r4_flight_prewarm(const r4_world *world,const fx16_mat4 *projection,const rast_viewport *vp)
{
    r4_flight_cache_reset();
    if(!R4_DISABLE_FLIGHT_CACHE)(void)flight_select(world,projection,vp);
    return 0;
}

static uint32_t ridge_emit_cached(const r4_world *world,const fx16_mat4 *projection,
                                  const rast_viewport *vp,rast_record *out,uint32_t cap,
                                  rast_setup_stats *stats)
{
    if(R4_DISABLE_FLIGHT_CACHE || cap!=R4_RECORD_CAP)
        return ridge_emit(world,projection,vp,out,cap,stats);
    flight_page *page=flight_select(world,projection,vp);
    /* Seven columns fit within the seven/eight 30Hz ticks per metre. Props
     * and clouds are spread over the first two jobs; no boundary-only rebuild. */
    flight_page_step(&flight_pages[flight_current^1u],world,projection,vp);
    if(!page->usable) {
        ++flight_stats.fallback;return ridge_emit(world,projection,vp,out,cap,stats);
    }
    uint32_t n=page->nt;
    memcpy(out,page->terrain,n*sizeof *out);
    n+=ridge_shadow_emit(world,projection,vp,out+n,cap-n,stats);
    n+=ridge_plane_emit(world,projection,vp,out+n,cap-n,stats);
    if(cap-n>=24u) {
        if(cap-n>=page->np+20u) {memcpy(out+n,page->props,page->np*sizeof *out);n+=page->np;}
        else n+=ridge_props_emit(world,projection,vp,out+n,cap-n,stats);
    }
    if(cap-n>=16u) {
        if(cap-n>=page->nc+10u) {memcpy(out+n,page->clouds,page->nc*sizeof *out);n+=page->nc;}
        else n+=ridge_clouds_emit(world,projection,vp,out+n,cap-n,stats);
    }
    ++flight_stats.hits;return n;
}

#ifdef R4_FLIGHT_TEST
uint32_t r4_flight_reference(const r4_world *world,const fx16_mat4 *projection,
                            const rast_viewport *vp,rast_record *out,uint32_t cap)
{
    rast_setup_stats stats={0};return ridge_emit(world,projection,vp,out,cap,&stats);
}
#endif

uint32_t r4_world_emit(const r4_world *w,const fx16_mat4 *projection,
                       const rast_viewport *vp,uint32_t ladder,
                       rast_record *out,uint32_t cap,rast_setup_stats *counts)
{
    rast_setup_stats st={0};
    uint32_t n=0,flat=RAST_VCOLOR_EN;
    uint32_t walls=flat|(ladder>=2?RAST_TEX_EN:0);
    uint32_t keyed=ladder>=2?(RAST_TEX_EN|RAST_KEY_EN|RAST_VCOLOR_EN):flat;
    if (w->scene == 2u)
        return ridge_emit_cached(w, projection, vp, out, cap, &st);
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
    /* 雾幕封底:隧道尽头的光——机高最亮,向顶/底渐暗(两段 Gouraud 拼出中央亮带),
       盖住 33m 外的一切,远端分段回绕的接缝由此不可见。 */
    {
        const uint32_t ctop=0xff101a30u,cbright=0xff9fd4f0u,cbot=0xff0a1020u;
        rast_view_vertex v[4];
        vertex(v,-R4_HALF_W,R4_HALF_H,-33*R4_ONE,0,0,ctop);
        vertex(v+1,R4_HALF_W,R4_HALF_H,-33*R4_ONE,0,0,ctop);
        vertex(v+2,R4_HALF_W,0,-33*R4_ONE,0,0,cbright);
        vertex(v+3,-R4_HALF_W,0,-33*R4_ONE,0,0,cbright);
        n+=polygon(v,4,vp,flat,projection,out+n,cap-n,&st);
        vertex(v,-R4_HALF_W,0,-33*R4_ONE,0,0,cbright);
        vertex(v+1,R4_HALF_W,0,-33*R4_ONE,0,0,cbright);
        vertex(v+2,R4_HALF_W,-R4_HALF_H,-33*R4_ONE,0,0,cbot);
        vertex(v+3,-R4_HALF_W,-R4_HALF_H,-33*R4_ONE,0,0,cbot);
        n+=polygon(v,4,vp,flat,projection,out+n,cap-n,&st);
    }
    /* 门环绘制整体删除(用户定案"本来就没啥用"):视觉与判定一起走。
       速度感交给灯带节律与敌机,走廊=哑面墙+灯带+雾幕光三件套。 */
    /* 顶灯光带:走廊轴向每 4m 一条,恒亮青白——发光只留灯带一种,
       不再撒细亮条。 */
    {
        uint32_t seg=w->d/(4u*R4_ONE);
        const unsigned pl=205u;
        for(unsigned j=0;j<4;++j) {
            uint32_t W=(seg+j)*4u*R4_ONE+3u*R4_ONE/2;
            int32_t z=(int32_t)w->d-(int32_t)W;
            if(z>=-(R4_SHIP_Z+R4_ONE) || z< -DRAW_FAR)continue;
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
        int32_t z=(int32_t)w->d+w->enemy.z;
        {
            /* Omen(CC0)减面版:48 顶点 111 面,横滚绕前进轴。
               全程同一模型——远景不再切 billboard,远近切换突变由此消失;
               掠过相机时被近平面自然裁掉,也没有"到 8m 换材质"的跳变。 */
            const int16_t bank=fx_cos((uint16_t)(w->ticks*370u))*4/5;
            const int32_t cosine=FX16_ONE-fx16_mul(bank,bank)/2;
            const int16_t elight[3]={-6000,12000,11000};
            const int16_t ehalf[3]={-4000,6500,12000};
            /* 材质索引 → 实色:琥珀机身/板岩暗件/赤红饰条(与 2D 身份色同族)。 */
            const uint32_t ematerial[3]={0x00d8a058,0x004a5e74,0x00c85a40};
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
                        r4_fill_color(r4_enemy_normals[idx],elight,ehalf,ematerial[mat]);
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
            (int16_t)((fx16_mul(44000,cosine)-fx16_mul(-32000,roll))/4),10000};
        int16_t half[3]={-4000,6500,14000};
        const uint32_t material[4]={0x006e8eae,0x00364858,0x00bfe8ff,0x00ff4fd8};
        for(unsigned i=0;i<R4_SHIP_VERTICES;++i) {
            const int16_t *local=r4_ship_vertices[i];
            int32_t x=fx16_mul(local[0],cosine)-fx16_mul(local[1],roll);
            int32_t y=fx16_mul(local[0],roll)+fx16_mul(local[1],cosine);
            int32_t z=local[2]+w->pitch*local[1]/5+w->strafe*local[0]/6;
            for(unsigned m=0;m<4;++m)colors[m][i]=r4_fill_color(r4_ship_normals[i],light,half,material[m]);
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
