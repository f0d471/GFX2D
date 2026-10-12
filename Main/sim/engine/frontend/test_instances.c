/* 独立金标准：从输入坐标构造原 BLIT，绝不调用实例展开器产生期望。 */
#include "gfx/gfx.h"
#include "gfx/census.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
static uint32_t seed=0x904f41u;
static uint32_t rnd(void){seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed;}
static uint8_t initial[8192],actual[8192],expected[8192],tex[1024];
static uint8_t arena_mem[1048576] __attribute__((aligned(16)));
static gfx_cmd packets[128],scalar[131080],expanded[131080];
static gfx_instance_id ids[131080];
static gfx_blit_pos positions[70000];
static uint32_t pair(int x,int y){return ((uint32_t)x&65535u)|((uint32_t)y<<16);}
static void dump(const char *dir,unsigned k,const gfx_surface_table *tab,
                 const gfx_list *l,const gfx_instance_arena *a,const exec_sw_stats *st,unsigned instances)
{
    if(!dir || (k>=144u && k<200u) || k>205u)return;
    unsigned i,j;char path[512];FILE *f;uint8_t image[65536];
#define OPEN(ext) snprintf(path,sizeof path,"%s/case%02u.%s.hex",dir,k,ext);f=fopen(path,"w");assert(f)
    memset(image,0xEE,sizeof image);memcpy(image+0x4000,initial,sizeof initial);
    assert(a->dev_base+a->used<=sizeof image);memcpy(image+a->dev_base,a->host,a->used);
    OPEN("init");for(i=0;i<sizeof image;i+=16){for(j=16;j>0;j--)fprintf(f,"%02x",image[i+j-1]);fputc('\n',f);}fclose(f);
    memcpy(image+0x4000,expected,sizeof expected);
    OPEN("exp");for(i=0;i<sizeof image;i+=16){for(j=16;j>0;j--)fprintf(f,"%02x",image[i+j-1]);fputc('\n',f);}fclose(f);
    OPEN("surf");for(i=0;i<16;i++)fprintf(f,"%08x%08x%08x\n",tab->s[i].ctrl,tab->s[i].dim,tab->s[i].base);fclose(f);
    OPEN("cmd");for(i=0;i<l->used;i++){for(j=8;j>0;j--)fprintf(f,"%08x",l->buf[i].w[j-1]);fputc('\n',f);}fclose(f);
    OPEN("cnt");fprintf(f,"%08x\n00000000\n00001000\n",l->used);fclose(f);
    OPEN("stats");fprintf(f,"%08x\n%08x\n%08x\n%08x\n%08x\n%08x\n%08x\n%08x\n%08x\n",
        st->pixel_total,st->src_oob,st->key_skip,instances,l->used-1u,0u,0u,0u,UINT32_MAX);fclose(f);
    OPEN("tex");for(i=0;i<1024;i+=4)fprintf(f,"%08x\n",*(uint32_t *)(void *)(tex+i));fclose(f);
#undef OPEN
}
static void scene(unsigned k,const char *dir)
{
    const uint32_t sizes[]={0,1,15,16,17,31};
    unsigned i,n=sizes[(k/12)%6],eq=k%6,layout=(k/6)%2;
    gfx_surface_table tab={0};gfx_list il,sl;exec_sw_state is,ss;
    exec_sw_stats ist={0},sst={0};exec_sw_error err;uint32_t ei,ni=0,got=0;
    gfx_instance_arena a={arena_mem,0x0ff0u,sizeof arena_mem,0u,k+1u};
    gfx_instance_desc d={.shared={1,0,(int)(rnd()%21),(int)(rnd()%18),(int)eq,0,(int)(k%3==0),0xFF112233u},
                         .layout=layout,.fixed_sx=-2,.fixed_sy=1};
    if(!d.shared.key_en)d.shared.key=0;
    if(eq==3)d.shared.c=(uint8_t)rnd();
    if(layout)d.fixed_sx=d.fixed_sy=0;
    unsigned space=(k/72)%2,fmt=eq==5?2u:(eq==1?1u:0u);
    gfx_surface dst={0x5000,32,8,7,0,0},src={space?0u:0x4400u,16,16,6,(uint8_t)space,(uint8_t)fmt};
    assert(!gfx_surface_set(&tab,0,&dst));assert(!gfx_surface_set(&tab,1,&src));
    for(i=0;i<8192;i+=4){uint32_t v=0xFF000000u|(rnd()&0x007F7F7Fu);memcpy(initial+i,&v,4);}
    for(i=0;i<1024;i+=4){uint32_t v=0x80000000u|(rnd()&0x007F7F7Fu);memcpy(tex+i,&v,4);}
    memcpy(initial+0x400,tex,sizeof tex);memcpy(actual,initial,sizeof initial);memcpy(expected,initial,sizeof initial);
    gfx_mem im={0x4000,actual,sizeof actual,tex,sizeof tex},sm={0x4000,expected,sizeof expected,tex,sizeof tex};
    gfx_list_begin(&il,packets,128,&tab);gfx_list_begin(&sl,scalar,131080,&tab);
    for(unsigned epoch=0;epoch<2;epoch++) {
        gfx_rect clip={(int32_t)(rnd()%8),(int32_t)(rnd()%3),25,6};
        assert(!gfx_set_clip(&il,clip));assert(!gfx_set_clip(&sl,clip));
        for(i=0;i<n;i++)positions[i]=(gfx_blit_pos){(int32_t)(rnd()%44)-8,(int32_t)(rnd()%24)-8,
            layout?(int32_t)(rnd()%25)-6:d.fixed_sx,layout?(int32_t)(rnd()%25)-6:d.fixed_sy};
        assert(!gfx_instances_append(&il,&d,n?positions:NULL,n,&a));
        assert(gfx_blit_run(&sl,&d.shared,positions,n)==n);
    }
    assert(!gfx_list_end(&il));assert(!gfx_list_end(&sl));
    gfx_instance_view view={a.host,a.dev_base,a.used};
    exec_sw_reset(&is);exec_sw_reset(&ss);
    assert(!exec_sw_run(&ss,&tab,&sm,sl.buf,sl.used,sl.used,&sst,&err));
    assert(!exec_sw_run_instances(&is,&tab,&im,il.buf,il.used,il.used,&view,1,&ist,&err,&ei,&ni));
    if(memcmp(actual,expected,sizeof actual)){fprintf(stderr,"PIXEL scene=%u\n",k);exit(1);}
    assert(ni==2*n && ei==UINT32_MAX && ist.cmd==4u);
    assert(ist.pixel_total==sst.pixel_total && ist.pixel_written==sst.pixel_written && ist.key_skip==sst.key_skip && ist.src_oob==sst.src_oob);
    assert(!gfx_instances_expand(&tab,il.buf,il.used,&view,1,expanded,ids,131080,&got));
    assert(got==sl.used);
    for(i=0;i<got;i++)for(unsigned w=0;w<7;w++)assert(expanded[i].w[w]==sl.buf[i].w[w]);
    gfx_census_state csi,css;gfx_census ci,cs;gfx_census_reset(&csi);gfx_census_reset(&css);
    gfx_census_clear(&ci);gfx_census_clear(&cs);
    assert(!gfx_census_run(&css,&tab,sl.buf,sl.used,&cs));
    assert(!gfx_census_run_instances(&csi,&tab,il.buf,il.used,&view,1,&ci));assert(!memcmp(&ci,&cs,sizeof ci));
    dump(dir,k,&tab,&il,&a,&sst,ni);
}
static void boundaries(void)
{
    gfx_surface_table t={0};gfx_surface s={0x5000,32,8,7,0,0};gfx_list l;
    assert(!gfx_surface_set(&t,0,&s));s.base=0x4000;assert(!gfx_surface_set(&t,1,&s));
    gfx_instance_desc d={.shared={1,0,0,0,0,0,0,0},.layout=0};
    gfx_instance_arena a={arena_mem,0x10000,sizeof arena_mem,0,1};
    for(unsigned i=0;i<70000;i++)positions[i]=(gfx_blit_pos){1,1,0,0};
    gfx_list_begin(&l,packets,128,&t);
    assert(!gfx_instances_append(&l,&d,positions,70000,&a));
    assert(l.used==2 && packets[0].w[2]==65535 && packets[1].w[2]==4465);
    assert(a.used==280016u);
    gfx_instance_view view={a.host,a.dev_base,a.used};uint32_t got=0;
    assert(!gfx_instances_expand(&t,packets,l.used,&view,1,expanded,ids,131080,&got));assert(got==70000);
    assert(ids[65534].packet==0 && ids[65534].instance==65534 && ids[65535].packet==1 && ids[65535].instance==0);
    gfx_cmd saved=packets[0];char decoded[256];
    assert(gfx_cmd_decode(&saved,decoded,sizeof decoded)>0 && strstr(decoded,"BLIT_INST") &&
           strstr(decoded,"base=00010000") && strstr(decoded,"count=65535") && strstr(decoded,"stride=4"));
    uint32_t before=a.used,used=l.used;positions[1].dx=32768;
    assert(gfx_instances_append(&l,&d,positions,2,&a)==GFX_BUILD_BAD_ARG);
    assert(a.used==before && l.used==used && !memcmp(&saved,packets,sizeof saved));
    positions[1].dx=1;l.ended=1;
    assert(gfx_instances_append(&l,&d,NULL,0,&a)==GFX_BUILD_CLOSED);l.ended=0;
    a.bytes=a.used;assert(gfx_instances_append(&l,&d,positions,1,&a)==GFX_BUILD_FULL);
    assert(a.used==before && l.used==used && l.overflow==1);
    gfx_cmd bad=saved;bad.w[2]=0;bad.w[5]=4u|(2u<<16);assert(gfx_instance_packet_check(&t,&bad)==GFX_E_BAD_FIELD);
    bad=saved;bad.w[1]=0xfffffff0u;bad.w[2]=5;assert(gfx_instance_packet_check(&t,&bad)==GFX_E_BAD_FIELD);
    bad=saved;bad.w[2]=0;t.s[1].ctrl=0;assert(gfx_instance_packet_check(&t,&bad)==GFX_E_BAD_SURFACE);
    t.s[1].ctrl=0x80000007;d.shared.src=0;d.shared.w=1;d.shared.h=1;
    a.bytes=sizeof arena_mem;positions[0]=(gfx_blit_pos){0,0,0,0};
    assert(gfx_instances_append(&l,&d,positions,1,&a)==GFX_E_OVERLAP);
    d.shared.src=1;t.s[1].base=t.s[0].base+4u;
    uint32_t before_alias=l.used,arena_before_alias=a.used;
    assert(gfx_instances_append(&l,&d,positions,1,&a)==GFX_BUILD_BAD_ARG);
    assert(l.used==before_alias && a.used==arena_before_alias);
    t.s[1].base=0x4000u;
    /* 原始 ABI 的 stride=12，XYUV 第二个 entry 跨 4 KiB。 */
    gfx_cmd raw={.w={GFX_OP_BLIT_INST|(1u<<16),0x0ff0,3,pair(1,1),0,12u|(1u<<16),0,77}};
    uint32_t *words=(uint32_t *)(void *)arena_mem;for(unsigned i=0;i<9;i++)words[i]=0xAAAAAAAA;
    for(unsigned i=0;i<3;i++){words[i*3]=pair(i+1,1);words[i*3+1]=pair(0,0);}
    view=(gfx_instance_view){arena_mem,0x0ff0,32};gfx_cmd one;
    assert(!gfx_instance_packet_check(&t,&raw));
    for(unsigned i=0;i<3;i++){assert(!gfx_instance_at(&raw,&view,1,i,&one));assert(one.w[2]==pair(i+1,1));}
    view.bytes=28;assert(gfx_instance_at(&raw,&view,1,2,&one)==GFX_E_FETCH);
    puts("F4 BOUNDARY PASS split=65535+4465 atomic=arg/closed/arena overlap=checked stride=12 cross4k padding=checked");
}

