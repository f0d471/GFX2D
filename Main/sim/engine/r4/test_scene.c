#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "board.h"
#include "core/crc32.h"
#include "r4.h"
#include "r4_scene.h"
#include "r4_cup.h"
#include "r4_atlas.h"
#include "gfx/rast_ref.h"

static void witness(const r4_world *w,uint32_t *pixel,uint16_t *z)
{
    static rast_record records[512];
    static uint32_t color[640*16];static uint16_t depth[640*16];
    fx16_mat4 p;const rast_viewport vp={0,0,400,480};
    assert(!fx16_perspective(&p,R4_FOV_BAM,R4_ASPECT,R4_NEAR_Z,R4_FAR_Z));
    unsigned n=r4_world_emit(w,&p,&vp,2,records,512,0);assert(n && n<128);
    memset(color,0,sizeof color);memset(depth,255,sizeof depth);
    rast_target target={color,depth,640,15,r4_atlas_words,64,64,64,0};
    assert(!rast_ref_run(records,n,&target));
    pixel[0]=color[231];pixel[1]=color[169];pixel[2]=color[222];
    z[0]=depth[231];z[1]=depth[169];z[2]=depth[222];
}

int main(void)
{
    rast_record records[512],sentinel;
    r4_world a,b;
    fx16_mat4 projection;
    const rast_viewport vp={0,0,400,480};
    uint32_t fail=0,peak=0;crc32_ctx crc;crc32_begin(&crc);
    assert(!fx16_perspective(&projection,R4_FOV_BAM,R4_ASPECT,R4_NEAR_Z,R4_FAR_Z));
    r4_world_init(&a,R4_SEED);r4_world_init(&b,R4_SEED);
    for(unsigned i=0;i<2000;++i) {
        uint32_t keys=(i%120<30?BOARD_KEY_LEFT:i%120<60?BOARD_KEY_RIGHT:0u)|
                      (i%200<30?BOARD_KEY_UP:i%200<60?BOARD_KEY_DOWN:0u);
        r4_world_tick(&a,keys,&fail);r4_world_tick(&b,keys,&fail);
        assert(!memcmp(&a,&b,sizeof a));
        if(i%20)continue;
        for(unsigned ladder=1;ladder<=2;++ladder) {
            unsigned n=r4_world_emit(&a,&projection,&vp,ladder,records,512,0);
            assert(n>0 && n<128);
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
    for(unsigned tick=0;tick<768;tick+=64) {
        int n=r4_cup_emit(tick,records,512);
        assert(n>100 && n<=512);
        for(int i=0;i<n;++i) {
            assert(rast_record_valid(records+i));
            assert(rast_get16(records+i,72)>=160 && rast_get16(records+i,74)<480);
            assert(rast_get16(records+i,76)>=120 && rast_get16(records+i,78)<360);
        }
        crc32_update(&crc,records,(unsigned)n*sizeof records[0]);
    }
    assert(!fail);
    /* Two explicit world occlusion instants: an enemy and a projectile behind
       opposite gate posts. Removing the gate reveals each sprite at greater z. */
    {
        r4_world w;uint32_t gated[3],open[3],absent[3];uint16_t gz[3],oz[3],az[3];
        r4_world_init(&w,R4_SEED);w.gate[0].dist=8*R4_ONE;w.gate[0].x=0;
        w.enemy=(r4_bullet){25*R4_ONE/16,0,-10*R4_ONE,1};
        w.ebul[0]=(r4_bullet){-25*R4_ONE/16,0,-10*R4_ONE,1};
        witness(&w,gated,gz);w.gate[0].dist=0;witness(&w,open,oz);
        assert(gated[0]!=open[0] && gated[1]!=open[1]);
        assert(gz[0]<oz[0] && gz[1]<oz[1]);
        w.enemy.alive=0;witness(&w,absent,az);
        assert(open[2]==absent[2] && oz[2]==az[2]);
        puts("R4 OCCLUSION PASS enemy/projectile behind gate posts");
    }
    printf("R4 SCENE PASS ticks=2000 peak=%u cup_input=396 crc=%08x\n",peak,crc32_end(&crc));
    return 0;
}
