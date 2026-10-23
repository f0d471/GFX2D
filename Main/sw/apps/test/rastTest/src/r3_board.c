#if defined(__riscv)
#include <string.h>
#include "board.h"
#include "gfx_engine.h"
#include "gfx/gfx_abi.h"
#include "gfx/rast_ref.h"
#include "core/crc32.h"
#include "r3_cases.h"

#ifndef R3_TEST_FRAMES
#define R3_TEST_FRAMES R3_CASE_COUNT
#endif
static uint32_t color[10240],texture[4096];
static uint16_t depth[10240];
static unsigned fails;
static void hex(uint32_t v)
{
    char s[9];for(unsigned i=0;i<8;++i)s[i]="0123456789abcdef"[(v>>(28-i*4))&15u];
    s[8]=0;board_puts(s);
}
static int surfaces(void)
{
    return gfx_engine_surf_set(0,g_mem_regions[REGION_OFFSCREEN].offset,640,480,12,0) ||
           gfx_engine_surf_set(1,g_mem_regions[REGION_FB1].offset,640,480,12,0);
}
static void hud_list(uint32_t *list)
{
    memset(list,0,1048576u);
    for(unsigned s=0;s<30;++s) {
        list[s]=1;
        gfx_cmd *c=(gfx_cmd *)((uint8_t *)list+(s+1)*32768u);
        c->w[0]=GFX_OP_FILL|(1u<<20);c->w[2]=8u<<16;
        c->w[3]=(1u<<16)|8u;c->w[4]=R3_HUD_COLOR;c->w[7]=0xc0000000u+s;
    }
}
static uint32_t reference(rast_record *records,unsigned count,uint32_t *bad)
{
    crc32_ctx crc;crc32_begin(&crc);
    volatile uint32_t *dst=board_fb(1);
    for(unsigned s=0;s<30;++s) {
        for(unsigned i=0;i<10240;++i){color[i]=R3_BACKGROUND;depth[i]=65535;}
        rast_target t={color,depth,640,s,texture,64,64,64,R3_KEY};
        if(rast_ref_run(records,count,&t))++*bad;
        for(unsigned x=0;x<8;++x)color[8*640+x]=R3_HUD_COLOR;
        crc32_update(&crc,color,sizeof color);
        for(unsigned y=0;y<16;++y)for(unsigned x=0;x<1024;++x) {
            uint32_t want=x<640 ? color[y*640+x]:0xdeadbeefu;
            if(dst[(s*16+y)*1024+x]!=want)++*bad;
        }
    }
    return crc32_end(&crc);
}
static uint32_t hw_crc(void)
{
    crc32_ctx c;crc32_begin(&c);
    for(unsigned y=0;y<480;++y)crc32_update(&c,board_fb(1)+y*1024,2560);
    return crc32_end(&c);
}
static void health(const char *label,uint32_t *last_field)
{
    uint32_t field=0,min=0,zero=0,under=0,after=0;
    int r=-1;
    for(unsigned i=0;i<8;++i) {
        r=board_display_diag(&field,&min,&zero,&under);
        uint32_t a,b,c;board_display_diag(&after,&a,&b,&c);
        if(field==after)break;
    }
    uint32_t resp=board_display_response_errors();
    board_puts("HEALTH ");board_puts(label);board_puts(" field ");hex(field);
    board_puts(" min ");hex(min);board_puts(" zero ");hex(zero);
    board_puts(" under ");hex(under);board_puts(" resp ");hex(resp);board_puts("\n");
    if(r || field!=after || zero || under || resp || (last_field && field==*last_field))++fails;
    if(last_field)*last_field=field;
}

