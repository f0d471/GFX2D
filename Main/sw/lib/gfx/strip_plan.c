/* 稳定小引用分桶，封表时连续物化；ABI v2 与错误索引保持。 */
#include "gfx/gfx.h"
#include "strip_internal.h"
#include <stddef.h>
#include <string.h>
static int fail(gfx_list *l,int e)
{l->rejected++;if(!l->first_error)l->first_error=e;return e;}
static int range(const void *p,uint32_t n,uint32_t unit,uintptr_t *end)
{
    uintptr_t a=(uintptr_t)p;
    if(!p || n>UINTPTR_MAX/unit || a>UINTPTR_MAX-(uintptr_t)n*unit)return 0;
    *end=a+(uintptr_t)n*unit;return 1;
}
static int overlaps(uintptr_t a,uintptr_t ae,uintptr_t b,uintptr_t be)
{return a<be && b<ae;}
int gfx_list_begin_strips_planned(gfx_list *l,void *table,uint32_t bytes,
 const gfx_surface_table *t,const gfx_strip_config *cfg,const gfx_rect *clip,gfx_strip_workspace *w)
{
    uintptr_t pe,re,te,we;int e;
    if(!l)return GFX_BUILD_BAD_ARG;
    gfx_list_begin(l,table,UINT32_MAX,t);
    if(!w || ((uintptr_t)w->items&15u) || ((uintptr_t)w->refs&3u) ||
       !w->item_cap || !w->ref_cap ||
       !range(w->items,w->item_cap,48,&pe) || !range(w->refs,w->ref_cap,4,&re) ||
       !range(table,bytes,1,&te) || !range(w,1,sizeof *w,&we) ||
       overlaps((uintptr_t)w->items,pe,(uintptr_t)w->refs,re) ||
       overlaps((uintptr_t)w->items,pe,(uintptr_t)table,te) ||
       overlaps((uintptr_t)w->refs,re,(uintptr_t)table,te) ||
       overlaps((uintptr_t)w,we,(uintptr_t)w->items,pe) ||
       overlaps((uintptr_t)w,we,(uintptr_t)w->refs,re) ||
       overlaps((uintptr_t)w,we,(uintptr_t)table,te))return fail(l,GFX_BUILD_BAD_ARG);
    e=gfx_list_begin_strips(l,table,bytes,t,cfg,clip);
    if(e)return e;
    memset(w->count,0,sizeof w->count);memset(w->offset,0,sizeof w->offset);
    memset(w->cursor,0,sizeof w->cursor);w->items_used=w->refs_used=0;w->sealed=0;
    l->planned=w;return 0;
}
int gfx_strip_plan_add(gfx_list *l,const gfx_cmd *c,const gfx_rect *r,
 uint32_t first,uint32_t last,int32_t dx,int32_t dy,int32_t sx,int32_t sy)
{
    gfx_strip_workspace *w=l->planned;gfx_strip_plan_item *p;uint32_t s,n=last-first+1u;
    if(w->items_used>=w->item_cap || n>w->ref_cap-w->refs_used){l->overflow++;return fail(l,GFX_BUILD_FULL);}
    p=&w->items[w->items_used];p->cmd=*c;
    if(gfx_op(c)!=GFX_OP_NOP) {
        p->cmd.w[2]=(uint32_t)r->x0|((uint32_t)r->y0<<16);
        p->cmd.w[3]=(uint32_t)(r->x1-r->x0+1)|((uint32_t)(r->y1-r->y0+1)<<16);
        if(gfx_op(c)==GFX_OP_BLIT)p->cmd.w[1]=(uint32_t)(sx+r->x0-dx)|((uint32_t)(sy+r->y0-dy)<<16);
    }
    p->first_strip=(uint16_t)first;p->last_strip=(uint16_t)last;
    p->logical_index=w->items_used;p->reserved[0]=p->reserved[1]=0;
    for(s=first;s<=last;s++)w->count[s]++;
#ifdef INJECT_PLAN_EARLY_PUBLISH
    memcpy(l->strip_table,w->count,128u);
#endif
    w->items_used++;w->refs_used+=n;l->used+=n;return 0;
}
int gfx_strip_plan_seal(gfx_list *l)
{
    gfx_strip_workspace *w=l->planned;uint32_t s,i,total=0,sh=1u<<l->strips.strip_log2;
    /* 旧 END 的返回与 ended 语义保持；sticky 首错的表没有可消费表头。 */
    if(l->first_error || l->rejected || l->overflow)return 0;
    for(s=0;s<l->strips.lstrips;s++){w->offset[s]=total;w->cursor[s]=total;total+=w->count[s];}
    if(total!=w->refs_used || total!=l->used || total>w->ref_cap)return fail(l,GFX_BUILD_BAD_ARG);
    for(i=0;i<w->items_used;i++) {
        const gfx_strip_plan_item *p=&w->items[i];
        for(s=p->first_strip;s<=p->last_strip;s++)w->refs[w->cursor[s]++]=i;
    }
    for(s=0;s<l->strips.lstrips;s++) {
        gfx_cmd *out=(gfx_cmd *)(void *)(l->strip_table+(s+1u)*GFX_STRIP_REGION_BYTES);
        for(i=0;i<w->count[s];i++) {
            uint32_t ref=w->refs[w->offset[s]+i];
#ifdef INJECT_PLAN_ORDER
            ref=w->refs[w->offset[s]+w->count[s]-1u-i];
#endif
            gfx_cmd c=w->items[ref].cmd;
            if(gfx_op(&c)!=GFX_OP_NOP) {
                uint32_t y=gfx_u16(c.w[2]>>16),h=gfx_u16(c.w[3]>>16);
                uint32_t lo=y>s*sh?y:s*sh,hi=y+h<(s+1u)*sh?y+h:(s+1u)*sh;
                if(gfx_op(&c)==GFX_OP_BLIT)c.w[1]=(c.w[1]&65535u)|((gfx_u16(c.w[1]>>16)+lo-y)<<16);
                c.w[2]=(c.w[2]&65535u)|((lo-s*sh)<<16);
                c.w[3]=(c.w[3]&65535u)|((hi-lo)<<16);
#ifdef INJECT_PLAN_TAIL
                if(gfx_op(&c)==GFX_OP_BLIT)c.w[1]+=65536u;
#endif
            }
            c.w[7]=s*GFX_STRIP_CAP+i;out[i]=c;
        }
    }
    memcpy(l->strip_table,w->count,128u);w->sealed=1;return 0;
}
int gfx_list_geometry(gfx_list *l,gfx_geometry_cache *c)
{
    if(!l || !l->strip_table || l->ended || !c)return GFX_BUILD_BAD_ARG;
    memset(c,0,sizeof *c);l->geometry=c;return 0;
}
int gfx_list_geometry_generation(gfx_list *l,uint32_t generation)
{
    gfx_geometry_cache *c;
    if(!l||!l->geometry||l->ended)return GFX_BUILD_BAD_ARG;
    c=l->geometry;
    if(c->generation!=generation) {
#ifndef INJECT_TEMPLATE_GENERATION
        memset(c->entry,0,sizeof c->entry);c->active=NULL;
#endif
        c->generation=generation;
    }
    return 0;
}
int gfx_geometry_full(gfx_list *l,const gfx_cmd *c,gfx_rect *r)
{
    gfx_geometry_cache *cache=l->geometry;uint32_t key[17],h=2166136261u,i;gfx_geometry_entry *e;
    int32_t dx=gfx_s16(c->w[2]),dy=gfx_s16(c->w[2]>>16),sx=gfx_s16(c->w[1]),sy=gfx_s16(c->w[1]>>16);
    int32_t w=(int32_t)gfx_u16(c->w[3]),ht=(int32_t)gfx_u16(c->w[3]>>16);
    const gfx_surf_raw *dst=&l->t->s[l->strips.dst_id];
    if(!cache)return 0;
    cache->active=NULL;
    if(dx<0||dy<0||dx+w>(int32_t)gfx_surf_width(dst)||dy+ht>(int32_t)gfx_surf_height(dst)||
       dx<l->build_clip.x0||dy<l->build_clip.y0||dx+w-1>l->build_clip.x1||dy+ht-1>l->build_clip.y1)return 0;
    if(gfx_op(c)==GFX_OP_BLIT) {
        const gfx_surf_raw *src=&l->t->s[gfx_src(c)];
        if(sx<0||sy<0||sx+w>(int32_t)gfx_surf_width(src)||sy+ht>(int32_t)gfx_surf_height(src))return 0;
    }
    key[0]=c->w[0];key[1]=c->w[1];key[2]=c->w[3];key[3]=c->w[4];key[4]=c->w[5];key[5]=c->w[6];
    memcpy(key+6,dst,sizeof *dst);memcpy(key+9,&l->t->s[gfx_src(c)],sizeof *dst);
    memcpy(key+12,&l->build_clip,16);key[16]=(l->strips.strip_log2<<16)|((uint32_t)dy&((1u<<l->strips.strip_log2)-1u));
#ifdef INJECT_TEMPLATE_SOURCE
    key[1]=0;
#endif
#ifdef INJECT_TEMPLATE_EPOCH
    memset(key+12,0,16);
#endif
    for(i=0;i<17u;i++)h=(h^key[i])*16777619u;
    e=&cache->entry[h&7u];
    if(e->valid && !memcmp(key,e->key,sizeof key)){cache->hits++;cache->active=e;}
    else {
        uint32_t sh=1u<<l->strips.strip_log2,phase=(uint32_t)dy&(sh-1u);
        cache->misses++;memcpy(e->key,key,sizeof key);e->valid=1;
        e->leading=(uint32_t)ht<sh-phase?(uint32_t)ht:sh-phase;
#ifdef INJECT_TEMPLATE_PATTERN
        e->leading++;
#endif
        e->pieces=((uint32_t)ht+phase+sh-1u)>>l->strips.strip_log2;
        e->trailing=((uint32_t)ht+phase)&(sh-1u);if(!e->trailing)e->trailing=sh;
        return 0;
    }
    *r=(gfx_rect){dx,dy,dx+w-1,dy+ht-1};return 1;
}
