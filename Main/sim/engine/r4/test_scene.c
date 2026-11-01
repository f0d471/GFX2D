#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "board.h"
#include "core/crc32.h"
#include "r4.h"
#include "r4_scene.h"
#include "r4_atlas.h"
#include "r4_geometry.h"
#include "gfx/rast_ref.h"

static void witness(const r4_world *w,uint32_t *pixel,uint16_t *z)
{
    static rast_record records[512];
    static uint32_t color[640*16];static uint16_t depth[640*16];
    fx16_mat4 p;const rast_viewport vp={0,0,400,480};
    assert(!fx16_perspective(&p,R4_FOV_BAM,R4_ASPECT,R4_NEAR_Z,R4_FAR_Z));
    unsigned n=r4_world_emit(w,&p,&vp,2,records,512,0);assert(n && n<256);
    memset(color,0,sizeof color);memset(depth,255,sizeof depth);
    rast_target target={color,depth,640,15,r4_atlas_words,64,64,64,0};
    assert(!rast_ref_run(records,n,&target));
    int32_t ndc=fx16_div(fx16_mul(p.v[0],R4_GATE_OPEN),8*R4_ONE);
    unsigned x=(unsigned)(fx16_viewport(ndc,400,0)/16);
    unsigned at[3]={x,400u-x,(x+14u)+14u*640u};
    for(unsigned i=0;i<3;++i){pixel[i]=color[at[i]];z[i]=depth[at[i]];}
}

