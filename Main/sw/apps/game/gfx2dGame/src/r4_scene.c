/******************************************************************************
* @file r4_scene.c: 固定 30Hz 的世界逻辑与真实三维几何
* @brief 世界/自机/弹幕共享 R1 投影、setup 与 z；相机周期平移，自机独立横移。
*        墙面按世界深度分段，近缘延伸至视口外；整个物体发射或显式失败。
******************************************************************************/
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "board.h"
#include "r4.h"
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

void r4_world_tick(r4_world *w,uint32_t keys,uint32_t *fail)
{
    int32_t dir=((keys&BOARD_KEY_RIGHT)!=0)-((keys&BOARD_KEY_LEFT)!=0);
    int32_t vertical=((keys&BOARD_KEY_UP)!=0)-((keys&BOARD_KEY_DOWN)!=0);
    (void)fail;
    ++w->ticks;
    if(keys&(BOARD_KEY_UP|BOARD_KEY_DOWN|BOARD_KEY_LEFT|BOARD_KEY_RIGHT))w->manual=1;
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
    if((keys&BOARD_KEY_BOMB) && !(w->last_keys&BOARD_KEY_BOMB) && w->bombs) {
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
        r=r4_setup_polygon(screen,n,vp,&frame,flags,out,cap,st);
    else r=r4_setup_view(v,n,p,R4_NEAR_Z,R4_FAR_Z,vp,&frame,flags,out,cap,st);
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
    uint32_t distance=(uint32_t)(-z);
    unsigned shade=30u+(uint32_t)(202u*9u*R4_ONE)/(9u*R4_ONE+distance);
    const unsigned light[8]={200,255,154,175,142,212,182,158};
    shade=shade*light[face]/255u;
    unsigned r=shade*190u/255u,g=shade*223u/255u,b=shade;
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

uint32_t r4_world_emit(const r4_world *w,const fx16_mat4 *projection,
                       const rast_viewport *vp,uint32_t ladder,
                       rast_record *out,uint32_t cap,rast_setup_stats *counts)
{
    rast_setup_stats st={0};
    uint32_t n=0,flat=RAST_VCOLOR_EN;
    uint32_t walls=flat|(ladder>=2?RAST_TEX_EN:0);
    uint32_t keyed=ladder>=2?(RAST_TEX_EN|RAST_KEY_EN|RAST_VCOLOR_EN):flat;
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
        uint32_t c=((g->dist/R4_GATE_SPACING)%3u==0)?0xffff6088u:0xff60ffb8u;
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
    if(ladder>=2 && w->enemy.alive) {
        uint32_t c=w->enemy_hit?0xffffffffu:0xffffd890u;
        int32_t z=(int32_t)w->d+w->enemy.z;
        if(z< -8*R4_ONE) n+=facing(w->enemy.x,w->enemy.y,z,
                R4_ONE/2,R4_ONE/2,c,0,512,keyed,vp,projection,out+n,cap-n,&st);
        else {
            const int32_t model[6][3]={{0,0,12000},{-31000,-1800,-4000},
                {31000,-1800,-4000},{0,17000,-8000},{0,-13500,-8000},{0,0,-21000}};
            const unsigned faces[8][3]={{0,1,3},{0,3,2},{0,4,1},{0,2,4},
                                      {5,3,1},{5,2,3},{5,1,4},{5,4,2}};
            const uint32_t colors[6]={0xffffdfaa,0xff713542,0xffb75e5a,
                                      0xffe6a17b,0xff4a344a,0xffa44c65};
            rast_view_vertex v[6];rast_vertex screen[6];
            int32_t bank=fx_cos((uint16_t)(w->ticks*370u))*4/5;
            int32_t cosine=FX16_ONE-fx16_mul(bank,bank)/2;
            for(unsigned j=0;j<6;++j) {
                int32_t x=fx16_mul(model[j][0],cosine)-fx16_mul(model[j][1],bank);
                int32_t y=fx16_mul(model[j][0],bank)+fx16_mul(model[j][1],cosine);
                vertex(v+j,w->enemy.x+x,w->enemy.y+y,z+model[j][2],0,0,
                    w->enemy_hit?0xffffffff:colors[j]);
            }
            int cached=!R4_DISABLE_VERTEX_CACHE && !r4_project_inside(v,6,projection,
                R4_NEAR_Z,R4_FAR_Z,vp,screen);
            for(unsigned j=0;j<8;++j) {
                rast_view_vertex tri[3];rast_vertex projected[3];
                for(unsigned k=0;k<3;++k){tri[k]=v[faces[j][k]];if(cached)projected[k]=screen[faces[j][k]];}
                int emitted=cached?r4_setup_polygon(projected,3,vp,&frame,flat,out+n,cap-n,&st):
                    r4_setup_view(tri,3,projection,R4_NEAR_Z,R4_FAR_Z,vp,&frame,flat,out+n,cap-n,&st);
                if(emitted<0)emit_failed=1;else n+=(uint32_t)emitted;
            }
            rast_view_vertex eye[3];
            vertex(eye,w->enemy.x-R4_ONE/13,w->enemy.y+R4_ONE/16,z+14000,0,0,0xffffe2a0);
            vertex(eye+1,w->enemy.x+R4_ONE/13,w->enemy.y+R4_ONE/16,z+14000,0,0,0xffffe2a0);
            vertex(eye+2,w->enemy.x,w->enemy.y-R4_ONE/16,z+14000,0,0,0xffffe2a0);
            n+=polygon(eye,3,vp,flat,projection,out+n,cap-n,&st);
        }
    }
    /* Shared projection, smooth fuselage, hard metal wings and glass cockpit. */
    if(w->shield) {
        rast_view_vertex v[R4_SHIP_VERTICES];rast_vertex screen[R4_SHIP_VERTICES];
        uint32_t colors[3][R4_SHIP_VERTICES];
        int32_t roll=w->bank;
        int32_t cosine=FX16_ONE-fx16_mul(roll,roll)/2;
        int16_t light[3]={(int16_t)((fx16_mul(-32000,cosine)+fx16_mul(44000,roll))/4),
            (int16_t)((fx16_mul(44000,cosine)-fx16_mul(-32000,roll))/4),-8500};
        int16_t half[3]={-4000,6500,-14500};
        const uint32_t material[3]={0x00b9d7e5,0x00dae2df,0x00367fc5};
        for(unsigned i=0;i<R4_SHIP_VERTICES;++i) {
            const int16_t *local=r4_ship_vertices[i];
            int32_t x=fx16_mul(local[0],cosine)-fx16_mul(local[1],roll);
            int32_t y=fx16_mul(local[0],roll)+fx16_mul(local[1],cosine);
            int32_t z=local[2]+w->pitch*local[1]/5+w->strafe*local[0]/6;
            for(unsigned m=0;m<3;++m)colors[m][i]=r4_light_color(r4_ship_normals[i],light,half,material[m]);
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
            int emitted=cached?r4_setup_polygon(projected,3,vp,&frame,flat,out+n,cap-n,&st):
                r4_setup_view(tri,3,projection,R4_NEAR_Z,R4_FAR_Z,vp,&frame,flat,out+n,cap-n,&st);
            if(emitted<0)emit_failed=1;else n+=(uint32_t)emitted;
            if(emitted==0 && r4_ship_material[i]==1) {
                if(cached) {
                    rast_vertex tmp=projected[1];projected[1]=projected[2];projected[2]=tmp;
                    emitted=r4_setup_polygon(projected,3,vp,&frame,flat,out+n,cap-n,&st);
                    if(emitted<0)emit_failed=1;else n+=(uint32_t)emitted;
                } else {
                    rast_view_vertex tmp=tri[1];tri[1]=tri[2];tri[2]=tmp;
                    n+=polygon(tri,3,vp,flat,projection,out+n,cap-n,&st);
                }
            }
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
        n+=facing(b->x,b->y,(int32_t)w->d+b->z,R4_ONE/40,R4_ONE/9,
            0xffa0ffe0,0,0,flat,vp,projection,out+n,cap-n,&st);
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