int r3_board_main(void)
{
    board_init();
    /* Collection starts after the loader has exited. */
    uint32_t delay=board_cycles();while((uint32_t)(board_cycles()-delay)<2u*BOARD_HZ){}
    board_puts("R3 raster board v1\ncaps ");
    uint32_t caps=0;gfx_engine_caps_read(&caps);hex(caps);board_puts("\n");
    if(!(caps&GFX_CAPS_RAST_M)){board_puts("FAIL no RAST\nfail 1\ndone\n");return 1;}
    unsigned textured=(caps&GFX_CAPS_RAST_TEX_M)!=0;
    uint32_t id=0,rcaps=0,r_cycles=0,r_refs=0;
    if(gfx_engine_raster_info(&id,&rcaps,&r_cycles,&r_refs) || id!=GFX_R3_ID_VALUE ||
       rcaps!=(caps&0xc000u))++fails;
    board_puts("R3 id ");hex(id);board_puts(" caps ");hex(rcaps);board_puts("\n");
    uint32_t *bg=board_region(REGION_OFFSCREEN).ptr;
    uint32_t *list=board_region(REGION_CMDLIST_A).ptr;
    rast_record *records=board_region(REGION_SHARED).ptr;
    uint8_t *sub=(uint8_t *)records+4096;
    for(unsigned i=0;i<480u*1024u;++i)bg[i]=board_fb(0)[i]=R3_BACKGROUND;
    hud_list(list);
    for(unsigned i=0;i<4096;++i)texture[i]=r3_texel(i);
    if(gfx_engine_soft_reset() || surfaces())++fails;
    if(textured && gfx_engine_tex_write(texture,0,sizeof texture))++fails;
    board_present(0);board_wait_frame();
    if(board_display_stats_clear())++fails;
    health("before",0);
    uint32_t last_field=0;
    for(unsigned f=0;f<R3_TEST_FRAMES;++f) {
        uint32_t bad=0,code=0xffffffffu,index=0,tag=0;
        int n=r3_scene(f,textured,records,32),bytes=rast_bin(records,(unsigned)n,sub,2048);
        if(n!=16 || bytes<0){++fails;board_puts("FAIL scene generation\n");break;}
        for(unsigned i=0;i<480u*1024u;++i)board_fb(1)[i]=0xdeadbeefu;
        gfx_engine_raster p={g_mem_regions[REGION_SHARED].offset,4096,
            g_mem_regions[REGION_SHARED].offset+4096,2048,(uint32_t)bytes,(uint32_t)n,R3_KEY,0};
        if(f==0) {
            gfx_engine_raster q=p;q.tri_count=65536;
            if(gfx_engine_submit_raster(g_mem_regions[REGION_CMDLIST_A].offset,0,1,&q)!=GFX_ENG_E_ARG)++bad;
            q=p;q.sub_alloc=1;
            if(gfx_engine_submit_raster(g_mem_regions[REGION_CMDLIST_A].offset,0,1,&q)!=GFX_ENG_E_ARG)++bad;
            q=p;q.sub_base=q.tri_base;
            if(gfx_engine_submit_raster(g_mem_regions[REGION_CMDLIST_A].offset,0,1,&q)!=GFX_ENG_E_ARG)++bad;
            q=p;q.tri_base++;
            if(gfx_engine_submit_raster(g_mem_regions[REGION_CMDLIST_A].offset,0,1,&q)!=GFX_ENG_E_ALIGN)++bad;
            board_puts(bad ? "DRIVER reject FAIL\n":"DRIVER reject ok\n");
        }
        uint32_t begin=board_cycles();
        if(n!=16 || bytes<0 || gfx_engine_submit_raster(g_mem_regions[REGION_CMDLIST_A].offset,0,1,&p) ||
           gfx_engine_wait(&code,&index,&tag) || code)++bad;
        uint32_t elapsed=board_cycles()-begin;
        if(gfx_engine_raster_info(&id,&rcaps,&r_cycles,&r_refs) || !r_cycles ||
           r_refs!=((uint32_t)bytes-120u)/2u)++bad;
        board_dcache_invalidate_range(board_fb(1),FB_BYTES);
#if defined(R3_INJECT_PIXEL)
        if(f==0)board_fb(1)[10]^=1u;
#endif
        uint32_t hw=hw_crc(),sw=reference(records,(unsigned)n,&bad);
        if(hw!=sw)++bad;
        board_puts("R3 frame ");hex(f);board_puts(" sw ");hex(sw);board_puts(" hw ");hex(hw);
        board_puts(" bad ");hex(bad);board_puts(" cycles ");hex(elapsed);
        board_puts(" code ");hex(code);board_puts("\n");
        if(bad)++fails;
        health("mixed",&last_field);
    }
    /* Physical error/quiet/recovery path, not just a negative parser fixture. */
    if(R3_TEST_FRAMES>1) {
        r3_scene(0,textured,records,32);int bytes=rast_bin(records,16,sub,2048);
        gfx_engine_raster p={g_mem_regions[REGION_SHARED].offset,4096,
            g_mem_regions[REGION_SHARED].offset+4096,2048,(uint32_t)bytes,16,R3_KEY,0};
        uint32_t code=0xffffffffu,index=0,tag=0,stage=0;
        sub[4]=255;sub[5]=255;
        if(gfx_engine_submit_raster(g_mem_regions[REGION_CMDLIST_A].offset,0,1,&p) ||
           gfx_engine_wait(&code,&index,&tag) || gfx_engine_error_stage_read(&stage) ||
           code!=13 || index!=65535 || tag || stage!=7)++fails;
        board_puts("ERROR index code ");hex(code);board_puts(" index ");hex(index);
        board_puts(" tag ");hex(tag);board_puts(" stage ");hex(stage);board_puts("\n");
        rast_bin(records,16,sub,2048);
        if(gfx_engine_submit_raster(g_mem_regions[REGION_CMDLIST_A].offset,0,1,&p) ||
           gfx_engine_soft_reset())++fails;
        if(surfaces())++fails;
        if(gfx_engine_submit_raster(g_mem_regions[REGION_CMDLIST_A].offset,0,1,&p) ||
           gfx_engine_wait(&code,&index,&tag) || code)++fails;
        board_puts("RESET recovery code ");hex(code);board_puts("\n");
    }
    board_present(1);board_wait_frame();health("after",&last_field);
    board_puts("fail ");
    if(fails)board_puts("1\n");else board_puts("0\n");
    board_puts("done\n");return fails ? 1:0;
}
#endif