static void protocol_scene(unsigned which,const char *dir)
{
    if(!dir)return;
    gfx_surface_table tab={0};gfx_surface dst={0x5000,32,8,7,0,0},src={0x4400,16,16,6,0,0};
    assert(!gfx_surface_set(&tab,0,&dst));assert(!gfx_surface_set(&tab,1,&src));
    for(unsigned i=0;i<8192;i+=4){uint32_t v=0xFF000000u|(i*71u&0xffffffu);memcpy(initial+i,&v,4);}
    memcpy(expected,initial,8192);memset(tex,0,sizeof tex);memset(arena_mem,0xEE,4096);
    gfx_list l,sl;gfx_list_begin(&l,packets,128,&tab);gfx_list_begin(&sl,scalar,131080,&tab);
    for(unsigned i=0;i<17;i++){assert(!gfx_nop(&l));assert(!gfx_nop(&sl));}
    gfx_rect clip={0,0,31,7};assert(!gfx_set_clip(&l,clip));assert(!gfx_set_clip(&sl,clip));
    unsigned n=which==201?20:17,layout=which==200?1:0,stride=which==200?12:4;
    unsigned prefix=which==200?n:which==201?19:which==202?4:which==205?3:0;
    unsigned srcid=which==201?0:1;
    gfx_cmd p={.w={GFX_OP_BLIT_INST|(srcid<<16),0xff0,n,pair(1,1),0,stride|(layout<<16),0,0xF4000099u}};
    if(which==204)p.w[5]=4|(2u<<16);
    packets[l.used++]=p;
    for(unsigned i=0;i<n;i++){
        uint32_t xy=which==201?pair(i==19?0:20,0):pair(i%14+1,i%5+1),uv=0;
        uint32_t *w=(uint32_t *)(void *)(arena_mem+i*stride);w[0]=xy;if(layout)w[1]=uv;
        if(i<prefix)assert(!gfx_blit(&sl,(int)srcid,0,0,0,gfx_s16(xy),gfx_s16(xy>>16),1,1,0,0,0,0));
    }
    assert(!gfx_list_end(&l));assert(!gfx_list_end(&sl));
    gfx_mem m={0x4000,expected,8192,tex,sizeof tex};exec_sw_state es;exec_sw_stats st={0};exec_sw_error e;
    exec_sw_reset(&es);assert(!exec_sw_run(&es,&tab,&m,sl.buf,sl.used,sl.used,&st,&e));
    gfx_instance_arena a={arena_mem,0xff0,4096,((n-1u)*stride+(layout?8u:4u)+15u)&~15u,1};
    dump(dir,which,&tab,&l,&a,&st,prefix);
    char path[512];snprintf(path,sizeof path,"%s/case%02u.stats.hex",dir,which);FILE *f=fopen(path,"w");assert(f);
    unsigned code=which==200?0:which==201?GFX_E_OVERLAP:which==202?10:(which==203||which==205)?12:GFX_E_BAD_FIELD;
    fprintf(f,"%08x\n%08x\n%08x\n%08x\n%08x\n%08x\n%08x\n%08x\n%08x\n",st.pixel_total+(which==205?1u:0u),st.src_oob,st.key_skip,prefix+(which==205?1u:0u),
        which==200?19u:18u,code,which==200?0u:18u,which==200?0u:p.w[7],
        which==200||which==204?UINT32_MAX:which==205?4u:prefix);fclose(f);
}

int main(int argc,char **argv)
{
    boundaries();for(unsigned i=0;i<10000;i++)scene(i,argc>1?argv[1]:NULL);
    for(unsigned which=200;which<=205;which++)protocol_scene(which,argc>1?argv[1]:NULL);
    puts("F4 HOST PASS frames=10000 equations=6 layouts=2 DDR/TEX clip_epochs=2 independent_scalar_pixels=equal");return 0;
}
