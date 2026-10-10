/* F4 正式板测：显式数组 view，独立标量像素金标准，计数与错误定位。 */
#include "board.h"
#include "gfx_engine.h"
#include "gfx/gfx.h"
#include "core/crc32.h"
#include "core/format.h"
#include <string.h>
static uint32_t fails;
static int has_inst;
static gfx_blit_pos pos[70000];
static void number(uint32_t v){char b[16];core_out o;core_out_init(&o,b,sizeof b);core_out_u32(&o,v);board_puts(b);}
static void field(const char *key,uint32_t v){board_puts(key);number(v);}
static void check(int ok,const char *what){if(!ok){board_puts("FAIL ");board_puts(what);board_puts("\r\n");fails++;}}
static uint32_t pair(int x,int y){return ((uint32_t)x&65535u)|((uint32_t)y<<16);}
static int bind(const gfx_surface_table *t)
{
    for(unsigned i=0;i<16;i++)if(gfx_surf_valid(&t->s[i])){
        const gfx_surf_raw *s=&t->s[i];
        int e=gfx_engine_surf_set_space((int)i,s->base,gfx_surf_width(s),gfx_surf_height(s),
            gfx_surf_stride_log2(s),gfx_surf_space(s),gfx_surf_format(s));if(e)return e;
    }
    return 0;
}
static void scene(unsigned space,unsigned layout,unsigned eq,unsigned n,int raw)
{
    board_mem off=board_region(REGION_OFFSCREEN),shared=board_region(REGION_SHARED);
    uint32_t *fb=off.ptr,*shadow=(uint32_t *)(void *)((uint8_t *)off.ptr+0x200000u);
    uint32_t *texture=(uint32_t *)(void *)((uint8_t *)off.ptr+0x400000u);
    gfx_mem mem={g_mem_regions[REGION_OFFSCREEN].offset,off.ptr,off.size,(uint8_t *)texture,16384u};
    gfx_surface_table tab={0},gold;
    gfx_surface dst={mem.dev_base,32,8,8,0,0};
    gfx_surface src={space?0u:mem.dev_base+0x400000u,16,16,6,(uint8_t)space,(uint8_t)(eq==5?2:(eq==1?1:0))};
    gfx_cmd *cmd=board_region(REGION_CMDLIST_A).ptr,*scalar=board_region(REGION_CMDLIST_B).ptr;
    gfx_list il,sl;gfx_instance_arena a={(uint8_t *)shared.ptr+0xff0u,
        g_mem_regions[REGION_SHARED].offset+0xff0u,0x100000u,0u,1u};
    gfx_instance_desc desc={.shared={1,0,3,3,(int)eq,(uint8_t)(eq==3?73:0),1,0x80010203u},.layout=layout};
    exec_sw_state es;exec_sw_stats sw={0};exec_sw_error err;uint32_t perf[GFX_PERF_COUNT],code=0,index=0,tag=0,ei=0;
    for(unsigned i=0;i<512;i++)fb[i]=shadow[i]=0x7F234567u;
    for(unsigned i=0;i<4096;i++)texture[i]=i%7?0x80402010u:0x80010203u;
    check(!gfx_surface_set(&tab,0,&dst)&&!gfx_surface_set(&tab,1,&src),"surface");gold=tab;gold.s[0].base+=0x200000u;
    check(!bind(&tab),"bind");
    if(space){board_dcache_invalidate_all();check(!gfx_engine_tex_write(texture,0,16384),"tex load");}
    gfx_list_begin(&il,cmd,32768,&tab);gfx_list_begin(&sl,scalar,32768,&gold);
    gfx_rect clip={1,1,28,6};check(!gfx_set_clip(&il,clip)&&!gfx_set_clip(&sl,clip),"clip");
    for(unsigned i=0;i<n;i++)pos[i]=(gfx_blit_pos){(int32_t)(i%37)-3,(int32_t)(i%11)-2,
        layout?(int32_t)(i%20)-3:0,layout?(int32_t)(i%19)-2:0};
    check(!gfx_instances_append(&il,&desc,n?pos:NULL,n,&a),"append");
    check(gfx_blit_run(&sl,&desc.shared,pos,n)==n,"scalar build");
    if(raw && n){
        gfx_cmd *p=&cmd[1];p->w[5]=12u|(1u<<16);desc.layout=1;
        uint32_t *w=(uint32_t *)(void *)a.host;
        for(unsigned i=0;i<n;i++){w[i*3]=pair(pos[i].dx,pos[i].dy);w[i*3+1]=pair(pos[i].sx,pos[i].sy);}
        a.used=((n-1u)*12u+8u+15u)&~15u;
    }
    check(!gfx_list_end(&il)&&!gfx_list_end(&sl),"end");
    exec_sw_reset(&es);check(!exec_sw_run(&es,&gold,&mem,scalar,sl.used,sl.used,&sw,&err),"software golden");
    check(!gfx_engine_perf_clear(),"perf clear");uint32_t start=board_cycles();
    uint32_t submitted=(uint32_t)(uintptr_t)cmd,submitted_count=il.used;
    if(!has_inst) {
        gfx_instance_view view={a.host,a.dev_base,a.used};
        gfx_instance_id *ids=(gfx_instance_id *)(void *)((uint8_t *)shared.ptr+0x200000u);
        check(!gfx_instances_expand(&tab,cmd,il.used,&view,1u,scalar,ids,32768u,&submitted_count),"fallback expand");
        for(unsigned i=1;i<=n;i++)check(ids[i].packet==1u&&ids[i].instance==i-1u,"fallback identity");
        submitted=(uint32_t)(uintptr_t)scalar;
    }
    check(!gfx_engine_submit(submitted,submitted_count),"submit");
    int rc=gfx_engine_wait(&code,&index,&tag);uint32_t elapsed=board_cycles()-start;
    check(!rc&&!code,"finish");check(!gfx_engine_perf_read(perf,GFX_PERF_COUNT),"perf read");
    check(!gfx_engine_error_instance_read(&ei)&&ei==UINT32_MAX,"error reset");
    board_dcache_invalidate_all();uint32_t h=crc32_ieee(fb,2048u),s=crc32_ieee(shadow,2048u);
    check(h==s,"pixels and padding");
    check(perf[GFX_PERF_INSTANCE]==(has_inst?n:0u)&&perf[GFX_PERF_CMD]==(has_inst?2u:n+1u),"instance/cmd count");
    check(perf[GFX_PERF_PIXEL]==sw.pixel_total-sw.src_oob&&perf[GFX_PERF_KEY_SKIP]==sw.key_skip,"pixel/key count");
    field("case space=",space);field(" layout=",layout);field(" eq=",eq);field(" n=",n);
    field(" raw=",(uint32_t)raw);field(" hw_crc=",h);field(" sw_crc=",s);field(" cycles=",elapsed);board_puts("\r\n");
    /* 同一合法包改成坏 layout：包错误实例号保持 FFFFFFFF，像素不被写。 */
    cmd[1].w[5]=4u|(2u<<16);cmd[1].w[7]=0xF4000001u;
    if(!has_inst) {
        gfx_instance_view view={a.host,a.dev_base,a.used};uint32_t unused=0;
        check(gfx_instances_expand(&tab,cmd,il.used,&view,1u,scalar,NULL,32768u,&unused)==GFX_E_BAD_FIELD,"fallback reject before publication");
        return;
    }
    check(!gfx_engine_submit((uint32_t)(uintptr_t)cmd,il.used),"bad submit");
    rc=gfx_engine_wait(&code,&index,&tag);check(!gfx_engine_error_instance_read(&ei),"bad instance read");
    check(!rc&&code==GFX_E_BAD_FIELD&&index==1u&&tag==0xF4000001u&&ei==UINT32_MAX,"bad packet identity");
}
static void max_count(void)
{
    board_mem off=board_region(REGION_OFFSCREEN),shared=board_region(REGION_SHARED);
    gfx_surface_table t={0};gfx_surface d={g_mem_regions[REGION_OFFSCREEN].offset,32,8,8,0,0};
    check(!gfx_surface_set(&t,0,&d),"max dst");d.base+=0x400000u;check(!gfx_surface_set(&t,1,&d),"max src");
    gfx_cmd *cmd=board_region(REGION_CMDLIST_A).ptr;gfx_list l;
    gfx_instance_arena a={shared.ptr,g_mem_regions[REGION_SHARED].offset,shared.size,0u,1u};
    gfx_instance_desc desc={.shared={1,0,0,0,0,0,0,0}};
    for(unsigned i=0;i<70000;i++)pos[i]=(gfx_blit_pos){0,0,0,0};
    gfx_list_begin(&l,cmd,32768,&t);check(!gfx_instances_append(&l,&desc,pos,70000,&a),"max append");
    check(cmd[0].w[2]==65535u&&cmd[1].w[2]==4465u,"max split");check(!gfx_list_end(&l),"max end");
    check(!bind(&t)&&!gfx_engine_perf_clear(),"max bind");uint32_t c,i,tag,perf[GFX_PERF_COUNT];
    check(!gfx_engine_submit((uint32_t)(uintptr_t)cmd,l.used),"max submit");
    check(!gfx_engine_wait(&c,&i,&tag)&&!c,"max finish");check(!gfx_engine_perf_read(perf,GFX_PERF_COUNT),"max perf");
    check(perf[GFX_PERF_INSTANCE]==70000u&&perf[GFX_PERF_CMD]==2u&&perf[GFX_PERF_PIXEL]==0u,"max counters");
    char decoded[256];
    check(gfx_cmd_decode(&cmd[0],decoded,sizeof decoded)>0 && strstr(decoded,"BLIT_INST") &&
        strstr(decoded,"count=65535") && strstr(decoded,"stride=4"),"instance decode");
    board_puts("DECODE ");board_puts(decoded);board_puts("\r\n");
    board_puts("MAX split=65535+4465 complete\r\n");(void)off;
}
static void v2_reject(void)
{
    board_mem off=board_region(REGION_OFFSCREEN),commands=board_region(REGION_CMDLIST_A);
    uint32_t base=g_mem_regions[REGION_OFFSCREEN].offset;
    gfx_surface_table t={0};gfx_surface bg={base,640,16,12,0,0},dst={base+0x200000u,640,16,12,0,0};
    check(!gfx_surface_set(&t,0,&bg)&&!gfx_surface_set(&t,1,&dst),"v2 surfaces");
    gfx_surface src={base+0x400000u,16,16,6,0,0};check(!gfx_surface_set(&t,2,&src),"v2 source");
    uint32_t *b=off.ptr,*d=(uint32_t *)(void *)((uint8_t *)off.ptr+0x200000u);
    for(unsigned i=0;i<16384u;i++){b[i]=0x12345678u;d[i]=0xA55A3CC3u;}
    uint32_t *counts=commands.ptr;for(unsigned i=0;i<32;i++)counts[i]=0;counts[0]=1;
    gfx_cmd *p=(gfx_cmd *)(void *)((uint8_t *)commands.ptr+32768u);
    *p=(gfx_cmd){.w={GFX_OP_BLIT_INST|(2u<<16)|(1u<<20),g_mem_regions[REGION_SHARED].offset,0,pair(1,1),0,4,0,0xF40000A5u}};
    check(!bind(&t)&&!gfx_engine_perf_clear(),"v2 bind");uint32_t code,index,tag,instance;
    check(!gfx_engine_submit_strips(g_mem_regions[REGION_CMDLIST_A].offset,1,4,0,1),"v2 submit");
    check(!gfx_engine_wait(&code,&index,&tag),"v2 wait");check(!gfx_engine_error_instance_read(&instance),"v2 identity read");
    check(code==GFX_E_BAD_OPCODE&&index==0&&tag==0xF40000A5u&&instance==UINT32_MAX,"v2 reject BLIT_INST");
    board_dcache_invalidate_all();for(unsigned i=0;i<16384u;i++)check(d[i]==0xA55A3CC3u,"v2 no flush after error");
    board_puts("V2 BLIT_INST rejected before flush\r\n");
}
int main(void)
{
    board_init();board_puts("instanceTest start\r\n");uint32_t caps=0;
    check(!gfx_engine_caps_read(&caps),"CAPS read");
    has_inst=(caps&GFX_CAPS_INST_M)!=0;field("fallback ",(uint32_t)!has_inst);board_puts("\r\n");
    check(!gfx_engine_soft_reset(),"reset");
    const unsigned counts[]={0,1,15,16,17};
    for(unsigned sp=0;sp<2;sp++)for(unsigned ly=0;ly<2;ly++)for(unsigned eq=0;eq<6;eq++)
        for(unsigned k=0;k<5;k++)scene(sp,ly,eq,counts[k],0);
    scene(0,1,1,17,1);scene(1,1,5,17,1);if(has_inst){max_count();v2_reject();}
    else board_puts("FALLBACK identity preserved before publication\r\n");
    field("fail ",fails);board_puts("\r\ndone\r\n");return (int)fails;
}
