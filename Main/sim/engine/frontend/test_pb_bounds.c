/* 独立模型边界：容量、包/实例身份、错误 PB、解析窗口与空帧。 */
#include "gfx/pb.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
_Alignas(128) static unsigned char bytes[70656];
static unsigned char arena[16],image[480u*4096u+4096u],gold[sizeof image];
static uint32_t word(unsigned off){uint32_t v;memcpy(&v,bytes+off,4);return v;}
static void set(unsigned off,uint32_t v){memcpy(bytes+off,&v,4);}
int main(void)
{
    gfx_surface_table t={0};
    gfx_surface d={0x20000000u,640,480,12,0,0},s={0x201e0000u,32,32,7,0,0};
    assert(!gfx_surface_set(&t,0,&d)&&!gfx_surface_set(&t,1,&s));
    gfx_pb_config c={{30,4,0,0},64,8192};
    gfx_pb_workspace p={bytes,0x10000000u,sizeof bytes,1};gfx_pb_error e;
    gfx_cmd input={{1,0,0x000f0000u,0x00020001u,0x112233u,0,0,0xf5000010u}};
    gfx_pb_config def={{30,4,0,0},1024,8192},max={{30,4,0,0},32768,131072};
    assert(gfx_pb_bytes(&def)==0x20400u&&gfx_pb_bytes(&max)==0x300400u);
    c.ref_cap=0;
    assert(gfx_pb_build(&t,&input,1,NULL,0,&c,&p,&e)==GFX_E_BAD_FIELD);
    assert(e.stage==3&&e.index==0&&e.instance==UINT32_MAX&&e.tag==input.w[7]);
    c.ref_cap=1;
    assert(gfx_pb_build(&t,&input,1,NULL,0,&c,&p,&e)==GFX_E_BAD_FIELD);
    assert(e.stage==3&&e.index==0&&e.instance==UINT32_MAX&&e.tag==0);
    assert(word(8)==3&&word(32)==2&&word(36)==0&&word(40)==UINT32_MAX);
    c.ref_cap=2;
    assert(!gfx_pb_build(&t,&input,1,NULL,0,&c,&p,&e)&&word(20)==2);
    gfx_instance_view pv={bytes,p.dev_base,p.bytes};gfx_mem a={0x20000000u,image,sizeof image,NULL,0};
    gfx_mem g={0x20000000u,gold,sizeof gold,NULL,0};exec_sw_state state;exec_sw_reset(&state);
    assert(!exec_sw_run(&state,&t,&g,&input,1,1,NULL,NULL));
    gfx_pb_stats stats;assert(!exec_sw_run_pb(&t,&a,&pv,NULL,0,&c,&stats,&e));
    assert(stats.refs==2&&stats.pixels==2&&!memcmp(image,gold,sizeof image));
    set(512+44,1);
    assert(exec_sw_run_pb(&t,&a,&pv,NULL,0,&c,&stats,&e)==GFX_E_BAD_FIELD&&e.stage==5);
    set(512+44,0);set(8,1);
    assert(exec_sw_run_pb(&t,&a,&pv,NULL,0,&c,&stats,&e)==GFX_E_BAD_FIELD);
    set(8,2);pv.bytes=512;
    assert(exec_sw_run_pb(&t,&a,&pv,NULL,0,&c,&stats,&e)==GFX_E_BAD_FIELD);
    c.ref_cap=8192;
    input=(gfx_cmd){{0x00010005u,0x30000000u,1,0x00010001u,0,0x00020004u,0,0xf5000020u}};
    gfx_instance_view v={arena,0x30000000u,sizeof arena};
    assert(gfx_pb_build(&t,&input,1,&v,1,&c,&p,&e)==GFX_E_BAD_FIELD);
    assert(e.stage==1&&e.index==0&&e.instance==UINT32_MAX&&e.tag==input.w[7]);
    input.w[5]=0x00010008u;v.bytes=4;
    assert(gfx_pb_build(&t,&input,1,&v,1,&c,&p,&e)==GFX_E_FETCH);
    assert(e.stage==2&&e.index==0&&e.instance==0&&e.tag==input.w[7]);
    assert(!gfx_pb_build(&t,NULL,0,NULL,0,&c,&p,&e));
    pv.bytes=p.bytes;
    assert(!exec_sw_run_pb(&t,&a,&pv,NULL,0,&c,&stats,&e));
    assert(stats.packets==0&&stats.instances==0&&stats.refs==0&&stats.pixels==0);
    /* 预检拒绝不能先清掉与输入重叠的工作区，也不能发布上一调用的ERROR。 */
    gfx_cmd fill={{1,0,0,0x00010001u,0x123456u,0,0,0x87654321u}};
    memcpy(bytes,&fill,sizeof fill);
    assert(gfx_pb_build(&t,(const gfx_cmd *)(const void *)bytes,1,NULL,0,&c,&p,&e)==GFX_E_BAD_FIELD);
    assert(e.stage==3&&!memcmp(bytes,&fill,sizeof fill));
    set(0,GFX_PB_MAGIC);set(8,1);unsigned char prefix[64];memcpy(prefix,bytes,sizeof prefix);
    gfx_pb_config invalid=c;invalid.draw_cap=0;
    assert(gfx_pb_build(&t,&fill,1,NULL,0,&invalid,&p,&e)==GFX_E_BAD_FIELD);
    assert(!memcmp(prefix,bytes,sizeof prefix));
    puts("F5 PB BOUNDARY PASS capacity/identities/ERROR/READY/windows/empty/max_encoding");
    return 0;
}
