/* 原生产即时 v2 为字节/错误参考；像素基准另用直通软件执行器。 */
#include "gfx/gfx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned char tables[4][GFX_STRIP_TABLE_BYTES] __attribute__((aligned(32)));
static gfx_strip_plan_item items[2][4096] __attribute__((aligned(16)));
static uint32_t refs[2][30720];
static gfx_strip_workspace ws[2];static gfx_geometry_cache cache[2];
static uint32_t rng=0x4711010u;
static uint32_t rnd(void){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng;}
static void need(int ok,const char *why,uint32_t scene)
{if(!ok){printf("FAIL %s scene=%u\n",why,scene);exit(1);}}
static int same(const gfx_list *a,const gfx_list *b)
{return a->used==b->used&&a->first_error==b->first_error&&a->rejected==b->rejected&&a->overflow==b->overflow&&a->ended==b->ended;}
int main(void)
{
    gfx_surface_table tab;gfx_list l[4];uint32_t scene,m,i,sealed=0;
    for(scene=0;scene<11000u;scene++) {
        uint32_t log2=3u+scene%3u,height=log2==3u?240u:480u;
        gfx_strip_config cfg={height>>log2,log2,0,0};
        gfx_surface d={0x200000u,640,(uint16_t)height,12,0,0};
        gfx_surface s={0x100000u,64,64,(uint8_t)(8u+scene%2u),0,1};
        gfx_surface_reset(&tab);need(!gfx_surface_set(&tab,0,&d)&&!gfx_surface_set(&tab,1,&s)&&!gfx_surface_set(&tab,2,&d),"surfaces",scene);
        s.format=2;need(!gfx_surface_set(&tab,3,&s),"straight source",scene);
        for(m=0;m<4;m++) {
            memset(tables[m],0xa5,sizeof tables[m]);
            if(m&1u){uint32_t w=m>>1;ws[w]=(gfx_strip_workspace){.items=items[w],.item_cap=4096,.refs=refs[w],.ref_cap=30720};
                need(!gfx_list_begin_strips_planned(&l[m],tables[m],sizeof tables[m],&tab,&cfg,0,&ws[w]),"planned begin",scene);}
            else need(!gfx_list_begin_strips(&l[m],tables[m],sizeof tables[m],&tab,&cfg,0),"begin",scene);
            if(m>=2u)need(!gfx_list_geometry(&l[m],&cache[m-2]),"cache",scene);
        }
        for(i=0;i<48u;i++) {
            int result[4],op=(int)(rnd()%9u),x=(int)(rnd()%720u)-40,y=(int)(rnd()%560u)-40;
            int sx=(int)(rnd()%84u)-10,sy=(int)(rnd()%84u)-10;
            int w=(int[]){1,7,16,20,6,640}[rnd()%6u],h=(int[]){1,31,16,20,24,480}[rnd()%6u];
            int eq=(int)(i%6u),src=eq==5?3:1;
            if(scene>=10000u&&i%17u==0u)src=2;
            if(scene>=10000u&&scene%23u==0u&&i==8u)x=40000;
            if(scene%19u==0u&&i==9u)w=0;
            for(m=0;m<4u;m++) {
                if(op==0)result[m]=gfx_set_clip(&l[m],(gfx_rect){x,y,x+110,y+80});
                else if(op==1)result[m]=gfx_nop(&l[m]);
                else if(op==2)result[m]=gfx_fill(&l[m],0,(gfx_rect){x,y,x+w-1,y+h-1},0x80201008u,eq<4?eq:0,eq==3?128:0);
                else if(scene>=10000u&&op==3&&i>30)result[m]=gfx_list_end(&l[m]);
                else if(op==4) {
                    gfx_blit_tmpl tm={src,0,w,h,eq,eq==3?128:0,i&1u,0x80201008};
                    gfx_blit_pos pos={x,y,sx,sy};
                    result[m]=(int)gfx_blit_run(&l[m],&tm,&pos,1);
                } else result[m]=gfx_blit(&l[m],src,sx,sy,0,x,y,w,h,eq,eq==3?128:0,i&1u,0x80201008u);
                need(result[m]==result[0]&&same(&l[0],&l[m]),"return/accounting",scene);
                if((m&1u)&&!l[m].ended)
                    need(!memcmp(tables[m],(uint32_t[32]){0},128u),"seal before publication",scene);
            }
        }
        for(m=0;m<4;m++)if(!l[m].ended)need(gfx_list_end(&l[m])==0,"end",scene);
        if(scene<10000u)need(!l[0].first_error&&ws[0].sealed,"10000 valid seals",scene);
        need(same(&l[0],&l[1])&&same(&l[0],&l[2])&&same(&l[0],&l[3]),"final accounting",scene);
        need(!memcmp(tables[0],tables[2],sizeof tables[0]),"template bytes",scene);
        if(ws[0].sealed){sealed++;for(m=1;m<4;m++)need(!memcmp(tables[0],tables[m],sizeof tables[0]),"sealed bytes",scene);}
        else need(!ws[1].sealed && !memcmp(tables[1],(uint32_t[32]){0},128u),"error unpublished",scene);
    }
    {
        gfx_surface d={0x200000,640,480,12,0,0},s={0x100000,64,64,8,0,1};
        gfx_strip_config cfg={30,4,0,0};uint32_t misses;
        gfx_surface_reset(&tab);gfx_surface_set(&tab,0,&d);gfx_surface_set(&tab,1,&s);
        gfx_surface_set(&tab,2,&d);
        gfx_list_begin_strips(&l[0],tables[0],sizeof tables[0],&tab,&cfg,0);gfx_list_geometry(&l[0],&cache[0]);
        need(gfx_blit(&l[0],2,0,0,0,80,80,16,16,0,0,0,0)==GFX_E_OVERLAP,"feedback field check",0);
        gfx_blit(&l[0],1,0,0,0,30,32,16,16,1,0,0,0);
        gfx_blit(&l[0],1,0,0,0,30,32,16,16,1,0,0,0);
        need(cache[0].hits==1,"geometry hit",0);misses=cache[0].misses;
        gfx_blit(&l[0],1,16,0,0,30,32,16,16,1,0,0,0);
        need(cache[0].misses==misses+1,"source key invalidation",0);
        gfx_blit(&l[0],1,16,0,0,30,32,16,16,1,0,0,0);misses=cache[0].misses;
        gfx_set_clip(&l[0],(gfx_rect){1,1,638,478});
        gfx_blit(&l[0],1,16,0,0,30,32,16,16,1,0,0,0);
        need(cache[0].misses==misses+1,"clip epoch invalidation",0);
        gfx_blit(&l[0],1,16,0,0,30,32,16,16,1,0,0,0);misses=cache[0].misses;
        gfx_list_geometry_generation(&l[0],2);
        gfx_blit(&l[0],1,16,0,0,30,32,16,16,1,0,0,0);
        need(cache[0].misses==misses+1,"descriptor generation invalidation",0);
    }
    /* 相同相位跨不同桶重复，必须实际命中并覆盖首/中/尾片段模式。 */
    {
        uint32_t log2,phase,hi,eq;
        static const int heights[]={1,7,16,20,24,31,64};
        for(log2=3;log2<=5;log2++)for(phase=0;phase<(1u<<log2);phase++)
        for(hi=0;hi<7;hi++)for(eq=0;eq<6;eq++) {
            uint32_t height=log2==3?240:480,j;
            gfx_surface d={0x200000,640,(uint16_t)height,12,0,0},s={0x100000,64,64,8,0,1};
            gfx_strip_config cfg={height>>log2,log2,0,0};
            gfx_surface_reset(&tab);gfx_surface_set(&tab,0,&d);gfx_surface_set(&tab,1,&s);s.format=2;gfx_surface_set(&tab,2,&s);
            memset(tables[0],0xa5,sizeof tables[0]);memset(tables[2],0xa5,sizeof tables[2]);
            gfx_list_begin_strips(&l[0],tables[0],sizeof tables[0],&tab,&cfg,0);
            gfx_list_begin_strips(&l[2],tables[2],sizeof tables[2],&tab,&cfg,0);gfx_list_geometry(&l[2],&cache[0]);
            for(j=0;j<10;j++)for(m=0;m<2;m++) {
                uint32_t z=m?2:0;
                need(!gfx_blit(&l[z],eq==5?2:1,5,0,0,10+(int)j,((int)j%3)*(1<<log2)+(int)phase,
                               20,heights[hi],(int)eq,eq==3?128:0,1,0),"pattern append",phase);
            }
            gfx_list_end(&l[0]);gfx_list_end(&l[2]);
            need(cache[0].hits==9 && !memcmp(tables[0],tables[2],sizeof tables[0]),"leading interior trailing pattern",phase);
        }
    }
    /* 溢出发生在中间桶；本次调用不能改前后桶，也不能消耗计划项。 */
    {
        gfx_strip_config cfg={30,4,0,0};gfx_surface d={0x200000u,640,480,12,0,0};
        gfx_surface_reset(&tab);gfx_surface_set(&tab,0,&d);
        ws[0]=(gfx_strip_workspace){.items=items[0],.item_cap=4096,.refs=refs[0],.ref_cap=30720};
        gfx_list_begin_strips(&l[0],tables[0],sizeof tables[0],&tab,&cfg,0);
        gfx_list_begin_strips_planned(&l[1],tables[1],sizeof tables[1],&tab,&cfg,0,&ws[0]);
        for(i=0;i<1024;i++)for(m=0;m<2;m++)need(!gfx_fill(&l[m],0,(gfx_rect){0,16,1,16},0,0,0),"fill capacity",i);
        for(m=0;m<2;m++){need(gfx_fill(&l[m],0,(gfx_rect){0,0,1,47},0,0,0)==GFX_BUILD_FULL,"atomic full",0);
            need(!gfx_fill(&l[m],0,(gfx_rect){0,48,1,48},0,0,0),"after reject",0);}
        need(same(&l[0],&l[1])&&ws[0].count[0]==0&&ws[0].count[2]==0&&ws[0].items_used==1025,"capacity atomic",0);
    }
    printf("PASS: 11000 planned/template scenes; valid seals=%u, full bytes, errors, clips, source OOB, alias, atomic overflow\n",sealed);
    return 0;
}
