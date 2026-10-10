#include "gfx/frame.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void need(int ok,const char *s){if(!ok){printf("FAIL %s\n",s);exit(1);}}
int main(void)
{
    gfx_frame_arena a={0};gfx_frame_plan p,q;gfx_surface_table tab;
    gfx_surface s={0x10000,640,480,12,0,0};gfx_list l;gfx_cmd buf[8];gfx_pass v;uint32_t ticket;
    gfx_surface_reset(&tab);need(!gfx_surface_set(&tab,0,&s),"surface");
    gfx_list_begin(&l,buf,8,&tab);gfx_fill(&l,0,(gfx_rect){0,0,639,479},0,0,0);gfx_list_end(&l);
    need(gfx_frame_arena_add(&a,0x10000,0x200000,1)==0,"lease");
    need(gfx_frame_arena_add(&a,0x20000,0x200000,1)!=0,"overlap rejected");
    need(gfx_frame_arena_add(&a,0xffffffe0,64,1)!=0,"wrap rejected");
    need(!gfx_frame_arena_add(&a,0x400000,0x200000,1),"second lease");
    need(!gfx_frame_begin(&p,&a,1,3),"begin");
    need(gfx_frame_begin(&q,&a,2,1)==GFX_BUILD_FULL,"shared page reuse rejected");
    memset(&v,0,sizeof v);v.draw=&l;v.surfaces=tab;v.target=(gfx_pass_resource){0x10000,0x1e0000,1,0};v.dependency=GFX_PASS_NO_DEP;
    need(!gfx_frame_add_pass(&p,&v),"add");
    v.target=(gfx_pass_resource){0x400000,0x1e0000,1,1};v.initial=(gfx_pass_resource){0x10000,0x1e0000,1,0};v.load=1;v.dependency=0;
    need(!gfx_frame_add_pass(&p,&v)&&!gfx_frame_seal(&p),"dependency seal");
    need(!gfx_frame_publish(&p,&ticket),"publish");
    need(gfx_frame_complete(&p,1,0,1)!=0,"LOAD before prepass rejected");
    need(gfx_frame_release(&p,ticket)!=0,"inflight release rejected");
    need(!gfx_frame_complete(&p,0,0,1)&&!gfx_frame_complete(&p,1,0,1),"serial complete");
    need(!gfx_frame_release(&p,ticket),"release");
    need(gfx_frame_publish(&p,&ticket)!=0,"released plan cannot republish");
    need(!gfx_frame_begin(&p,&a,2,3),"reuse after complete");
    v.dependency=GFX_PASS_NO_DEP;v.initialized=0;
    need(gfx_frame_add_pass(&p,&v)!=0,"uninitialized LOAD rejected");
    need(gfx_frame_publish(&p,&ticket)!=0,"failed seal unpublished");
    need(!gfx_frame_release(&p,p.ticket),"cancel unpublished");
    need(!gfx_frame_begin(&p,&a,3,3),"generation case");v.load=0;v.target.generation=2;
    need(gfx_frame_add_pass(&p,&v)!=0,"stale generation rejected");gfx_frame_release(&p,p.ticket);
    need(!gfx_frame_begin(&p,&a,4,3),"error case");v.target.generation=1;
    need(!gfx_frame_add_pass(&p,&v)&&!gfx_frame_seal(&p)&&!gfx_frame_publish(&p,&ticket),"error submit");
    need(gfx_frame_complete(&p,0,9,0)==9&&gfx_frame_release(&p,ticket)!=0,"error undrained retained");
    p.drained=1;need(!gfx_frame_release(&p,ticket),"error drained release");
    need(!gfx_frame_begin(&p,&a,5,3),"hazard case");
    v.target=(gfx_pass_resource){0x10000,0x1e0000,1,0};v.load=0;v.read_count=1;
    v.reads[0]=(gfx_pass_resource){0x400000,0x1e0000,1,1};v.dependency=GFX_PASS_NO_DEP;
    need(!gfx_frame_add_pass(&p,&v),"reader pass");
    v.target=(gfx_pass_resource){0x400000,0x1e0000,1,1};v.read_count=0;
    need(!gfx_frame_add_pass(&p,&v)&&gfx_frame_seal(&p)==GFX_E_OVERLAP,"write-after-read dependency rejected");
    gfx_frame_release(&p,p.ticket);
    need(!gfx_frame_begin(&p,&a,6,3),"generation after seal");
    need(!gfx_frame_add_pass(&p,&v)&&!gfx_frame_seal(&p),"generation seal");
    a.page[1].generation=2;need(gfx_frame_publish(&p,&ticket)!=0,"generation changed before publish rejected");
    gfx_frame_release(&p,p.ticket);
    puts("PASS: frame leases, overlap, wrap, generation, initialization, dependency, publication, error drain");return 0;
}
