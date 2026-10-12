/* 条带控制正式实现的同一调用序列：直通金标准、生产 v2 表、RTL 初始内存。 */
#include "gfx/gfx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MEM_BYTES 0x400000u
#define FB 0x1000u
#define SRC 0x240000u
#define TABLE 0x300000u
static uint8_t memory[MEM_BYTES] __attribute__((aligned(16)));
static uint8_t initial[MEM_BYTES], golden[MEM_BYTES];
static uint8_t table[GFX_STRIP_TABLE_BYTES] __attribute__((aligned(32768)));
static uint32_t scratch[32768];
static int compose(gfx_list *l)
{
    uint32_t i;
    if(gfx_fill(l,0,(gfx_rect){0,0,639,479},0x00102030u,GFX_EQ_COPY,0u))return 1;
    if(gfx_set_clip(l,(gfx_rect){0,0,399,479}))return 1;
    for(i=0;i<40u;i++) if(gfx_blit(l,1,(int)(i%3u),0,0,(int)(i%10u)*9-3,
        (int)(i/10u)*13+11,28,28,(int)(i%6u),i%6u==3u?128u:0u,1,0x40201008u))return 1;
    if(gfx_set_clip(l,(gfx_rect){0,0,639,479}))return 1;
    for(i=0;i<20u;i++)if(gfx_fill(l,0,(gfx_rect){401+(int)i,0,401+(int)i,479},
        0x80203040u,(int)(i%4u),i%4u==3u?96u:0u))return 1;
    return gfx_list_end(l);
}
static void dump(const char *prefix,const char *suffix,const uint8_t *m)
{
    char name[512];uint32_t i;FILE *f;
    snprintf(name,sizeof name,"%s.%s",prefix,suffix);f=fopen(name,"w");
    if(!f)exit(2);
    for(i=0;i<MEM_BYTES;i+=16u) {
        int j;for(j=15;j>=0;j--)fprintf(f,"%02x",m[i+(uint32_t)j]);fputc('\n',f);
    }
    fclose(f);
}
int main(int argc,char **argv)
{
    gfx_surface_table t;
    gfx_surface d={FB,640,480,12,0,0},s={SRC,64,64,8,0,0};
    gfx_strip_config cfg={30,4,0,0};
    gfx_mem m={0,memory,MEM_BYTES,0,0};
    gfx_list l;gfx_cmd flat[128];exec_sw_state es;exec_sw_error err;
    uint32_t i,y;char name[512];FILE *f;
    if(argc<2)return 2;
    if(argc>2)cfg.strip_log2=(uint32_t)atoi(argv[2]);
    cfg.lstrips=480u>>cfg.strip_log2;
    for(i=0;i<MEM_BYTES;i++)memory[i]=(uint8_t)(i*37u+(i>>12));
    for(i=SRC;i<SRC+16384u;i+=4u) {
        uint32_t v=(i/4u)%5u==0u?0x40201008u:0x40201000u+(i&255u);
        memcpy(memory+i,&v,4u);
    }
    gfx_surface_reset(&t);
    if(gfx_surface_set(&t,0,&d)||gfx_surface_set(&t,1,&s))return 3;
    memcpy(initial,memory,MEM_BYTES);
    gfx_list_begin(&l,flat,128u,&t);
    if(compose(&l))return 4;
    exec_sw_reset(&es);
    if(exec_sw_run(&es,&t,&m,flat,l.used,l.used,0,&err))return 5;
    memcpy(golden,memory,MEM_BYTES);memcpy(memory,initial,MEM_BYTES);
    if(gfx_list_begin_strips(&l,table,sizeof table,&t,&cfg,0)||compose(&l))return 6;
    if(exec_sw_run_strips(&t,&m,table,sizeof table,&cfg,scratch,sizeof scratch,0,&err))return 7;
    for(y=0;y<480u;y++)if(memcmp(memory+FB+y*4096u,golden+FB+y*4096u,4096u))return 8;
    memcpy(initial+TABLE,table,sizeof table);dump(argv[1],"init",initial);dump(argv[1],"gold",golden);
    snprintf(name,sizeof name,"%s.surf",argv[1]);f=fopen(name,"w");if(!f)return 9;
    for(i=0;i<16u;i++)fprintf(f,"%08x\n%08x\n%08x\n",t.s[i].base,t.s[i].dim,t.s[i].ctrl);
    fclose(f);printf("G3 GOLDEN PASS: strips=%u records=%u two clips, eq0..5, key, crossing, padding\n",cfg.lstrips,l.used);
    return 0;
}
