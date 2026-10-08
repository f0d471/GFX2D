/* G3 帧级 BAND 板测：混合 DDR/TEX、两次 clip、六方程、空桶和错误恢复。 */
#include <stdint.h>
#include <string.h>
#include "board.h"
#include "gfx_engine.h"
#include "gfx/gfx.h"
#include "core/crc32.h"
#include "core/format.h"
static void put_u32(uint32_t v) {char b[16];core_out o;core_out_init(&o,b,sizeof b);core_out_u32(&o,v);board_puts(b);}
static void put_hex32(uint32_t v) {char b[16];core_out o;core_out_init(&o,b,sizeof b);core_out_hex32(&o,v);board_puts(b);}
#define HW 0x02000000u
#define SW 0x02400000u
#define SOURCE 0x00600000u
#define TEX_SHADOW 0x00610000u
#define TABLE 0x00400000u
static uint32_t fails;
static gfx_cmd flat[512] __attribute__((aligned(32)));
static void check(const char *name,int good)
{
    board_puts(name);board_puts(good?" ok\n":" FAIL\n");if(!good)fails++;
}
static void init_face(uint32_t base)
{
    uint32_t y,x;
    for(y=0;y<480u;y++)for(x=0;x<1024u;x++)
        ((volatile uint32_t *)(uintptr_t)(base+y*4096u))[x]=0x40010203u+y+x;
}
static uint32_t face_crc(uint32_t base)
{
    crc32_ctx c;uint32_t y;crc32_begin(&c);
    for(y=0;y<480u;y++)crc32_update(&c,(void *)(uintptr_t)(base+y*4096u),4096u);
    return crc32_end(&c);
}
static int compose(gfx_list *l)
{
    uint32_t i;
    if(gfx_set_clip(l,(gfx_rect){0,0,399,479}))return 1;
    for(i=0;i<80u;i++) {
        int src=i%2u?1:2,eq=(int)(i%6u);
        if(gfx_blit(l,src,0,0,0,(int)(i%10u)*23-3,(int)(i/10u)*17+11,
                    16,16,eq,eq==3?128u:0u,1,0x40201008u))return 1;
    }
    if(gfx_set_clip(l,(gfx_rect){0,0,639,479}))return 1;
    for(i=0;i<6u;i++)if(gfx_fill(l,0,(gfx_rect){401+(int)i,0,401+(int)i,479},
        0x80304050u,(int)(i%4u),i%4u==3u?96u:0u))return 1;
    return gfx_list_end(l);
}
static int finish(uint32_t code,uint32_t idx,uint32_t tag)
{
    uint32_t ec=99,ei=99,et=99;
    int rc=gfx_engine_wait(&ec,&ei,&et);
    board_puts("error ");put_u32(ec);board_puts("/");put_u32(ei);board_puts("/");put_hex32(et);board_puts("\n");
    return rc==0 && ec==code && ei==idx && et==tag;
}
int main(void)
{
    gfx_surface_table th,ts;
    gfx_surface d={HW,640,480,12,0,0},src={SOURCE,64,64,8,0,0},tx={0,16,16,6,1,0};
    gfx_strip_config cfg={30,4,0,0};gfx_mem m={0,(uint8_t *)0,0x10000000u,(uint8_t *)TEX_SHADOW,1024u};
    gfx_list l;exec_sw_state es;exec_sw_error err;
    uint32_t i,y,x,ec,ei,et,caps,perf[9],a,b;
    uint32_t *counts=(uint32_t *)(uintptr_t)TABLE;
    gfx_cmd *bucket=(gfx_cmd *)(uintptr_t)(TABLE+65536u);
    board_puts("stripTest G3\n");
    check("reset",gfx_engine_soft_reset()==0);
    check("CAPS",gfx_engine_caps_read(&caps)==0 && (caps&GFX_CAPS_BAND_M)!=0);
    for(i=0;i<4096u;i++)((volatile uint32_t *)(uintptr_t)SOURCE)[i]=i%5u?0x40201010u+i%64u:0x40201008u;
    for(y=0;y<16u;y++)for(x=0;x<16u;x++)
        ((volatile uint32_t *)(uintptr_t)TEX_SHADOW)[y*16u+x]=((volatile uint32_t *)(uintptr_t)SOURCE)[y*64u+x];
    board_dcache_invalidate_all();
    check("TEX upload",gfx_engine_tex_write((void *)TEX_SHADOW,0,1024)==0);
    gfx_surface_reset(&th);gfx_surface_set(&th,0,&d);gfx_surface_set(&th,1,&src);gfx_surface_set(&th,2,&tx);
    ts=th;ts.s[0].base=SW;
    init_face(HW);init_face(SW);board_dcache_invalidate_all();
    gfx_list_begin(&l,flat,512,&ts);check("direct build",compose(&l)==0 && !l.first_error);
    exec_sw_reset(&es);check("direct software",exec_sw_run(&es,&ts,&m,flat,l.used,l.used,0,&err)==0);
    check("strip begin",gfx_list_begin_strips(&l,(void *)TABLE,1048576,&th,&cfg,0)==0);
    check("strip build",compose(&l)==0 && !l.first_error);
    check("register dst",gfx_engine_surf_set(0,HW,640,480,12,0)==0);
    check("register DDR",gfx_engine_surf_set(1,SOURCE,64,64,8,0)==0);
    check("register TEX",gfx_engine_surf_set_space(2,0,16,16,6,1,0)==0);
    check("submit",gfx_engine_submit_strips(TABLE,30,4,0,0)==0);
    check("frame finish",finish(0,0,0));board_dcache_invalidate_all();
    a=face_crc(HW);b=face_crc(SW);
    board_puts("CRC hw=");put_hex32(a);board_puts(" sw=");put_hex32(b);board_puts("\n");
    check("same frame and padding",a==b);
    check("perf read",gfx_engine_perf_read(perf,9)==0);
    board_puts("PERF cmd/rd/wr/rdbeat/wrbeat/busy ");
    for(i=0;i<9u;i++){put_u32(perf[i]);board_puts(i==8u?"\n":",");}
    // 空桶：原画 LOAD/FLUSH，保留 padding 和色键跳过区域。
    memset((void *)TABLE,0,128u);
    check("empty submit",gfx_engine_submit_strips(TABLE,30,4,0,0)==0);
    check("empty finish",finish(0,0,0));board_dcache_invalidate_all();
    check("empty unchanged",face_crc(HW)==a);
    // 条带 1 首条禁止 SET_CLIP：首错带条带维，排空后允许重新提交。
    counts[1]=1u;memset(bucket,0,sizeof *bucket);bucket->w[0]=3u;bucket->w[7]=0xcafe005au;
    check("invalid submit",gfx_engine_submit_strips(TABLE,30,4,0,0)==0);
    check("invalid triple",finish(2,1024,0xcafe005au));counts[1]=1025u;
    check("capacity submit",gfx_engine_submit_strips(TABLE,30,4,0,0)==0);
    check("capacity triple",finish(2,1024,0));
    memset((void *)TABLE,0,128u);counts[1]=1u;bucket->w[0]=0u;bucket->w[7]=0x1234u;
    check("recovery submit",gfx_engine_submit_strips(TABLE,30,4,0,0)==0);
    check("recovery finish",finish(0,0,0));
    // 同一 bit 返回旧直通提交入口。
    gfx_list_begin(&l,(gfx_cmd *)TABLE,16,&th);gfx_nop(&l);gfx_list_end(&l);
    check("direct mode restore",gfx_engine_submit(TABLE,l.used)==0);
    check("direct mode finish",gfx_engine_wait(&ec,&ei,&et)==0 && ec==0);
    board_puts("fail ");put_u32(fails);board_puts("\ndone\n");
    for(;;){}
}
