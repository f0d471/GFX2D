/* v1-I 生产构造与显式数组访问。研究原型不参与链接。 */
#include "gfx/gfx.h"
#include "gfx/census.h"
#include <stddef.h>

static uint32_t pair(int32_t x, int32_t y)
{ return ((uint32_t)x & 65535u) | ((uint32_t)y << 16); }
static int coord(int32_t x) { return x >= -32768 && x <= 32767; }
static int rejected(gfx_list *l, int e)
{
    if (l) { l->rejected++; if (!l->first_error) l->first_error=e;
             if (e==GFX_BUILD_FULL) l->overflow++; }
    return e;
}
static gfx_cmd unpack(const gfx_cmd *p, uint32_t xy, uint32_t uv)
{
    gfx_cmd c=*p;
    c.w[0]=(c.w[0]&~255u)|GFX_OP_BLIT;
    c.w[1]=uv; c.w[2]=xy; c.w[4]=0; c.w[5]=p->w[4]; c.w[6]=0;
    return c;
}
int gfx_instance_packet_check(const gfx_surface_table *t, const gfx_cmd *c)
{
    uint32_t layout,stride,entry; uint64_t length,end; gfx_cmd scalar;
    if (!c || gfx_op(c)!=GFX_OP_BLIT_INST) return GFX_E_BAD_OPCODE;
    layout=(c->w[5]>>16)&3u; stride=c->w[5]&65535u;
    entry=layout==GFX_INSTANCE_XY4?4u:8u;
    if ((c->w[0]&0xE000u) || layout>1u || (c->w[5]>>18) ||
        stride<entry || stride>256u || (stride&3u) ||
        c->w[2]>GFX_INSTANCE_MAX || (c->w[1]&15u) ||
        (layout==GFX_INSTANCE_XYUV8 && c->w[6]) ||
        (!gfx_key(c) && c->w[4]) || (gfx_eq(c)!=GFX_EQ_CONST && gfx_c(c)))
        return GFX_E_BAD_FIELD;
    length=c->w[2] ? (uint64_t)(c->w[2]-1u)*stride+entry : 0u;
    end=(uint64_t)c->w[1]+((length+15u)&~(uint64_t)15u);
    if (end>0x100000000ull) return GFX_E_BAD_FIELD;
    scalar=unpack(c,0u,0u); scalar.w[3]=0u;
    return gfx_cmd_check(t,&scalar); /* 共享检查不凭空检查一个实例的重叠。 */
}
int gfx_instance_at(const gfx_cmd *c, const gfx_instance_view *views,
                    uint32_t nviews, uint32_t index, gfx_cmd *scalar)
{
    uint32_t j,entry=(c->w[5]&(1u<<16))?8u:4u;
    uint64_t addr=(uint64_t)c->w[1]+(uint64_t)index*(c->w[5]&65535u);
    if (!scalar || index>=c->w[2] || addr+entry>0x100000000ull)
        return GFX_E_FETCH;
    for (j=0;views && j<nviews;j++) {
        const gfx_instance_view *v=&views[j];
        uint64_t off=addr-v->dev_base;
        if (addr>=v->dev_base && ((addr+entry+15u)&~(uint64_t)15u)<=(uint64_t)v->dev_base+v->bytes && v->host &&
            !((uintptr_t)(v->host+(size_t)off)&3u)) {
            const uint32_t *p=(const uint32_t *)(const void *)(v->host+(size_t)off);
            uint32_t xy=p[0],uv=entry==8u?p[1]:c->w[6];
            *scalar=unpack(c,xy,uv); return GFX_OK;
        }
    }
    return GFX_E_FETCH;
}
int gfx_instances_append(gfx_list *l, const gfx_instance_desc *d,
                         const gfx_blit_pos *pos, uint32_t count,
                         gfx_instance_arena *a)
{
    uint32_t i,entry,packets,start,remaining,offset; uint64_t bytes; gfx_cmd p={0}; int e;
    if (!l || !l->t || !d || !a || (count && !pos))
        return rejected(l,GFX_BUILD_BAD_ARG);
    if(l->instances && (l->instances!=a || (l->instance_generation && l->instance_generation!=a->generation)))
        return rejected(l,GFX_BUILD_BAD_ARG);
    const gfx_blit_tmpl *s=&d->shared;
    if (d->layout>1u || s->src<0 || s->src>=16 || s->dst<0 || s->dst>=16 ||
        s->eq<0 || s->eq>15 || s->w<0 || s->w>65535 || s->h<0 || s->h>65535 ||
        (s->key_en!=0 && s->key_en!=1) || !coord(d->fixed_sx) || !coord(d->fixed_sy) ||
        (d->layout && (d->fixed_sx || d->fixed_sy)) || (a->dev_base&15u) ||
        a->used>a->bytes || (count && (!a->host || ((uintptr_t)a->host&3u))))
        return rejected(l,GFX_BUILD_BAD_ARG);
    for (i=0;i<count;i++)
        if (!coord(pos[i].dx) || !coord(pos[i].dy) || !coord(pos[i].sx) || !coord(pos[i].sy) ||
            (!d->layout && (pos[i].sx!=d->fixed_sx || pos[i].sy!=d->fixed_sy)))
            return rejected(l,GFX_BUILD_BAD_ARG);
    if (l->ended) return rejected(l,GFX_BUILD_CLOSED);
    packets=count ? count/GFX_INSTANCE_MAX+(count%GFX_INSTANCE_MAX!=0u) : 1u;
    if (l->used>l->cap || packets>l->cap-l->used || !l->buf)
        return rejected(l,GFX_BUILD_FULL);
    if (l->strip_table) return rejected(l,GFX_E_BAD_OPCODE);
    entry=d->layout?8u:4u;
    p.w[0]=GFX_OP_BLIT_INST|((uint32_t)s->eq<<8)|((uint32_t)s->key_en<<12)|
           ((uint32_t)s->src<<16)|((uint32_t)s->dst<<20)|((uint32_t)s->c<<24);
    p.w[1]=a->dev_base; p.w[3]=pair(s->w,s->h); p.w[4]=s->key;
    p.w[5]=entry|(d->layout<<16); p.w[6]=pair(d->fixed_sx,d->fixed_sy);
    e=gfx_instance_packet_check(l->t,&p); if (e) return rejected(l,e);
    /* 不同槽的源/目的 backing 不得别名，属于提交前的驱动契约。 */
    if(s->src!=s->dst && gfx_surf_space(&l->t->s[s->src])==GFX_SPACE_DDR) {
        const gfx_surf_raw *ss=&l->t->s[s->src],*ds=&l->t->s[s->dst];
        uint64_t se=(uint64_t)ss->base+((uint64_t)(gfx_surf_height(ss)-1u)<<gfx_surf_stride_log2(ss))+4u*gfx_surf_width(ss);
        uint64_t de=(uint64_t)ds->base+((uint64_t)(gfx_surf_height(ds)-1u)<<gfx_surf_stride_log2(ds))+4u*gfx_surf_width(ds);
        if(ss->base<de && ds->base<se)return rejected(l,GFX_BUILD_BAD_ARG);
    }
    /* 同槽重叠依赖坐标，完整预检保证失败前没有写入任何一颗。 */
    if (s->src==s->dst) for (i=0;i<count;i++) {
        gfx_cmd c=unpack(&p,pair(pos[i].dx,pos[i].dy),pair(pos[i].sx,pos[i].sy));
        e=gfx_cmd_check(l->t,&c); if(e) return rejected(l,e);
    }
    if (a->used>UINT32_MAX-15u) return rejected(l,GFX_BUILD_FULL);
    start=(a->used+15u)&~15u;
    bytes=(uint64_t)start+(uint64_t)count*entry+(packets-1u)*entry;
    /* 每个包独立向 16 B 取整；满包 65535 的 padding 分别为 4/8 B。 */
    bytes=(bytes+15u)&~(uint64_t)15u;
    if (count && (bytes>a->bytes || (uint64_t)a->dev_base+bytes>0x100000000ull))
        return rejected(l,GFX_BUILD_FULL);
    if (!count) {p.w[7]=l->used;l->buf[l->used++]=p;return GFX_OK;}
    remaining=count;offset=start;i=0;
    while (remaining) {
        uint32_t n=remaining>GFX_INSTANCE_MAX?GFX_INSTANCE_MAX:remaining,k;
        p.w[1]=a->dev_base+offset;p.w[2]=n;p.w[7]=l->used;
        uint32_t *words=(uint32_t *)(void *)(a->host+offset);
        for(k=0;k<n;k++,i++) {
            words[k*(entry/4u)]=pair(pos[i].dx,pos[i].dy);
            if(d->layout) words[k*2u+1u]=pair(pos[i].sx,pos[i].sy);
        }
        uint32_t end=(offset+n*entry+15u)&~15u;
        for(k=offset+n*entry;k<end;k+=4u) *(uint32_t *)(void *)(a->host+k)=0u;
        l->buf[l->used++]=p;offset=end;remaining-=n;
    }
    a->used=offset;return GFX_OK;
}
int gfx_instances_expand(const gfx_surface_table *t,const gfx_cmd *in,uint32_t count,
                          const gfx_instance_view *views,uint32_t nviews,
                          gfx_cmd *out,gfx_instance_id *ids,uint32_t cap,uint32_t *used)
{
    uint32_t pass,i,j,n=0;int e;gfx_cmd c;
    if(!used || (count && !in))return GFX_BUILD_BAD_ARG;
    for(pass=0;pass<2u;pass++) {
        n=0;
        for(i=0;i<count;i++) {
            int inst=gfx_op(&in[i])==GFX_OP_BLIT_INST;
            e=inst?gfx_instance_packet_check(t,&in[i]):gfx_cmd_check(t,&in[i]);if(e)return e;
            uint32_t num=inst?in[i].w[2]:1u;
            if(num>cap-n || (num && !out))return GFX_BUILD_FULL;
            for(j=0;j<num;j++) {
                if(inst){e=gfx_instance_at(&in[i],views,nviews,j,&c);if(e)return e;
                         e=gfx_cmd_check(t,&c);if(e)return e;}else c=in[i];
                if(pass){out[n]=c;if(ids)ids[n]=(gfx_instance_id){i,inst?j:UINT32_MAX};}
                n++;
            }
            if(gfx_op(&in[i])==GFX_OP_END)break;
        }
    }
    *used=n;return GFX_OK;
}
int exec_sw_run_instances(exec_sw_state *es,const gfx_surface_table *t,const gfx_mem *m,
                           const gfx_cmd *list,uint32_t count,uint32_t readable_count,
                           const gfx_instance_view *views,uint32_t nviews,
                           exec_sw_stats *st,exec_sw_error *err,
                           uint32_t *error_instance,uint32_t *instances)
{
    uint32_t i,j;int e;gfx_cmd c;exec_sw_error sub;
    if(err)*err=(exec_sw_error){0,0,0};
    if(error_instance)*error_instance=UINT32_MAX;
    for(i=0;i<count;i++) {
        if(st)st->records_read++;
        if(i>=readable_count || !list){e=GFX_E_FETCH;goto packet_error;}
        if(gfx_op(&list[i])!=GFX_OP_BLIT_INST) {
            if(st)st->records_read--;
            e=exec_sw_run(es,t,m,&list[i],1u,1u,st,&sub);
            if(e)goto packet_error;
            if(gfx_op(&list[i])==GFX_OP_END)return GFX_OK;
            continue;
        }
        e=gfx_instance_packet_check(t,&list[i]);if(e)goto packet_error;
        for(j=0;j<list[i].w[2];j++) {
            e=gfx_instance_at(&list[i],views,nviews,j,&c);
            if(!e)e=gfx_cmd_check(t,&c);
            if(e){if(error_instance)*error_instance=j;goto packet_error;}
            if(instances)(*instances)++;
            exec_sw_stats one={0};
            e=exec_sw_run(es,t,m,&c,1u,1u,&one,&sub);
            if(st){st->pixel_total+=one.pixel_total;st->pixel_written+=one.pixel_written;
                   st->key_skip+=one.key_skip;st->src_oob+=one.src_oob;}
            if(e){if(error_instance)*error_instance=j;goto packet_error;}
        }
        if(st)st->cmd++;
    }
    return GFX_OK;
packet_error:
    if(err)*err=(exec_sw_error){e,i,(list && i<readable_count)?list[i].w[7]:0u};
    return e;
}
static uint32_t instance_area(const gfx_rect *clip,const gfx_surf_raw *dst,
                               uint32_t xy,uint32_t w,uint32_t h)
{
    int32_t x0=gfx_s16(xy),y0=gfx_s16(xy>>16),x1=x0+(int32_t)w-1,y1=y0+(int32_t)h-1;
    if(!w||!h)return 0;
    if(x0<0)x0=0;
    if(y0<0)y0=0;
    if(x0<clip->x0)x0=clip->x0;
    if(y0<clip->y0)y0=clip->y0;
    if(x1>=GFX_SCREEN_W)x1=GFX_SCREEN_W-1;
    if(y1>=GFX_SCREEN_H)y1=GFX_SCREEN_H-1;
    if(x1>=(int32_t)gfx_surf_width(dst))x1=(int32_t)gfx_surf_width(dst)-1;
    if(y1>=(int32_t)gfx_surf_height(dst))y1=(int32_t)gfx_surf_height(dst)-1;
    if(x1>clip->x1)x1=clip->x1;
    if(y1>clip->y1)y1=clip->y1;
    return x1<x0||y1<y0?0u:(uint32_t)(x1-x0+1)*(uint32_t)(y1-y0+1);
}
int gfx_census_run_instances(gfx_census_state *cs,const gfx_surface_table *t,
                              const gfx_cmd *list,uint32_t count,
                              const gfx_instance_view *views,uint32_t nviews,gfx_census *c)
{
    uint32_t i,j;gfx_cmd scalar;
    for(i=0;i<count;i++) {
        const gfx_cmd *p=&list[i];
        if(gfx_op(p)==GFX_OP_END)return 0;
        if(gfx_op(p)!=GFX_OP_BLIT_INST) {
            if(gfx_census_run(cs,t,p,1u,c))return -1;
        } else {
            if(gfx_instance_packet_check(t,p))return -1;
            if(!p->w[2])continue;
            uint32_t stride=p->w[5]&65535u,entry=(p->w[5]&(1u<<16))?8u:4u;
            uint64_t length=((uint64_t)(p->w[2]-1u)*stride+entry+15u)&~(uint64_t)15u;
            const uint8_t *array=NULL;
            /* 一包只查一次完整 view；普查只消费目的坐标，不重复解析共享状态。 */
            for(j=0;views && j<nviews;j++) {
                const gfx_instance_view *v=&views[j];uint64_t off=(uint64_t)p->w[1]-v->dev_base;
                if(p->w[1]>=v->dev_base && off+length<=v->bytes && v->host &&
                    !((uintptr_t)(v->host+(size_t)off)&3u)) {
                    array=v->host+(size_t)off;break;
                }
            }
            if(array) {
                uint32_t eq=gfx_eq(p),w=gfx_u16(p->w[3]),h=gfx_u16(p->w[3]>>16);
                c->cmd_by_eq[eq]+=p->w[2];
                for(j=0;j<p->w[2];j++) {
                    uint32_t xy=*(const uint32_t *)(const void *)(array+(size_t)j*stride);
                    c->pix_by_eq[eq]+=instance_area(&cs->clip,&t->s[gfx_dst(p)],xy,w,h);
                }
            } else for(j=0;j<p->w[2];j++) {
                /* 部分可读 view 仍按前缀推进，保留原有失败位置。 */
                if(gfx_instance_at(p,views,nviews,j,&scalar) ||
                   gfx_census_run(cs,t,&scalar,1u,c))return -1;
            }
        }
    }
    return 0;
}
