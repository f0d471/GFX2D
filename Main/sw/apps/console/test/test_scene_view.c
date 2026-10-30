/* Real command construction + software execution: scenery must reach the
 * former margins. A larger upload texture by itself cannot pass these tests. */
#include <stdio.h>
#include <string.h>
#include "gfx/gfx.h"
#include "game/stg_draw.h"
#include "game/stg_stage.h"
#include "game/sw6_scene_a.h"
#include "game/sw6_scene_b.h"

#define SENTINEL 0xdeadbeefu
static uint32_t pixels[0x1000000u/4u],failures;
static gfx_cmd commands[8192];
static gfx_surface_table tab;
static gfx_mem mem={.host=(uint8_t *)pixels,.dev_base=0,.bytes=sizeof pixels};
static stg_game game,previous;
static sw6_scene_a blocks;
static void check(const char *name,int pass)
{if(!pass){printf("FAIL %s\n",name);++failures;}}
static void surface(int id,unsigned base,unsigned width,unsigned height,unsigned log2)
{
    gfx_surface s={base,(uint16_t)width,(uint16_t)height,(uint8_t)log2,0,0};
    check("surface registration",gfx_surface_set(&tab,id,&s)==0);
}
static uint32_t sky(unsigned x,unsigned y) {return 0x00100000u+(x<<8)+(y&31u);}
static unsigned prepare(unsigned width)
{
    const unsigned stride=width>1024u?2048u:1024u;
    surface(0,0,width,480,width>1024u?13:12);
    for(unsigned i=0;i<stride*480u;++i)pixels[i]=SENTINEL;
    return stride;
}
static void execute(gfx_list *list)
{
    exec_sw_state state;exec_sw_error error;
    exec_sw_reset(&state);
    check("valid scene command list",!list->rejected && !list->overflow);
    check("actual scene execution",exec_sw_run_frame(&state,&tab,&mem,commands,list->used,list->used,NULL,&error,
          gfx_surf_width(&tab.s[0]),480)==0);
}
static void coverage(unsigned width,unsigned stride)
{
    int full=1,padding=1;
    for(unsigned y=0;y<480;++y) {
        for(unsigned x=0;x<width;++x)full&=pixels[y*stride+x]!=SENTINEL;
        for(unsigned x=width;x<stride;++x)padding&=pixels[y*stride+x]==SENTINEL;
    }
    check("complete scene coverage",full);check("frame padding untouched",padding);
}
static void danmaku(unsigned width)
{
    unsigned stride=prepare(width);
    stg_draw_cfg cfg={0};
    cfg.pool.dst=0;for(unsigned i=0;i<STG_SRC_N;++i)cfg.pool.src_slot[i]=2;
    cfg.ships_slot=7;cfg.boss_slot=8;cfg.stars_slot=9;cfg.title_slot=10;
    cfg.digits_slot=11;cfg.icons_slot=12;cfg.bglayers_slot=15;
    cfg.font=(stg_font){13,8,16,16,32,96,0};cfg.font_cyan=cfg.font;cfg.font_cyan.slot=14;
    stg_hud_live live={0};stg_frame_stats stats;gfx_list list;
    stg_game_init(&game,0x5e7b0c01u);game.phase=STG_PH_PLAY;game.inv=0;game.slow=1;
    stg_pool_add(&game.f.pools.ebullet,200*256,140*256,0,0,0,0);
    previous=game;
    const stg_draw_view view={width,((int)width-560)/2,(int)width-160};
    gfx_list_begin(&list,commands,8192,&tab);
    check("responsive game composition",stg_game_compose_view(&game,&list,&cfg,&live,&stats,&view)==0 && !stats.rejected);
    check("drawing does not alter gameplay",memcmp(&game,&previous,sizeof game)==0);
    execute(&list);coverage(width,stride);
    unsigned matched=0,total=0;
    for(unsigned y=8;y<120;++y)for(int x=0;x<view.hud_x;++x) {
        if(x>=view.field_x && x<view.field_x+400)continue;
        ++total;matched+=pixels[y*stride+(unsigned)x]==sky((unsigned)x%400u,y);
    }
    check("scenery reaches former margins",matched*100u>total*80u);
}
static void demo(unsigned width)
{
    unsigned stride=prepare(width);gfx_list list;sw6_build_stat stats;
    const gfx_rect full={0,0,(int)width-1,479};
    gfx_list_begin(&list,commands,8192,&tab);gfx_set_clip(&list,full);
    gfx_fill(&list,0,full,SW6_BG_COLOR,GFX_EQ_COPY,0);
    sw6_a_begin(&blocks,SW6_SEED_DEMO,100);sw6_build_stat_clear(&stats);
    sw6_a_build_view(&blocks,&list,0,width,&stats);gfx_list_end(&list);
    check("demo A builds",!stats.bad);execute(&list);coverage(width,stride);
    unsigned outer=0;
    for(unsigned y=160;y<480;++y)for(unsigned x=width*3u/4u;x<width;++x)
        outer+=pixels[y*stride+x]!=SW6_BG_COLOR;
    check("demo objects distributed across viewport",outer>16u);
    for(unsigned scene=0;scene<3;++scene) {
        prepare(width);gfx_list_begin(&list,commands,8192,&tab);gfx_set_clip(&list,full);
        sw6_build_stat_clear(&stats);sw6_b_build_view(scene,&list,0,width,&stats);gfx_list_end(&list);
        check("demo B builds",!stats.bad);execute(&list);coverage(width,stride);
    }
}
int main(void)
{
    gfx_surface_reset(&tab);
    for(int id=2;id<15;++id) {
        unsigned base=0x600000u+(unsigned)(id-2)*0x80000u;surface(id,base,256,512,10);
        for(unsigned y=0;y<512;++y)for(unsigned x=0;x<256;++x)
            pixels[base/4u+y*256u+x]=0x00402010u+((x^y)&31u)*0x00010101u;
    }
    surface(15,0x400000,512,608,11);
    for(unsigned y=0;y<608;++y)for(unsigned x=0;x<512;++x)
        pixels[0x400000u/4u+y*512u+x]=y<480?sky(x,y):x%32u<12u?0u:0x00204060u;
    const unsigned widths[]={640,768,854,1147,1792};
    for(unsigned i=0;i<sizeof widths/sizeof widths[0];++i)danmaku(widths[i]);
    surface(SW6_SLOT_BG_GRAD,0x400000,640,480,12);
    for(unsigned y=0;y<480;++y)for(unsigned x=0;x<640;++x)
        pixels[0x400000u/4u+y*1024u+x]=sky(x,y);
    for(unsigned i=0;i<sizeof widths/sizeof widths[0];++i)demo(widths[i]);
    printf("real scene coverage / textured margins / unchanged gameplay / responsive demos\nfail %u\n",failures);
    return failures?1:0;
}
