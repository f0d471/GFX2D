/* ABI v2 定长条带表：裁剪吸收、保序分桶与执行时的纯检查。 */
#include "gfx/gfx.h"
#include "strip_internal.h"
#include <stddef.h>
#include <stdint.h>

static uint64_t end_of(const gfx_surf_raw *s)
{
    return (uint64_t)s->base + (((uint64_t)gfx_surf_height(s)-1u) << gfx_surf_stride_log2(s))
         + (uint64_t)gfx_surf_width(s)*4u;
}
static int intersect(const gfx_surf_raw *a, const gfx_surf_raw *b)
{
    return (uint64_t)a->base < end_of(b) && (uint64_t)b->base < end_of(a);
}
int gfx_strip_config_check(const gfx_surface_table *t, const gfx_strip_config *cfg)
{
    const gfx_surf_raw *b, *d;
    if (!t || !cfg || cfg->bg_id>=GFX_SURF_N || cfg->dst_id>=GFX_SURF_N ||
        !cfg->lstrips || cfg->lstrips>GFX_STRIP_MAX || cfg->strip_log2<3u || cfg->strip_log2>5u)
        return GFX_E_BAD_FIELD;
    b=&t->s[cfg->bg_id]; d=&t->s[cfg->dst_id];
    if (!gfx_surface_struct_ok(t,cfg->bg_id) || !gfx_surface_struct_ok(t,cfg->dst_id) ||
        (b->base&15u) || (d->base&15u) || gfx_surf_space(b) || gfx_surf_space(d) ||
        gfx_surf_format(b) || gfx_surf_format(d) || gfx_surf_width(b)!=640u ||
        gfx_surf_width(d)!=640u || gfx_surf_height(b)!=gfx_surf_height(d) ||
        gfx_surf_height(d)!=(cfg->lstrips<<cfg->strip_log2) || gfx_surf_height(d)>480u ||
        (intersect(b,d) && (b->base!=d->base || gfx_surf_stride_log2(b)!=gfx_surf_stride_log2(d))))
        return GFX_E_BAD_SURFACE;
    return GFX_OK;
}
static int feedback(const gfx_surface_table *t, const gfx_strip_config *cfg, const gfx_cmd *c)
{
    return gfx_op(c)==GFX_OP_BLIT && gfx_surf_space(&t->s[gfx_src(c)])==GFX_SPACE_DDR &&
           intersect(&t->s[gfx_src(c)],&t->s[cfg->dst_id]);
}
int gfx_strip_cmd_check(const gfx_surface_table *t, const gfx_strip_config *cfg, const gfx_cmd *c)
{
    int e=gfx_cmd_check(t,c);
    uint32_t op=gfx_op(c), sh=1u<<cfg->strip_log2;
    int32_t y=gfx_s16(c->w[2]>>16);
    if(e) return e;
    if(feedback(t,cfg,c)) return GFX_E_OVERLAP;
    if(op==GFX_OP_SET_CLIP || op==GFX_OP_END ||
       ((op==GFX_OP_FILL || op==GFX_OP_BLIT) &&
        (gfx_dst(c)!=cfg->dst_id || y<0 || (uint32_t)y>=sh ||
         (uint32_t)y+gfx_u16(c->w[3]>>16)>sh))) return GFX_E_BAD_FIELD;
    return GFX_OK;
}
static int rejected(gfx_list *l,int e)
{
    l->rejected++;
    if(!l->first_error) l->first_error=e;
    return e;
}
int gfx_list_begin_strips(gfx_list *l, void *table, uint32_t bytes,
                         const gfx_surface_table *t, const gfx_strip_config *cfg,
                         const gfx_rect *initial_clip)
{
    int e;
    uint32_t i;
    if(!l) return GFX_BUILD_BAD_ARG;
    gfx_list_begin(l,table,UINT32_MAX,t);
    if(!table || ((uintptr_t)table&3u) || !cfg ||
       bytes<(cfg->lstrips+1u)*GFX_STRIP_REGION_BYTES) return rejected(l,GFX_BUILD_BAD_ARG);
    e=gfx_strip_config_check(t,cfg);
    if(e) return rejected(l,e);
    l->strip_table=table;l->strips=*cfg;
    l->build_clip=initial_clip ? *initial_clip : (gfx_rect){0,0,639,479};
    for(i=0;i<32u;i++) ((uint32_t *)table)[i]=0;
    return GFX_OK;
}
int gfx_strip_append_checked(gfx_list *l,const gfx_cmd *c)
{
    uint32_t *counts=(uint32_t *)(void *)l->strip_table;
    uint32_t op=gfx_op(c),sh=1u<<l->strips.strip_log2,s,s0,s1;
    gfx_rect r;
    int32_t dx=gfx_s16(c->w[2]),dy=gfx_s16(c->w[2]>>16);
    int32_t sx=gfx_s16(c->w[1]),sy=gfx_s16(c->w[1]>>16);
    if(op==GFX_OP_SET_CLIP) {
        int32_t x=gfx_s16(c->w[1]),y=gfx_s16(c->w[1]>>16);
        l->build_clip=(gfx_rect){x,y,x+(int32_t)gfx_u16(c->w[3])-1,
                                   y+(int32_t)gfx_u16(c->w[3]>>16)-1};
        return GFX_OK;
    }
    if(op==GFX_OP_END) return GFX_OK;
    if(feedback(l->t,&l->strips,c)) return rejected(l,GFX_E_OVERLAP);
    if(op!=GFX_OP_NOP && gfx_dst(c)!=l->strips.dst_id) return rejected(l,GFX_E_BAD_FIELD);
    if(op==GFX_OP_NOP) {s0=s1=0;r=(gfx_rect){0,0,0,0};}
    else {
        uint32_t w=gfx_u16(c->w[3]),h=gfx_u16(c->w[3]>>16);
        const gfx_surf_raw *d=&l->t->s[l->strips.dst_id];
        if(!w || !h) return GFX_OK;
        r=(gfx_rect){dx,dy,dx+(int32_t)w-1,dy+(int32_t)h-1};
        if(r.x0<0)r.x0=0;
        if(r.y0<0)r.y0=0;
        if(r.x0<l->build_clip.x0)r.x0=l->build_clip.x0;
        if(r.y0<l->build_clip.y0)r.y0=l->build_clip.y0;
        if(r.x1>639)r.x1=639;
        if(r.y1>=(int32_t)gfx_surf_height(d))r.y1=(int32_t)gfx_surf_height(d)-1;
        if(r.x1>l->build_clip.x1)r.x1=l->build_clip.x1;
        if(r.y1>l->build_clip.y1)r.y1=l->build_clip.y1;
        if(r.x0>r.x1 || r.y0>r.y1)return GFX_OK;
        /* 源有效区也先裁入，防止裁剪后源坐标超出 int16；源越界原本就是不写。 */
        if(op==GFX_OP_BLIT) {
            const gfx_surf_raw *ss=&l->t->s[gfx_src(c)];
            int32_t xmin=dx-sx,ymin=dy-sy;
            int32_t xmax=xmin+(int32_t)gfx_surf_width(ss)-1;
            int32_t ymax=ymin+(int32_t)gfx_surf_height(ss)-1;
            if(r.x0<xmin)r.x0=xmin;
            if(r.y0<ymin)r.y0=ymin;
            if(r.x1>xmax)r.x1=xmax;
            if(r.y1>ymax)r.y1=ymax;
            if(r.x0>r.x1 || r.y0>r.y1)return GFX_OK;
        }
        s0=(uint32_t)r.y0>>l->strips.strip_log2;
        s1=(uint32_t)r.y1>>l->strips.strip_log2;
    }
    /* 两遍只走被覆盖的桶：先检查容量，确保整次调用无部分追加。 */
    if(op==GFX_OP_BLIT && (sx+r.x0-dx>32767 || sy+r.y1-dy>32767))
        return rejected(l,GFX_BUILD_BAD_ARG);
    for(s=s0;s<=s1;s++) if(counts[s]>=GFX_STRIP_CAP) {
        l->overflow++;return rejected(l,GFX_BUILD_FULL);
    }
    for(s=s0;s<=s1;s++) {
        gfx_cmd out=*c;
        gfx_cmd *bucket=(gfx_cmd *)(void *)(l->strip_table+(s+1u)*GFX_STRIP_REGION_BYTES);
        int32_t y0=r.y0>(int32_t)(s*sh)?r.y0:(int32_t)(s*sh);
        int32_t y1=r.y1<(int32_t)((s+1u)*sh-1u)?r.y1:(int32_t)((s+1u)*sh-1u);
        if(op!=GFX_OP_NOP) {
            out.w[2]=(uint32_t)r.x0 | ((uint32_t)(y0-(int32_t)(s*sh))<<16);
            out.w[3]=(uint32_t)(r.x1-r.x0+1) | ((uint32_t)(y1-y0+1)<<16);
            if(op==GFX_OP_BLIT) out.w[1]=(uint32_t)(sx+r.x0-dx) |
                                                   ((uint32_t)(sy+y0-dy)<<16);
        }
        out.w[7]=s*GFX_STRIP_CAP+counts[s];
#ifdef INJ_STRIP_DROP
        if(s!=s0)continue;
#endif
#ifdef INJ_STRIP_CLIP
        out.w[2]++;
#endif
        bucket[counts[s]++]=out;l->used++;
    }
    return GFX_OK;
}

int gfx_strip_append(gfx_list *l,const gfx_cmd *c)
{
    const int e = gfx_cmd_check(l->t,c);
    if(e) return rejected(l,e); /* 零面积也不能跳过字段/表面检查。 */
    return gfx_strip_append_checked(l,c);
}
