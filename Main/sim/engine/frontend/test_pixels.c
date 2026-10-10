/* F2/F3 独立像素检查：非游戏矩形；直通解释器与 prepass/BAND 对拍。 */
#include "gfx/gfx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint8_t memory[0x800000];
static uint8_t table[GFX_STRIP_TABLE_BYTES] __attribute__((aligned(32)));
static gfx_cmd direct[128],pre[128];
static uint32_t scratch[32768],refs[30720];
static gfx_strip_plan_item items[4096] __attribute__((aligned(16)));
static gfx_geometry_cache cache;
static uint32_t rng=0x471f2;
static uint32_t rnd(void){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng;}
static void need(int ok,const char *s,uint32_t n){if(!ok){printf("FAIL %s scene=%u\n",s,n);exit(1);}}
int main(void)
{
    uint32_t n,i,y;gfx_surface_table t;gfx_mem m={0,memory,sizeof memory,0,0};
    for(i=0;i<4096u;i++)((uint32_t *)(void *)(memory+0x600000))[i]=0x80201008u+(i%7u);
    for(n=0;n<10000u;n++) {
        uint32_t log2=3u+n%3u,height=(1u+n%3u)<<log2;
        gfx_surface ref={0x1000,640,(uint16_t)height,12,0,0},dst={0x200000,640,(uint16_t)height,12,0,0};
        gfx_surface bg={0x400000,640,(uint16_t)height,12,0,0},src={0x600000,64,64,8,0,1};
        gfx_strip_config cfg={height>>log2,log2,2,1};gfx_list a,b,c;
        gfx_strip_workspace w={.items=items,.item_cap=4096,.refs=refs,.ref_cap=30720};
        exec_sw_state es;exec_sw_error e;gfx_surface_reset(&t);
        gfx_surface_set(&t,0,&ref);gfx_surface_set(&t,1,&dst);gfx_surface_set(&t,2,&bg);gfx_surface_set(&t,3,&src);
        src.format=2;gfx_surface_set(&t,4,&src);
        memset(memory+ref.base,0xa5,height*4096u);memset(memory+dst.base,0xa5,height*4096u);
        gfx_list_begin(&a,direct,128,&t);gfx_list_begin(&b,pre,128,&t);
        need(!gfx_list_begin_strips_planned(&c,table,sizeof table,&t,&cfg,0,&w),"begin",n);
        if(n&1u)gfx_list_geometry(&c,&cache);
        gfx_fill(&a,0,(gfx_rect){0,0,639,(int32_t)height-1},0x00112233,0,0);
#ifndef INJECT_PREPASS_INITIALIZATION
        gfx_fill(&b,2,(gfx_rect){0,0,639,(int32_t)height-1},0x00112233,0,0);
#endif
        for(i=0;i<8;i++) {
            int x=(int)(rnd()%640u),yy=(int)(rnd()%height);
            gfx_fill(&a,0,(gfx_rect){x,yy,x+7,yy+1},0x80201008,1,0);
            gfx_fill(&b,2,(gfx_rect){x,yy,x+7,yy+1},0x80201008,1,0);
        }
        for(i=0;i<24;i++) {
            int x=(int)(rnd()%680u)-20,yy=(int)(rnd()%(height+40u))-20;
            int sx=(int)(rnd()%80u)-8,sy=(int)(rnd()%80u)-8,eq=(int)(i%6u);
            int ww=(int[]){1,7,16,20,6,640}[i%6u],hh=(int[]){1,31,16,20,24,480}[i%6u];
            if(i==12u){gfx_rect clip={10,1,600,(int32_t)height-2};gfx_set_clip(&a,clip);gfx_set_clip(&c,clip);}
            need(!gfx_blit(&a,eq==5?4:3,sx,sy,0,x,yy,ww,hh,eq,eq==3?128:0,i&1u,0x80201008)&&
                 !gfx_blit(&c,eq==5?4:3,sx,sy,1,x,yy,ww,hh,eq,eq==3?128:0,i&1u,0x80201008),"append",n);
        }
        /* 有重叠的后置 overlay 必须仍在动态 pass 末尾。 */
        gfx_fill(&a,0,(gfx_rect){5,0,50,7},0x80201008,3,96);
        gfx_fill(&c,1,(gfx_rect){5,0,50,7},0x80201008,3,96);
        gfx_list_end(&a);gfx_list_end(&b);gfx_list_end(&c);
        need(!a.first_error&&!b.first_error&&!c.first_error&&w.sealed,"valid sealed inputs",n);
        exec_sw_reset(&es);need(!exec_sw_run(&es,&t,&m,direct,a.used,a.used,0,&e),"direct",n);
        exec_sw_reset(&es);need(!exec_sw_run(&es,&t,&m,pre,b.used,b.used,0,&e),"prepass",n);
        need(!exec_sw_run_strips(&t,&m,table,sizeof table,&cfg,scratch,sizeof scratch,0,&e),"BAND",n);
        for(y=0;y<height;y++)need(!memcmp(memory+ref.base+y*4096u,memory+dst.base+y*4096u,4096),"pixels and padding",n);
    }
    puts("PASS: 10000 non-game prepass/planned pixel scenes, six equations, source/clip edges, overlay, padding");return 0;
}
