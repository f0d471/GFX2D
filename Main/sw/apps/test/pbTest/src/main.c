/* F5 板上通用矩形与实例：独立标量像素、PB 字节、容量及错误完成。 */
#include "board.h"
#include "gfx_engine.h"
#include "gfx/pb.h"
#include "core/crc32.h"
#include "core/format.h"
#include <string.h>
static uint32_t fails;
static gfx_blit_pos positions[31];
static void number(uint32_t v){char b[16];core_out o;core_out_init(&o,b,sizeof b);core_out_u32(&o,v);board_puts(b);}
static void field(const char *s,uint32_t v){board_puts(s);number(v);}
static void check(int ok,const char *s){if(!ok){board_puts("FAIL ");board_puts(s);board_puts("\r\n");fails++;}}
static uint32_t pair(int x,int y){return (uint32_t)(uint16_t)x|((uint32_t)(uint16_t)y<<16);}
static int bind(const gfx_surface_table *t)
{
    for(unsigned i=0;i<16;i++)if(gfx_surf_valid(&t->s[i])){
        const gfx_surf_raw *s=&t->s[i];
        int rc=gfx_engine_surf_set_space((int)i,s->base,gfx_surf_width(s),gfx_surf_height(s),
            gfx_surf_stride_log2(s),gfx_surf_space(s),gfx_surf_format(s));if(rc)return rc;
    }
    return 0;
}
static void scene(unsigned space,unsigned layout,unsigned eq,unsigned n,unsigned raw)
{
    board_mem off=board_region(REGION_OFFSCREEN),shared=board_region(REGION_SHARED);
    uint32_t base=g_mem_regions[REGION_OFFSCREEN].offset;
    uint32_t *fb=off.ptr,*shadow=(uint32_t *)((uint8_t *)off.ptr+0x200000u);
    uint32_t *background=(uint32_t *)((uint8_t *)off.ptr+0x400000u);
    uint32_t *texture=(uint32_t *)((uint8_t *)off.ptr+0x600000u);
    gfx_surface_table t={0},gold;
    gfx_surface dst={base,640,32,12,0,0},bg={base+0x400000u,640,32,12,0,0};
    gfx_surface src={space?0u:base+0x600000u,32,32,7,(uint8_t)space,(uint8_t)(eq==5?2:eq==1?1:0)};
    check(!gfx_surface_set(&t,0,&dst)&&!gfx_surface_set(&t,1,&src)&&!gfx_surface_set(&t,2,&bg),"surfaces");
    gold=t;gold.s[0].base+=0x200000u;
    for(unsigned i=0;i<32768;i++)fb[i]=shadow[i]=background[i]=0x7f234567u;
    for(unsigned i=0;i<4096;i++)texture[i]=i%7?0x80402010u:0x80010203u;
    check(!bind(&t),"bind");
    if(space){board_dcache_invalidate_all();check(!gfx_engine_tex_write(texture,0,16384),"TEX load");}
    gfx_cmd *input=board_region(REGION_CMDLIST_A).ptr,*scalar=board_region(REGION_CMDLIST_B).ptr;
    gfx_instance_arena arena={(uint8_t *)shared.ptr+0xff0u,g_mem_regions[REGION_SHARED].offset+0xff0u,0x100000u,0,1};
    gfx_list il,sl;gfx_list_begin(&il,input,32768,&t);gfx_list_begin(&sl,scalar,32768,&gold);
    gfx_instance_desc desc={.shared={1,0,13,19,(int)eq,(uint8_t)(eq==3?73:0),1,0x80010203u},
        .layout=layout,.fixed_sx=layout?0:-2,.fixed_sy=layout?0:1};
    check(!gfx_set_clip(&il,(gfx_rect){0,0,639,31})&&!gfx_set_clip(&sl,(gfx_rect){0,0,639,31}),"clip full");
    check(!gfx_fill(&il,0,(gfx_rect){0,0,639,31},0x112233,0,0)&&
          !gfx_fill(&sl,0,(gfx_rect){0,0,639,31},0x112233,0,0),"fill");
    for(unsigned i=0;i<n;i++)positions[i]=(gfx_blit_pos){(int32_t)(i%37)-3,(int32_t)(i%35)-2,
        layout?(int32_t)(i%40)-4:-2,layout?(int32_t)(i%35)-3:1};
    for(unsigned epoch=0;epoch<2;epoch++){
        if(epoch)check(!gfx_set_clip(&il,(gfx_rect){-4,-3,63,29})&&
                      !gfx_set_clip(&sl,(gfx_rect){-4,-3,63,29}),"clip epoch");
        check(!gfx_instances_append(&il,&desc,n?positions:NULL,n,&arena),"instance build");
        check(gfx_blit_run(&sl,&desc.shared,positions,n)==n,"scalar build");
    }
    if(raw&&n){
        uint32_t next=0;
        for(unsigned k=0;k<il.used;k++)if(gfx_op(&input[k])==GFX_OP_BLIT_INST){
            input[k].w[1]=arena.dev_base+next;input[k].w[5]=12u|(1u<<16);
            uint32_t *words=(uint32_t *)(void *)(arena.host+next);
            for(unsigned j=0;j<n;j++){words[j*3]=pair(positions[j].dx,positions[j].dy);words[j*3+1]=pair(positions[j].sx,positions[j].sy);}
            next+=(((n-1u)*12u+8u)+15u)&~15u;
        }
        arena.used=next;
    }
    check(!gfx_list_end(&il)&&!gfx_list_end(&sl),"end");
    gfx_mem mem={base,off.ptr,off.size,(uint8_t *)texture,16384};exec_sw_state es;exec_sw_reset(&es);
    exec_sw_stats sw={0};exec_sw_error se;
    check(!exec_sw_run(&es,&gold,&mem,scalar,sl.used,sl.used,&sw,&se),"independent scalar");
    gfx_pb_config cfg={{2,4,2,0},1024,8192};
    gfx_pb_workspace model={(uint8_t *)shared.ptr+0x600000u,g_mem_regions[REGION_SHARED].offset+0x500000u,0x20400u,1};
    gfx_instance_view view={arena.host,arena.dev_base,arena.used};gfx_pb_error pe;
    check(!gfx_pb_build(&t,input,il.used,&view,1,&cfg,&model,&pe),"PB model");
    const uint32_t *header=(const uint32_t *)(const void *)model.host;
    uint32_t used=header[4],refs=header[5],ro=header[7],span=0;
    for(unsigned b=0;b<2;b++)span+=(header[17+b*2]*8u+15u)&~15u;
    gfx_engine_pb config={model.dev_base,model.bytes,1024,8192,2,4,2,0};
    check(!gfx_engine_perf_clear(),"clear");uint32_t start=board_cycles(),code,index,tag,perf[GFX_PERF_COUNT];
    check(!gfx_engine_submit_pb((uint32_t)(uintptr_t)input,il.used,&config),"PB submit");
    check(!gfx_engine_wait(&code,&index,&tag)&&!code,"PB finish");
    uint32_t elapsed=board_cycles()-start;
    check(!gfx_engine_perf_read(perf,GFX_PERF_COUNT),"PERF read");
    board_dcache_invalidate_all();
    uint8_t *actual=(uint8_t *)shared.ptr+0x500000u;
    check(!memcmp(actual,model.host,512)&&!memcmp(actual+512,model.host+512,used*64u)&&
          !memcmp(actual+ro,model.host+ro,span),"PB directory metadata refs padding");
    uint32_t hc=crc32_ieee(fb,32u*4096u),sc=crc32_ieee(shadow,32u*4096u);
    check(hc==sc,"pixel and row padding");
    check(perf[GFX_PERF_CMD]==5u&&perf[GFX_PERF_INSTANCE]==2*n&&perf[GFX_PERF_REF_COUNT]==refs,"packets instances refs");
    check(perf[GFX_PERF_PIXEL]==sw.pixel_total-sw.src_oob&&perf[GFX_PERF_KEY_SKIP]==sw.key_skip,"pixel and key PERF");
    check(perf[GFX_PERF_PB_WR_BEAT]==(16u+64u*used+512u+span)/16u,"PB written beats");
    check(perf[GFX_PERF_PB_RD_BEAT]<=perf[GFX_PERF_RD_BEAT]&&perf[GFX_PERF_PB_WR_BEAT]<=perf[GFX_PERF_WR_BEAT],"traffic categories");
    field("pbcase space=",space);field(" layout=",layout);field(" eq=",eq);field(" n=",n);field(" raw=",raw);
    field(" hw_crc=",hc);field(" sw_crc=",sc);field(" refs=",refs);field(" bin=",perf[GFX_PERF_BIN_CYCLES]);field(" cycles=",elapsed);board_puts("\r\n");
    if(n==1&&eq==0&&space==0&&layout==0){
        check(!gfx_engine_submit((uint32_t)(uintptr_t)scalar,sl.used),"v1 return");
        check(!gfx_engine_wait(&code,&index,&tag)&&!code,"v1 finish");
        board_dcache_invalidate_all();check(crc32_ieee(fb,32u*4096u)==sc,"v1/PB mode pixels");
    }
}
static void failures(void)
{
    board_mem off=board_region(REGION_OFFSCREEN),shared=board_region(REGION_SHARED);
    uint32_t base=g_mem_regions[REGION_OFFSCREEN].offset;gfx_surface_table t={0};
    gfx_surface d={base,640,32,12,0,0};check(!gfx_surface_set(&t,0,&d),"failure surface");check(!bind(&t),"failure bind");
    gfx_cmd *p=board_region(REGION_CMDLIST_A).ptr;
    p[0]=(gfx_cmd){{1,0,0,0x00200280u,0x112233,0,0,0xf5000000u}};p[1]=(gfx_cmd){{255,0,0,0,0,0,0,0}};
    gfx_engine_pb cfg={g_mem_regions[REGION_SHARED].offset+0x500000u,0x20400u,1024,1,2,4,0,0};
    for(unsigned test=0;test<4;test++){
        uint32_t before=crc32_ieee(off.ptr,32u*4096u),code,index,tag,stage,inst;
        if(test==1){cfg.ref_cap=8192;p[0].w[6]=1;}
        if(test==2)p[0]=(gfx_cmd){{0x00010005u,g_mem_regions[REGION_SHARED].offset,1,0x00010001u,0,0x00020004u,0,0xf5000002u}};
        if(test==3){cfg.ref_cap=0;p[0]=(gfx_cmd){{1,0,0,0x00010001u,0x112233,0,0,0xf5000003u}};}
        check(!gfx_engine_submit_pb((uint32_t)(uintptr_t)p,2,&cfg),"failure submit");
        check(!gfx_engine_wait(&code,&index,&tag)&&code==2,"failure completion");
        check(!gfx_engine_error_stage_read(&stage)&&stage==((test==0||test==3)?3u:1u),"failure stage");
        check(!gfx_engine_error_instance_read(&inst)&&inst==UINT32_MAX,"failure instance");
        board_dcache_invalidate_all();check(crc32_ieee(off.ptr,32u*4096u)==before,"preflight preserves dst");
        uint32_t *h=(uint32_t *)((uint8_t *)shared.ptr+0x500000u);check(h[2]==3&&h[8]==2&&h[11]==stage,"ERROR PB header");
        field("pberror test=",test);field(" code=",code);field(" stage=",stage);field(" index=",index);field(" tag=",tag);board_puts("\r\n");
    }
    p[0]=(gfx_cmd){{1,0,0,0x00200280u,0x112233,0,0,0xf5000000u}};cfg.ref_cap=8192;uint32_t code,index,tag;
    check(!gfx_engine_submit_pb((uint32_t)(uintptr_t)p,2,&cfg)&&!gfx_engine_wait(&code,&index,&tag)&&!code,"recovery after error");
}
static void clip_roundtrip(void)
{
    board_mem off=board_region(REGION_OFFSCREEN);uint32_t base=g_mem_regions[REGION_OFFSCREEN].offset;
    uint32_t *fb=off.ptr,*shadow=(uint32_t *)((uint8_t *)off.ptr+0x200000u);
    gfx_surface_table t={0};gfx_surface d={base,640,32,12,0,0};
    check(!gfx_surface_set(&t,0,&d)&&!bind(&t),"roundtrip bind");
    gfx_cmd *p=board_region(REGION_CMDLIST_A).ptr;uint32_t code,index,tag;
    gfx_engine_pb cfg={g_mem_regions[REGION_SHARED].offset+0x500000u,0x20400u,1024,8192,2,4,0,0};
    for(unsigned failure=0;failure<2;failure++){
        for(unsigned k=0;k<32768;k++)fb[k]=shadow[k]=0xdeadc0deu;
        p[0]=(gfx_cmd){{3,0,0,0x00060006u,0,0,0,0}};
        check(!gfx_engine_submit((uint32_t)(uintptr_t)p,1)&&!gfx_engine_wait(&code,&index,&tag)&&!code,"old clip set");
        p[0]=(gfx_cmd){{1,0,0,0x00200280u,0x112233,0,0,0}};
        cfg.ref_cap=failure?1:8192;
        check(!gfx_engine_submit_pb((uint32_t)(uintptr_t)p,1,&cfg)&&!gfx_engine_wait(&code,&index,&tag)&&code==(failure?2u:0u),"PB roundtrip completion");
        if(!failure)for(unsigned y=0;y<32;y++)for(unsigned x=0;x<640;x++)shadow[y*1024u+x]=0x112233;
        p[0].w[4]=0x556677;
        check(!gfx_engine_submit((uint32_t)(uintptr_t)p,1)&&!gfx_engine_wait(&code,&index,&tag)&&!code,"new v1 command after PB");
        for(unsigned y=0;y<6;y++)for(unsigned x=0;x<6;x++)shadow[y*1024u+x]=0x556677;
        board_dcache_invalidate_all();
        check(crc32_ieee(fb,32u*4096u)==crc32_ieee(shadow,32u*4096u),"private clip preserved on success/failure");
    }
    board_puts("PB/V1 private clip success/failure roundtrip ok\r\n");
}
int main(void)
{
    board_init();board_puts("pbTest start\r\n");uint32_t caps;
    check(!gfx_engine_caps_read(&caps)&&(caps&GFX_CAPS_BIN_M),"BIN capability");
    check(!gfx_engine_soft_reset(),"reset");
    const unsigned counts[]={0,1,31};
    for(unsigned sp=0;sp<2;sp++)for(unsigned ly=0;ly<2;ly++)for(unsigned eq=0;eq<6;eq++)
        for(unsigned k=0;k<3;k++)scene(sp,ly,eq,counts[k],0);
    scene(0,1,1,31,1);scene(1,1,5,31,1);failures();clip_roundtrip();
    field("fail ",fails);board_puts("\r\ndone\r\n");return (int)fails;
}
