/* G3 构造拒绝、表容量和软件错误三元组。与 RTL 定向输入使用相同预期。 */
#include "gfx/gfx.h"
#include <stdio.h>
#include <string.h>
#define BYTES 0x280000u
#define FB 0x1000u
static uint8_t memory[BYTES];
static uint8_t table[GFX_STRIP_TABLE_BYTES] __attribute__((aligned(32768)));
static uint32_t scratch[16u*1024u];
static gfx_surface_table t;
static gfx_mem m={0,memory,BYTES,0,0};
static gfx_strip_config cfg={2,4,0,0};
static uint32_t checks,errors;
static void expect(const char *name,int actual,int wanted)
{
    checks++;if(actual!=wanted){errors++;printf("FAIL %s got=%d exp=%d\n",name,actual,wanted);}
}
static void raw(uint32_t s,uint32_t op,uint32_t xy,uint32_t size,uint32_t wanted)
{
    gfx_cmd *c=(gfx_cmd *)(void *)(table+(s+1u)*32768u);
    exec_sw_error e;
    memset(table,0,sizeof table);((uint32_t *)(void *)table)[s]=1;
    c->w[0]=op;c->w[2]=xy;c->w[3]=size;c->w[7]=0xcafe005au;
    expect("raw-code",exec_sw_run_strips(&t,&m,table,sizeof table,&cfg,scratch,sizeof scratch,0,&e),(int)wanted);
    expect("raw-index",(int)e.index,(int)(s*1024u));
    expect("raw-tag",(int)e.tag,(int)0xcafe005au);
}
int main(void)
{
    gfx_surface d={FB,640,32,12,0,0},s={0x240000u,64,64,8,0,0};
    gfx_list l,flat;gfx_cmd f[16];gfx_rect cross={0,14,1,17};
    exec_sw_error e;uint32_t i;
    gfx_surface_reset(&t);gfx_surface_set(&t,0,&d);gfx_surface_set(&t,1,&s);
    raw(1,3,0,0,2);raw(1,255,0,0,2);raw(1,1,0xffff0000u,0x10001u,2);
    raw(1,1,15u<<16,0x20001u,2);raw(1,0x100001u,0,0x10001u,2);
    raw(1,0x40000001u,0,0,2);
    gfx_surface_set(&t,2,&d);raw(1,0x20002u,0,0x10001u,7);
    memset(table,0,sizeof table);((uint32_t *)(void *)table)[1]=1025;
    expect("capacity-code",exec_sw_run_strips(&t,&m,table,sizeof table,&cfg,scratch,sizeof scratch,0,&e),2);
    expect("capacity-index",(int)e.index,1024);expect("capacity-tag",(int)e.tag,0);
    memset(table,0,sizeof table);((uint32_t *)(void *)table)[1]=1;
    expect("fetch-code",exec_sw_run_strips(&t,&m,table,65536u,&cfg,scratch,sizeof scratch,0,&e),9);
    expect("fetch-index",(int)e.index,1024);expect("fetch-tag",(int)e.tag,0);
    expect("begin",gfx_list_begin_strips(&l,table,sizeof table,&t,&cfg,0),0);
    gfx_list_begin(&flat,f,16,&t);
    expect("empty-bad-fill",gfx_fill(&l,0,(gfx_rect){0,0,-1,-1},0,0,64),
                              gfx_fill(&flat,0,(gfx_rect){0,0,-1,-1},0,0,64));
    expect("empty-bad-surface",gfx_fill(&l,3,(gfx_rect){0,0,-1,-1},0,0,0),
                                 gfx_fill(&flat,3,(gfx_rect){0,0,-1,-1},0,0,0));
    expect("bad-equation",gfx_blit(&l,1,0,0,0,0,0,1,1,15,0,0,0),
                             gfx_blit(&flat,1,0,0,0,0,0,1,1,15,0,0,0));
    expect("feedback",gfx_blit(&l,2,0,0,0,32,16,1,1,0,0,0,0),7);
    expect("clip-api",gfx_set_clip(&l,(gfx_rect){0,0,399,31}),0);
    expect("clip-no-record",(int)l.used,0);
    expect("empty-valid",gfx_fill(&l,0,(gfx_rect){0,0,-1,-1},0,0,0),0);
    expect("begin-again",gfx_list_begin_strips(&l,table,sizeof table,&t,&cfg,0),0);
    ((uint32_t *)(void *)table)[1]=1024;
    expect("atomic-overflow",gfx_fill(&l,0,cross,0,0,0),GFX_BUILD_FULL);
    expect("first-bucket-unchanged",(int)((uint32_t *)(void *)table)[0],0);
    expect("sticky-overflow",l.first_error,GFX_BUILD_FULL);
    expect("begin-boundary",gfx_list_begin_strips(&l,table,sizeof table,&t,&cfg,0),0);
    for(i=0;i<1024;i++)if(gfx_nop(&l))errors++;
    expect("exact-cap",(int)((uint32_t *)(void *)table)[0],1024);
    expect("next-region",(int)((uint32_t *)(void *)table)[1],0);
    expect("1025-reject",gfx_nop(&l),GFX_BUILD_FULL);
    expect("end",gfx_list_end(&l),0);expect("closed",gfx_nop(&l),GFX_BUILD_CLOSED);
    cfg.strip_log2=2;expect("bad-height",gfx_strip_config_check(&t,&cfg),2);cfg.strip_log2=4;
    t.s[2].base=FB+16u;cfg.bg_id=2;expect("partial-background-alias",gfx_strip_config_check(&t,&cfg),4);
    cfg.bg_id=0;gfx_surface_set_raw(&t,1,0x240000u,0xffff0040u,0x80000008u);
    expect("wide-source-begin",gfx_list_begin_strips(&l,table,sizeof table,&t,&cfg,0),0);
    expect("source-coordinate-lossless",gfx_blit(&l,1,0,32760,0,0,0,16,32,0,0,0,0),GFX_BUILD_BAD_ARG);
    expect("lossless-atomic",(int)l.used,0);
    printf("G3 ERROR checks=%u errors=%u\nRESULT: %s\n",checks,errors,errors?"FAIL":"PASS");
    return errors?1:0;
}