int main(void)
{
    rast_record records[512],sentinel;
    r4_world a,b;
    fx16_mat4 projection;
    const rast_viewport vp={0,0,400,480};
    uint32_t fail=0,peak=0;crc32_ctx crc;crc32_begin(&crc);
    assert(!fx16_perspective(&projection,R4_FOV_BAM,R4_ASPECT,R4_NEAR_Z,R4_FAR_Z));
    /* A separate generic R1 path is the oracle for application-only batch
       projection. Exercise varying z/UV/colors and a non-sparse matrix. */
    {
        uint32_t random=0x319bed21u;
        for(unsigned i=0;i<50000;++i) {
            rast_view_vertex input[3];rast_vertex screen[3];
            rast_record generic[12],fast[12];fx16_mat4 p=projection;
            if(i&1u){p.v[1]=1324;p.v[8]=1057;p.v[12]=-321;}
            if((i&3u)==2u) {
                p.v[0]=FX16_ONE;p.v[5]=FX16_ONE;p.v[6]=0;
                p.v[14]=0;p.v[15]=(int32_t)(8192u+random%2000000u);
            }
            for(unsigned j=0;j<3;++j) {
                random^=random<<13;random^=random>>17;random^=random<<5;
                input[j]=(rast_view_vertex){(int32_t)(random%400001u)-200000,
                    (int32_t)(random%240001u)-120000,-(int32_t)(R4_ONE+random%(30u*R4_ONE)),
                    (uint16_t)(random%1009u),(uint16_t)((random>>9)%1009u),random|0xff000000u};
                if((i&3u)==2u){input[j].x=(int32_t)random;input[j].y=(int32_t)(random^0x793a519bu);}
            }
            int a=rast_setup_view_vp(input,3,&p,R4_NEAR_Z,R4_FAR_Z,&vp,7,generic,12,0);
            if(!r4_project_inside(input,3,&p,R4_NEAR_Z,R4_FAR_Z,&vp,screen)) {
                int b=rast_setup_polygon_vp(screen,3,&vp,7,fast,12,0);
                assert(a==b);
                if(a>0)assert(!memcmp(generic,fast,(unsigned)a*sizeof generic[0]));
            }
        }
        puts("R4 PROJECTION PASS 50000 generic/optimized triangles");
    }
    r4_world_init(&a,R4_SEED);r4_world_init(&b,R4_SEED);
    for(unsigned i=0;i<2000;++i) {
        uint32_t keys=(i%120<30?BOARD_KEY_LEFT:i%120<60?BOARD_KEY_RIGHT:0u)|
                      (i%200<30?BOARD_KEY_UP:i%200<60?BOARD_KEY_DOWN:0u);
        r4_world_tick(&a,keys,&fail);r4_world_tick(&b,keys,&fail);
        assert(!memcmp(&a,&b,sizeof a));
        if(i%20)continue;
        for(unsigned ladder=1;ladder<=2;++ladder) {
            unsigned n=r4_world_emit(&a,&projection,&vp,ladder,records,512,0);
            assert(n>0 && n<=R4_RECORD_BUDGET);
            if(n>peak)peak=n;
            for(unsigned k=0;k<n;++k) {
                assert(rast_record_valid(records+k));
                assert(rast_get16(records+k,74)<400);
                assert(rast_get16(records+k,78)<480);
                assert(ladder!=1 || !(rast_get32(records+k,0)&RAST_TEX_EN));
            }
            crc32_update(&crc,records,n*sizeof records[0]);
        }
    }
    memset(&sentinel,0xa5,sizeof sentinel);records[0]=sentinel;
    assert(r4_world_emit(&a,&projection,&vp,2,records,1,0)==UINT32_MAX);
    assert(!memcmp(records,&sentinel,sizeof sentinel));
    /* Repeated wall phases actually hit the cache; compare this stream against
       a separately compiled uncached reference in check_r4.py. */
    r4_world_init(&a,R4_SEED);
    for(unsigned i=0;i<120;++i) {
        r4_world_tick(&a,0,&fail);
        unsigned n=r4_world_emit(&a,&projection,&vp,2,records,512,0);
        assert(n && n<=R4_RECORD_BUDGET);crc32_update(&crc,records,n*sizeof records[0]);
    }
    assert(!fail);
    /* Gameplay uses the ship's world depth, and projectiles really hit enemies. */
    {
        r4_world w;r4_world_init(&w,R4_SEED);w.manual=1;
        for(unsigned i=0;i<R4_GATE_SLOTS;++i)w.gate[i].dist=0;
        w.enemy.alive=0;w.pcooldown=99;
        w.ebul[0]=(r4_bullet){0,w.ship_y,-R4_SHIP_Z-4*R4_ONE/30-5*R4_ONE/60,1};
        r4_world_tick(&w,0,&fail);
        assert(w.shield==R4_SHIELD_INIT-1 && !w.ebul[0].alive);
        r4_world_init(&w,R4_SEED);w.manual=1;
        w.cam_x=R4_ONE;
        w.gate[0].dist=R4_SHIP_Z+R4_ONE/30;w.gate[0].x=r4_gate_x_of(0,0);
        r4_world_tick(&w,0,&fail);
        /* Gates were removed with their collision (2026-11-01): no invisible wall remains. */
        assert(!w.gate[0].passed && w.shield==R4_SHIELD_INIT);
        r4_world_init(&w,R4_SEED);w.manual=1;w.enemy_hp=1;
        w.pbul[0]=(r4_bullet){fx_sin(370)*2,R4_ONE/5+fx_sin(240)/2,
            -10*R4_ONE+R4_ONE/4,1};
        r4_world_tick(&w,0,&fail);
        assert(!w.enemy.alive && !w.pbul[0].alive && w.score==100 && w.blast_ticks);
        r4_world_init(&w,R4_SEED);w.manual=1;
        for(unsigned i=0;i<R4_GATE_SLOTS;++i)w.gate[i].dist=0;
        w.ebul[0]=(r4_bullet){0,0,-8*R4_ONE,1};
        r4_world_tick(&w,BOARD_KEY_BOMB,&fail);assert(w.bombs==1 && !w.ebul[0].alive);
        r4_world_tick(&w,BOARD_KEY_BOMB,&fail);assert(w.bombs==1);
        r4_world_tick(&w,0,&fail);r4_world_tick(&w,BOARD_KEY_BOMB,&fail);assert(!w.bombs);
        r4_world_init(&w,R4_SEED);w.ladder=1;
        for(unsigned i=0;i<R4_GATE_SLOTS;++i)w.gate[i].dist=0;
        for(unsigned i=0;i<120;++i)r4_world_tick(&w,0,&fail);
        assert(!w.enemy.alive);
        for(unsigned i=0;i<R4_EBULLET_MAX;++i)assert(!w.ebul[i].alive);
        puts("R4 GAMEPLAY PASS ship-plane hit, no gate wall, enemy hit, bomb edge");
    }
    /* Gates are no longer drawn (2026-11-01): gate data must not change the
       rendered world, so the enemy and projectile look the same with or without it. */
    {
        r4_world w;uint32_t gated[3],open[3];uint16_t gz[3],oz[3];
        r4_world_init(&w,R4_SEED);w.gate[0].dist=8*R4_ONE;w.gate[0].x=0;
        for(unsigned i=1;i<R4_GATE_SLOTS;++i)w.gate[i].dist=0;
        w.enemy=(r4_bullet){R4_GATE_OPEN*10/8,0,-10*R4_ONE,1};
        w.ebul[0]=(r4_bullet){-R4_GATE_OPEN*10/8,0,-10*R4_ONE,1};
        witness(&w,gated,gz);w.gate[0].dist=0;witness(&w,open,oz);
        assert(!memcmp(gated,open,sizeof gated) && !memcmp(gz,oz,sizeof gz));
        puts("R4 GATES REMOVED PASS gate data emits no geometry");
    }
    printf("R4 SCENE PASS ticks=2000 peak=%u crc=%08x\n",peak,crc32_end(&crc));
    return 0;
}
