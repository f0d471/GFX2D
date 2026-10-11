/* F5：独立标量像素基准与 PB 字节金标准。导出一万张 RTL 构建场景。 */
#include "gfx/pb.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint32_t seed=0x51b00ffu;
static uint32_t rnd(void){seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed;}
static uint32_t pair(int x,int y){return (uint32_t)(uint16_t)x|((uint32_t)(uint16_t)y<<16);}
static uint8_t initial[480u*4096u+4096u],actual[sizeof initial],expected[sizeof initial];
static uint8_t arena[8192] __attribute__((aligned(16))),pb_data[71040] __attribute__((aligned(128)));
static gfx_cmd cmds[64],scalars[128];
static uint32_t word(const uint8_t *p){uint32_t v;memcpy(&v,p,4);return v;}
static void emit(FILE *f,unsigned n,unsigned an,unsigned used,unsigned refs,unsigned refoff)
{
    fprintf(f,"%u %u %u %u\n",n,an/4,used,refs);
    for(unsigned i=0;i<n;i++){for(unsigned w=8;w>0;w--)fprintf(f,"%08x",cmds[i].w[w-1]);fputc('\n',f);}
    for(unsigned i=0;i<an;i+=4)fprintf(f,"%08x\n",word(arena+i));
    for(unsigned i=0;i<512;i+=16){for(unsigned w=16;w>0;w--)fprintf(f,"%02x",pb_data[i+w-1]);fputc('\n',f);}
    for(unsigned i=0;i<used;i++){for(unsigned w=64;w>0;w--)fprintf(f,"%02x",pb_data[512+i*64+w-1]);fputc('\n',f);}
    unsigned span=0;
    for(unsigned b=0;b<30;b++)span+=(word(pb_data+68+b*8)*8u+15u)&~15u;
    fprintf(f,"%u\n",span/16);
    for(unsigned i=0;i<span;i+=16){for(unsigned w=16;w>0;w--)fprintf(f,"%02x",pb_data[refoff+i+w-1]);fputc('\n',f);}
}
int main(int argc,char **argv)
{
    FILE *out=argc>1?fopen(argv[1],"w"):NULL;
    if(argc>1)assert(out);
    unsigned scenes=10000;
    if(out)fprintf(out,"%u\n",scenes);
    gfx_surface_table tab={0};
    gfx_surface dst={0x20000000u,640,480,12,0,0},src={0x201e0000u,32,32,7,0,0};
    assert(!gfx_surface_set(&tab,0,&dst));assert(!gfx_surface_set(&tab,1,&src));
    gfx_pb_config cfg={{30,4,0,0},64,8192};
    gfx_pb_workspace pb={pb_data,0x10000000u,sizeof pb_data,1};
    assert(gfx_pb_bytes(&cfg)==70656u);
    for(unsigned i=0;i<sizeof initial;i+=4){uint32_t v=0xff000000u|(rnd()&0x7f7f7fu);memcpy(initial+i,&v,4);}
    for(unsigned k=0;k<scenes;k++){
        memset(cmds,0,sizeof cmds);memset(arena,0,sizeof arena);
        unsigned n=0,sn=0,an=0,ni=0;
        for(unsigned group=0;group<2;group++){
            gfx_cmd c={0};c.w[0]=3;c.w[1]=pair((int)(rnd()%21)-10,(int)(rnd()%200)-10);
            c.w[3]=pair(k%11?640:0,k%11?480:0);c.w[7]=n;
            cmds[n++]=c;scalars[sn++]=c;
            c=(gfx_cmd){{0}};c.w[0]=1;c.w[2]=pair((int)(rnd()%700)-30,(int)(rnd()%520)-20);
            c.w[3]=pair(1+rnd()%80,1+rnd()%480);c.w[4]=0x80402010;c.w[7]=n;
            if(k%29==0){c.w[2]=pair(-32768,-32768);c.w[3]=0xffffffffu;}
            cmds[n++]=c;scalars[sn++]=c;
            unsigned layout=(k+group)%2,step=layout?8:4;
            if(k%13==0)step=12;
            if(k%17==0)step=256;
            unsigned count=(k+group)%7;
            if(k%19==0 && group==0)an=4080;
            an=(an+15u)&~15u;
            assert(an+count*step+16<sizeof arena);
            unsigned eq=k%6,key=k%3==0;
            c=(gfx_cmd){{0}};c.w[0]=5|(eq<<8)|(key<<12)|(1u<<16)|(eq==3?0x42000000u:0);
            c.w[1]=0x30000000u+an;c.w[2]=count;c.w[3]=pair(k%23?1+rnd()%32:0,1+rnd()%32);
            if(k%31==0)c.w[3]=0xffffffffu;
            c.w[4]=key?0xff123456u:0;c.w[5]=step|(layout<<16);c.w[6]=layout?0:pair(-2,1);c.w[7]=n;
            cmds[n++]=c;
            for(unsigned j=0;j<count;j++){
                uint32_t xy=pair((int)(rnd()%700)-30,(int)(rnd()%520)-20);
                uint32_t uv=layout?pair((int)(rnd()%44)-8,(int)(rnd()%44)-8):c.w[6];
                memcpy(arena+an+j*step,&xy,4);if(layout)memcpy(arena+an+j*step+4,&uv,4);
                gfx_cmd s=c;s.w[0]=(s.w[0]&~255u)|2;s.w[1]=uv;s.w[2]=xy;s.w[4]=0;s.w[5]=c.w[4];s.w[6]=0;
                scalars[sn++]=s;
            }
            ni+=count;an=(an+count*step+15u)&~15u;
        }
        cmds[n++]=(gfx_cmd){{255,0,0,0,0,0,0,0}};
        cmds[n++]=(gfx_cmd){{0xdeadbeef,0,0,0,0,0,0,0}}; /* END 后必须不检查。 */
        gfx_instance_view view={arena,0x30000000u,sizeof arena},pv={pb_data,pb.dev_base,pb.bytes};
        gfx_pb_error error;int rc=gfx_pb_build(&tab,cmds,n,&view,1,&cfg,&pb,&error);
        if(rc){fprintf(stderr,"BUILD scene=%u code=%d idx=%u stage=%u\n",k,rc,error.index,error.stage);return 1;}
        memcpy(actual,initial,sizeof actual);memcpy(expected,initial,sizeof expected);
        gfx_mem am={0x20000000u,actual,sizeof actual,NULL,0},em={0x20000000u,expected,sizeof expected,NULL,0};
        exec_sw_state state;exec_sw_reset(&state);exec_sw_stats st={0};
        assert(!exec_sw_run(&state,&tab,&em,scalars,sn,sn,&st,NULL));
        gfx_pb_stats stats;rc=exec_sw_run_pb(&tab,&am,&pv,&view,1,&cfg,&stats,&error);
        if(rc||memcmp(actual,expected,sizeof actual)||stats.instances!=ni){fprintf(stderr,"PIXEL scene=%u code=%d\n",k,rc);return 1;}
        if(out)emit(out,n,an,word(pb_data+16),word(pb_data+20),word(pb_data+28));
    }
    if(out)fclose(out);
    printf("F5 PB HOST PASS scenes=%u exact_pixels=%u skipped=0\n",scenes,scenes);
    return 0;
}
