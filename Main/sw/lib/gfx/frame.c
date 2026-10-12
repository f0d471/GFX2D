/* 通用串行 pass 计划：预检无 doorbell，租约保持到完成或错误排空。 */
#include "gfx/frame.h"
#include <string.h>
static int err(gfx_frame_plan *p,int e){if(p&&!p->first_error)p->first_error=e;return e;}
static int valid_range(uint32_t b,uint32_t n){return n && (uint64_t)b+n<=UINT32_MAX;}
static int overlap(uint32_t a,uint32_t an,uint32_t b,uint32_t bn)
{return (uint64_t)a+an>b && (uint64_t)b+bn>a;}
int gfx_frame_arena_add(gfx_frame_arena *a,uint32_t base,uint32_t bytes,uint32_t gen)
{
    uint32_t i;
    if(!a||a->count>=GFX_FRAME_MAX_LEASES||!valid_range(base,bytes)||!gen||(base&31u))return GFX_BUILD_BAD_ARG;
    for(i=0;i<a->count;i++)if(overlap(base,bytes,a->page[i].base,a->page[i].bytes))return GFX_BUILD_BAD_ARG;
    a->page[a->count++]=(gfx_lease){base,bytes,gen,0};return 0;
}
int gfx_frame_begin(gfx_frame_plan *p,gfx_frame_arena *a,uint32_t frame,uint32_t mask)
{
    uint32_t i,t,collision;
    if(!p||!a||!mask||a->count>GFX_FRAME_MAX_LEASES||mask>>a->count)return GFX_BUILD_BAD_ARG;
#ifndef INJECT_FRAME_REUSE
    for(i=0;i<a->count;i++)if((mask&(1u<<i))&&a->page[i].owner)return GFX_BUILD_FULL;
#endif
    do {
        t=++a->next_ticket;if(!t)t=++a->next_ticket;
        collision=0;
        for(i=0;i<a->count;i++)if(a->page[i].owner==t)collision=1;
    } while(collision);
    memset(p,0,sizeof *p);p->arena=a;p->frame_id=frame;p->mask=mask;p->ticket=t;
    for(i=0;i<a->count;i++)if(mask&(1u<<i))a->page[i].owner=t;
    return 0;
}
static int resource(const gfx_frame_plan *p,const gfx_pass_resource *r)
{
    const gfx_lease *l;
    if(!valid_range(r->base,r->bytes))return 0;
    if(r->lease==UINT32_MAX)return r->generation!=0; /* 调用方声明为只读的外部资源。 */
    if(r->lease>=p->arena->count||!(p->mask&(1u<<r->lease)))return 0;
    l=&p->arena->page[r->lease];
    return l->owner==p->ticket&&l->generation==r->generation&&r->base>=l->base&&
           (uint64_t)r->base+r->bytes<=(uint64_t)l->base+l->bytes;
}
static int hazard(const gfx_pass *a,const gfx_pass *b)
{
    gfx_pass_resource aw[5],ar[5],bw[5],br[5];uint32_t i,j,an=1,bn=1,arn=0,brn=0;
    aw[0]=a->target;bw[0]=b->target;
    for(i=0;i<a->write_count;i++)aw[an++]=a->writes[i];
    for(i=0;i<b->write_count;i++)bw[bn++]=b->writes[i];
    for(i=0;i<a->read_count;i++)ar[arn++]=a->reads[i];
    for(i=0;i<b->read_count;i++)br[brn++]=b->reads[i];
    if(a->load)ar[arn++]=a->initial;
    if(b->load)br[brn++]=b->initial;
    for(i=0;i<an;i++) {
        for(j=0;j<bn;j++)if(overlap(aw[i].base,aw[i].bytes,bw[j].base,bw[j].bytes))return 1;
        for(j=0;j<brn;j++)if(overlap(aw[i].base,aw[i].bytes,br[j].base,br[j].bytes))return 1;
    }
    for(i=0;i<arn;i++)for(j=0;j<bn;j++)if(overlap(ar[i].base,ar[i].bytes,bw[j].base,bw[j].bytes))return 1;
    return 0;
}
int gfx_frame_add_pass(gfx_frame_plan *p,const gfx_pass *v)
{
    uint32_t i;
    if(!p||!p->arena||!v)return GFX_BUILD_BAD_ARG;
    if(p->sealed||p->published)return err(p,GFX_BUILD_CLOSED);
    if(p->count>=GFX_FRAME_MAX_PASSES)return err(p,GFX_BUILD_FULL);
    if(v->read_count>4||v->write_count>4||!v->draw||!resource(p,&v->target)||v->target.lease==UINT32_MAX||
       (v->dependency!=GFX_PASS_NO_DEP&&v->dependency>=p->count))return err(p,GFX_BUILD_BAD_ARG);
    if(v->load&&(!resource(p,&v->initial)||(!v->initialized&&v->dependency==GFX_PASS_NO_DEP)))return err(p,GFX_BUILD_BAD_ARG);
    for(i=0;i<v->read_count;i++)if(!resource(p,&v->reads[i]))return err(p,GFX_BUILD_BAD_ARG);
    for(i=0;i<v->write_count;i++)if(!resource(p,&v->writes[i])||v->writes[i].lease==UINT32_MAX)return err(p,GFX_BUILD_BAD_ARG);
    p->pass[p->count++]=*v;return 0;
}
int gfx_frame_seal(gfx_frame_plan *p)
{
    uint32_t i,j;
    if(!p||!p->arena)return GFX_BUILD_BAD_ARG;
    if(p->sealed||p->published)return err(p,GFX_BUILD_CLOSED);
    if(p->first_error)return p->first_error;
    if(!p->count||p->count>GFX_FRAME_MAX_PASSES)return err(p,GFX_BUILD_BAD_ARG);
    for(i=0;i<p->count;i++) {
        gfx_pass *v=&p->pass[i];const gfx_list *l=v->draw;
        if(v->read_count>4u||v->write_count>4u||
           (v->dependency!=GFX_PASS_NO_DEP&&v->dependency>=i)||!l||!l->t||
           !l->ended||l->first_error||l->rejected||l->overflow||(l->planned&&!l->planned->sealed)||
           memcmp(l->t,&v->surfaces,sizeof v->surfaces))return err(p,GFX_BUILD_BAD_ARG);
        if(!resource(p,&v->target)||(v->load&&!resource(p,&v->initial)))return err(p,GFX_BUILD_BAD_ARG);
        for(j=0;j<i;j++) {
            int depends=0;uint32_t d=v->dependency;
            while(d!=GFX_PASS_NO_DEP){if(d==j){depends=1;break;}d=p->pass[d].dependency;}
            if(!depends&&hazard(v,&p->pass[j]))return err(p,GFX_E_OVERLAP);
        }
    }
    p->sealed=1;return 0;
}
int gfx_frame_publish(gfx_frame_plan *p,uint32_t *ticket)
{
    uint32_t i,j;
    if(!p||!p->arena||!ticket||!p->sealed||p->first_error||p->published)return GFX_BUILD_BAD_ARG;
    for(i=0;i<p->arena->count;i++)if((p->mask&(1u<<i))&&p->arena->page[i].owner!=p->ticket)return err(p,GFX_BUILD_BAD_ARG);
    for(i=0;i<p->count;i++) {
        const gfx_pass *v=&p->pass[i];
        if(!resource(p,&v->target)||(v->load&&!resource(p,&v->initial)))return err(p,GFX_BUILD_BAD_ARG);
        for(j=0;j<v->read_count;j++)if(!resource(p,&v->reads[j]))return err(p,GFX_BUILD_BAD_ARG);
        for(j=0;j<v->write_count;j++)if(!resource(p,&v->writes[j]))return err(p,GFX_BUILD_BAD_ARG);
    }
    p->published=1;*ticket=p->ticket;return 0;
}
int gfx_frame_complete(gfx_frame_plan *p,uint32_t id,int e,int drained)
{
    if(!p||!p->arena||!p->published||id!=p->completed||id>=p->count)return GFX_BUILD_BAD_ARG;
    if(e){p->drained=drained;return err(p,e);}
    p->pass[id].completion_ticket=p->ticket;p->completed++;p->drained=p->completed==p->count;return 0;
}
int gfx_frame_release(gfx_frame_plan *p,uint32_t t)
{
    uint32_t i;
    if(!p||!p->arena||t!=p->ticket||(!p->drained&&p->published))return GFX_BUILD_BAD_ARG;
    for(i=0;i<p->arena->count;i++)if(p->mask&(1u<<i)){
        if(p->arena->page[i].owner!=t)return GFX_BUILD_BAD_ARG;
    }
    for(i=0;i<p->arena->count;i++)if(p->mask&(1u<<i))p->arena->page[i].owner=0;
    p->arena=0;return 0;
}
