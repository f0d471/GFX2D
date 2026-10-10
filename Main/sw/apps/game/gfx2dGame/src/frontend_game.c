/* 游戏只在适配层选择阶段；通用 frame 库不认识游戏与素材。 */
#include "frontend_game.h"
#include "gfx/frame.h"
#include "board.h"
#ifdef FRONTEND_VERIFY
#include "core/format.h"
#endif
#include <string.h>
#ifndef GFX_FRONTEND_BACKGROUND
#define GFX_FRONTEND_BACKGROUND 0
#endif
typedef struct {gfx_list background,dynamic;gfx_frame_plan plan;} frame_context;
static frame_context context[2];
static gfx_frame_arena arena;
static int initialized,static_ready[2],previous_mode[2];
static uint32_t generation;
int frontend_game_reset(void)
{
    uint32_t i;
    for(i=0;i<arena.count;i++)if(arena.page[i].owner)return GFX_BUILD_FULL;
    initialized=0;memset(static_ready,0,sizeof static_ready);return 0;
}
static int init_arena(void)
{
    uint32_t i,next=arena.next_ticket;
    if(!++generation)generation++;
    memset(&arena,0,sizeof arena);
    arena.next_ticket=next;
    for(i=0;i<2;i++) {
        if(gfx_frame_arena_add(&arena,g_mem_regions[REGION_OFFSCREEN].offset+i*0x200000u,0x200000u,generation)<0 ||
           gfx_frame_arena_add(&arena,g_mem_regions[REGION_SHARED].offset+0x800000u+i*0x10000u,0x10000u,generation)<0 ||
           gfx_frame_arena_add(&arena,g_mem_regions[REGION_CMDLIST_A+i].offset,0x100000u,generation)<0 ||
           gfx_frame_arena_add(&arena,g_mem_regions[REGION_FB0+i].offset,0x400000u,generation)<0)return -1;
    }
    initialized=1;return 0;
}
int frontend_game_compose_selected(const stg_game *g,gfx_list *l,int background_mode,
                                    const stg_draw_cfg *cfg,const stg_hud_live *live,
                                    stg_frame_stats *stats)
{
    frame_context *c;uint32_t idx,bglease,prelease,cmdlease,dstlease,mask;gfx_pass pass;
    uint32_t bg=g_mem_regions[REGION_OFFSCREEN].offset;
    int rc;
    if(!background_mode||!l->strip_table)return stg_game_compose(g,l,cfg,live,stats);
    idx=l->buf==(gfx_cmd *)board_region(REGION_CMDLIST_B).ptr?1u:0u;c=&context[idx];
    bg+=idx*0x200000u;bglease=idx*4u;prelease=bglease+1u;cmdlease=bglease+2u;dstlease=bglease+3u;
    if(!initialized&&init_arena())return -1;
    mask=(1u<<bglease)|(1u<<prelease)|(1u<<cmdlease)|(1u<<dstlease);
    if(gfx_frame_begin(&c->plan,&arena,g->frame,mask))return -1;
    if(previous_mode[idx]!=background_mode)static_ready[idx]=0;
    previous_mode[idx]=background_mode;
    if(background_mode==1&&!static_ready[idx]) {
        board_mem off=board_region(REGION_OFFSCREEN);uint32_t x,y;
        uint32_t *pixels=(uint32_t *)(void *)((uint8_t *)off.ptr+idx*0x200000u);
#ifdef FRONTEND_VERIFY
        uint32_t start=board_cycles();
#endif
        for(y=0;y<480u;y++)for(x=0;x<640u;x++)pixels[y*1024u+x]=x<400u?STG_BG_ARGB:STG_HUD_BG;
#ifdef FRONTEND_VERIFY
        {uint32_t elapsed=board_cycles()-start;char b[80];core_out o;core_out_init(&o,b,sizeof b);
         core_out_str(&o,"frebuild,");core_out_u32(&o,idx);core_out_str(&o,",");core_out_u32(&o,elapsed);
         core_out_str(&o,",1228800\n");board_puts(b);}
#endif
        static_ready[idx]=1;
    }
    gfx_list_begin(&c->background,(gfx_cmd *)(void *)((uint8_t *)board_region(REGION_SHARED).ptr+0x800000u+idx*0x10000u),2048u,l->t);
    rc=stg_game_compose_layers(g,l,background_mode==2?&c->background:NULL,15,
                               background_mode==1,cfg,live,stats);
    if(rc)goto cancel;
    c->dynamic=*l;c->dynamic.frame=NULL;
    memset(&pass,0,sizeof pass);pass.surfaces=*l->t;pass.dependency=GFX_PASS_NO_DEP;
    if(background_mode==2) {
        pass.draw=&c->background;pass.target=(gfx_pass_resource){bg,FB_BYTES,generation,bglease};
        pass.read_count=3;
        pass.reads[0]=(gfx_pass_resource){arena.page[prelease].base,c->background.used*32u,generation,prelease};
        pass.reads[1]=(gfx_pass_resource){g_mem_regions[REGION_ASSET].offset,g_mem_regions[REGION_ASSET].size,generation,UINT32_MAX};
        pass.reads[2]=(gfx_pass_resource){0,16384,generation,UINT32_MAX};
        pass.write_count=1;pass.writes[0]=pass.target;
        if(gfx_frame_add_pass(&c->plan,&pass))goto cancel;
        pass.dependency=0;
    }
    pass.draw=&c->dynamic;pass.target=(gfx_pass_resource){l->t->s[cfg->pool.dst].base,FB_BYTES,generation,dstlease};
    pass.initial=(gfx_pass_resource){bg,FB_BYTES,generation,bglease};pass.load=1;pass.initialized=background_mode==1;
    pass.read_count=3;
    pass.reads[0]=(gfx_pass_resource){arena.page[cmdlease].base,0x100000u,generation,cmdlease};
    pass.reads[1]=(gfx_pass_resource){g_mem_regions[REGION_ASSET].offset,g_mem_regions[REGION_ASSET].size,generation,UINT32_MAX};
    pass.reads[2]=(gfx_pass_resource){0,16384,generation,UINT32_MAX};
    pass.write_count=1;pass.writes[0]=pass.target;
    if(gfx_frame_add_pass(&c->plan,&pass)||gfx_frame_seal(&c->plan))goto cancel;
#ifdef INJECT_FRONTEND_PREPASS_FAILURE
    if(background_mode==2)c->background.buf[0].w[0]=0x7fu;
#endif
    l->frame=&c->plan;return 0;
cancel:
    gfx_frame_release(&c->plan,c->plan.ticket);return -1;
}
int frontend_game_compose(const stg_game *g,gfx_list *l,const stg_draw_cfg *cfg,
                           const stg_hud_live *live,stg_frame_stats *stats)
{return frontend_game_compose_selected(g,l,GFX_FRONTEND_BACKGROUND,cfg,live,stats);}
