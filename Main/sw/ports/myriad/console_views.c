#include "board.h"
#include "renderer.h"
#include "console_content.h"
#include "console_views.h"
#include "console_present.h"
#include "stg_modes.h"
#include "game/sw6_scene_b.h"
int console_view_prepare(appfw_ctx *c,int family,int hw)
{
    stg_runtime *r=content_runtime(c);
    int rc=family==CONSOLE_VIEW_DEMO?stg_runtime_surfaces_demo(r):stg_runtime_surfaces_stg(r);
    return rc?rc:renderer_prepare(hw,&r->tab);
}
int console_view_begin(appfw_ctx *c,gfx_list *l,int *dst)
{
    stg_runtime *r=content_runtime(c);uint32_t i=content(c)->seq&1u;
    *dst=(int)i;
    if(*dst==content(c)->shown)return -1;
    if(console_present_begin(&r->tab,*dst,NULL)<0)return appfw_error("view layout",-1);
    gfx_list_begin(l,r->cmds[i],r->cap[i],&r->tab);return 0;
}
unsigned console_view_width(appfw_ctx *c,int dst)
{return gfx_surf_width(&content_runtime(c)->tab.s[dst]);}
int console_view_submit(appfw_ctx *c,const gfx_list *l,int dst,int hw)
{
    stg_runtime *r=content_runtime(c);exec_sw_stats stats;uint32_t cycles,ticket;
    unsigned width=console_view_width(c,dst);
    int rc=width>640u?renderer_run_view(&r->tab,&r->mem,&r->es,&r->err,l,&stats,&cycles,width):
        renderer_run(hw,&r->tab,&r->mem,&r->es,&r->err,l,&stats,&cycles);
    if(rc!=0)return appfw_error("view render",-1);
    if(console_present_request(dst,&ticket)!=0 || board_present_wait(ticket)!=0)return appfw_error("view present",-1);
    content(c)->shown=dst;++content(c)->seq;return 0;
}
int console_view_flush(appfw_ctx *c)
{
    int shown;if(renderer_pipeline_flush(&shown)!=0)return appfw_error("view flush",-1);
    if(shown>=0)content(c)->shown=shown;
    return 0;
}
int console_view_backend(appfw_ctx *c) { return (int)content_runtime(c)->backend; }
void console_view_set_backend(appfw_ctx *c,int backend) { content_runtime(c)->backend=(uint32_t)backend; }
int console_demo_prepare(appfw_ctx *c,struct sw6_scene_b *scene)
{
    stg_runtime *r=content_runtime(c);uint32_t eq[4],bad=0;
    int hw=app_port_hardware() && r->backend!=0;
    if(stg_runtime_surfaces_demo(r)!=0)return -1;
    sw6_b_ctx ctx={&r->tab,&r->mem,&r->es,&r->err,r->cmds[0],r->cap[0]};
    sw6_b_begin(scene,SW6_K_DEFAULT);
    if(sw6_b_precompose(scene,&ctx,eq,&bad)!=0 || bad)return -1;
    r->backend=(uint32_t)hw;return renderer_prepare(hw,&r->tab);
}
uint32_t console_view_crc(appfw_ctx *c)
{
    board_dcache_invalidate_all();return stg_modes_frame_crc(board_fb(content(c)->shown));
}
uint32_t console_view_fields(void) { return board_frame_count(); }
uint32_t console_view_presents(void) { return board_swap_count(); }
uint32_t console_view_faults(void) { return board_irq_faults(); }
