/* F1 首轮源码冻结，链接 1e2cd8d 的生产源码；不引用后续候选实现。 */
#include "board.h"
#include "gfx/gfx.h"
#include "gfx_engine.h"
#include "core/assetpak.h"
#include "core/format.h"
#include "core/crc32.h"
#include "core/replay.h"
#include "game/stg_game.h"
#include "game/stg_kind.h"
#include "stg_modes.h"
#include "renderer.h"
#include "assets.h"
#include "stg_clear.h"
#include <string.h>
#ifndef FRONTEND_ITERATIONS
#define FRONTEND_ITERATIONS 1000u
#endif
#ifndef FRONTEND_FIRST
#define FRONTEND_FIRST 0u
#endif
#ifndef FRONTEND_LAST
#define FRONTEND_LAST 17u
#endif
extern uint32_t frontend_consume(const gfx_list *);
static stg_game game;
static gfx_surface_table tab;
static stg_draw_cfg draw;
static stg_hud_live live;
static uint32_t samples[FRONTEND_ITERATIONS], fails;
static const uint32_t entries[13]={ASSET_BUL16_ENTRY,ASSET_BUL8_ENTRY,ASSET_RICE20_ENTRY,
 ASSET_FONT8X16_ENTRY,ASSET_PSHOT_ENTRY,ASSET_SPARK_ENTRY,ASSET_SHIPS_ENTRY,ASSET_BOSS_ENTRY,
 ASSET_STARS_ENTRY,ASSET_TITLE_ENTRY,ASSET_DIGITS_ENTRY,ASSET_ICONS_ENTRY,ASSET_FONT_CYAN_ENTRY};
static void number(uint32_t v){char b[16];core_out o;core_out_init(&o,b,sizeof b);core_out_u32(&o,v);board_puts(b);}
static void field(uint32_t v){board_puts(",");number(v);}
static int cmp_u32(const void *a,const void *b)
{uint32_t x=*(const uint32_t *)a,y=*(const uint32_t *)b;return x>y?1:x<y?-1:0;}
#include <stdlib.h>
static int setup(void)
{
    board_mem a=board_region(REGION_ASSET),s=board_region(REGION_SHARED);
    assetpak p;assetpak_limits lim={a.size};uint32_t bytes,i,off=0;
    gfx_surface dst={g_mem_regions[REGION_FB0].offset,640,480,12,0,0};
    if(board_asset_read(0,a.ptr,ASSET_PAK_TOTAL_LEN))return 1;
    bytes=ASSET_PAK_TOTAL_LEN;
    if(assetpak_parse(&p,a.ptr,bytes,&lim))return 2;
    gfx_surface_reset(&tab);if(gfx_surface_set(&tab,0,&dst))return 3;
    for(i=0;i<13u;i++) {
        assetpak_entry e;gfx_surface z;
        if(assetpak_entry_at(&p,entries[i],&e))return 4;
        z=(gfx_surface){g_mem_regions[REGION_ASSET].offset+e.offset,e.width,e.height,e.stride_log2,0,e.format};
        if(i==0u||i==1u||i==5u){memcpy((uint8_t *)s.ptr+off,(uint8_t *)a.ptr+e.offset,e.length);z.base=off;z.space=1;off+=e.length;}
        if(gfx_surface_set(&tab,(int)i+2,&z))return 5;
    }
#ifdef __riscv
    if(gfx_engine_soft_reset()||gfx_engine_tex_write(s.ptr,0,off))return 6;
#endif
    stg_modes_draw_cfg(&draw,0);return 0;
}
static void scene(uint32_t id)
{
    static const uint32_t k[6]={0,384,1024,1648,1984,2400};uint32_t i;
    stg_game_init(&game,STG_GAME_SEED);
    if(id<12u){stg_game_press_begin(&game,id<6u?STG_KIND_ROUND16:STG_KIND_SMALL8);stg_game_set_knob(&game,k[id%6u]);for(i=0;i<1200u;i++)stg_game_step(&game,0);}
    else {
        static const uint32_t f[5]={0,5000,9000,9027,9207};replay_view r;
        if(replay_open(stg_clear_bytes,STG_CLEAR_LEN,&r)) {fails++;return;}
        for(i=0;i<=f[id-12u];i++){uint32_t keys;if(replay_key_at(&r,i,&keys)){fails++;break;}stg_game_step(&game,keys);}
    }
}
static void measure(uint32_t id,uint32_t mode,uint32_t repeat)
{
    board_mem mem=board_region(REGION_CMDLIST_A);gfx_list l;stg_frame_stats st;
    gfx_strip_config cfg={30,4,0,0};uint32_t i,start,crc,expected=0;uint64_t total=0;
    for(i=0;i<FRONTEND_ITERATIONS+8u;i++) {
        start=board_cycles();
        if(mode==0u)gfx_list_begin(&l,mem.ptr,32768,&tab);
        else if(gfx_list_begin_strips(&l,mem.ptr,mem.size,&tab,&cfg,0)){fails++;return;}
        if(stg_game_compose(&game,&l,&draw,&live,&st)){fails++;return;}
        if(i>=8u){samples[i-8u]=board_cycles()-start;total+=samples[i-8u];}
        if(l.first_error||l.rejected||!l.ended){fails++;return;}
        if(i==7u)expected=frontend_consume(&l);
    }
    crc=frontend_consume(&l);if(crc!=expected)fails++;
    qsort(samples,FRONTEND_ITERATIONS,sizeof samples[0],cmp_u32);
    board_puts("fcsv");field(id);field(mode);field(repeat);field(FRONTEND_ITERATIONS);
    field((uint32_t)(total>>32));field((uint32_t)total);field(samples[FRONTEND_ITERATIONS/2u]);
    field(samples[(FRONTEND_ITERATIONS*99u)/100u]);field(samples[FRONTEND_ITERATIONS-1u]);
    field(st.act.ebullet);field(l.used);field(l.used*32u);field(l.rejected);field(crc);board_puts("\n");
}
int main(void)
{
    uint32_t id,m,r;board_init();crc32_init();
    board_puts("frontendBench F1 clock=100000000 iterations=");number(FRONTEND_ITERATIONS);board_puts("\n");
    if(setup()){board_puts("setup FAIL\nfail 1\ndone\n");return 1;}
    board_puts("fh,scene,mode,repeat,n,total_hi,total_lo,p50,p99,max,visible_enemy,records,bytes,errors,crc\n");
    for(id=FRONTEND_FIRST;id<FRONTEND_LAST;id++){scene(id);for(r=0;r<3u;r++)for(m=0;m<2u;m++)measure(id,m,r);}
    board_puts("fail ");number(fails);board_puts("\ndone\n");
#ifdef __riscv
    for(;;){}
#endif
    return fails?1:0;
}
