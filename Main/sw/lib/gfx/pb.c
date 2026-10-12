/* PB v1 的独立字节模型和宿主执行消费者，参考契约见 gfx_abi.h。 */
#include "gfx/pb.h"
#include <string.h>
static uint32_t get32(const uint8_t *p)
{return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static void put32(uint8_t *p,uint32_t v)
{p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);p[2]=(uint8_t)(v>>16);p[3]=(uint8_t)(v>>24);}
static int si16(uint32_t v){return (int)(v&32767u)-(int)(v&32768u);}
static uint32_t pair(int x,int y){return (uint32_t)(uint16_t)x|((uint32_t)(uint16_t)y<<16);}
static uint32_t meta_end(const gfx_pb_config *c){return (512u+c->draw_cap*64u+127u)&~127u;}
static int host_overlap(const void *a,uint32_t an,const void *b,uint32_t bn)
{
    uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    return an&&bn&&(x<=y?y-x<an:x-y<bn);
}
uint32_t gfx_pb_bytes(const gfx_pb_config *c)
{
    if(!c||!c->draw_cap||c->draw_cap>32768u||c->ref_cap>131072u||!c->strips.lstrips||
       c->strips.lstrips>30u||c->strips.strip_log2<3u||c->strips.strip_log2>5u||
       (c->strips.lstrips<<c->strips.strip_log2)>480u)return 0;
    return (meta_end(c)+c->ref_cap*8u+c->strips.lstrips*16u+127u)&~127u;
}
static int fail(gfx_pb_error *e,int code,uint32_t i,uint32_t inst,uint32_t tag,uint32_t stage)
{if(e)*e=(gfx_pb_error){(uint32_t)code,i,inst,tag,stage};return code;}
static const uint8_t *window(const gfx_instance_view *v,uint32_t n,uint32_t base,uint32_t bytes)
{
    if(!v)return NULL;
    for(uint32_t i=0;i<n;i++)if(v[i].host&&base>=v[i].dev_base&&
       (uint64_t)base+bytes<=(uint64_t)v[i].dev_base+v[i].bytes)
        return v[i].host+(base-v[i].dev_base);
    return NULL;
}
static void decode(const uint8_t *p,gfx_cmd *c)
{for(unsigned w=0;w<8;w++)c->w[w]=get32(p+w*4);}
static int scalar(const gfx_cmd *p,uint32_t id,const gfx_instance_view *v,uint32_t n,gfx_cmd *out)
{
    *out=*p;
    if(gfx_op(p)!=GFX_OP_BLIT_INST)return id==UINT32_MAX?0:GFX_E_BAD_FIELD;
    if(id>=p->w[2])return GFX_E_BAD_FIELD;
    uint32_t layout=(p->w[5]>>16)&3u,step=p->w[5]&65535u;
    uint64_t address=(uint64_t)p->w[1]+(uint64_t)id*step;
    if(address>UINT32_MAX)return GFX_E_FETCH;
    const uint8_t *a=window(v,n,(uint32_t)address,layout?8u:4u);
    if(!a)return GFX_E_FETCH;
    out->w[0]=(p->w[0]&~255u)|GFX_OP_BLIT;
    out->w[1]=layout?get32(a+4):p->w[6];out->w[2]=get32(a);
    out->w[4]=0;out->w[5]=p->w[4];out->w[6]=0;
    return 0;
}
static gfx_rect clip_set(const gfx_cmd *c)
{
    int x=si16(c->w[1]),y=si16(c->w[1]>>16);
    int ex=x+(int)(c->w[3]&65535u)-1,ey=y+(int)(c->w[3]>>16)-1;
    gfx_rect r={x>0?x:0,y>0?y:0,ex<640?ex:639,ey<480?ey:479};
    if(!(c->w[3]&65535u)||!(c->w[3]>>16)||r.x0>r.x1||r.y0>r.y1)r=(gfx_rect){0,0,-1,-1};
    return r;
}
static int bounds(const gfx_surface_table *t,const gfx_cmd *c,gfx_rect clip,unsigned *lo,unsigned *hi,unsigned log2)
{
    int dx=si16(c->w[2]),dy=si16(c->w[2]>>16),w=(int)(c->w[3]&65535u),h=(int)(c->w[3]>>16);
    int x0=dx,y0=dy,x1=dx+w-1,y1=dy+h-1;
    const gfx_surf_raw *d=&t->s[gfx_dst(c)];
#define MAXTO(a,b) do{int q=(b);if((a)<q)(a)=q;}while(0)
#define MINTO(a,b) do{int q=(b);if((a)>q)(a)=q;}while(0)
    MAXTO(x0,0);MAXTO(y0,0);MAXTO(x0,clip.x0);MAXTO(y0,clip.y0);
    MINTO(x1,639);MINTO(y1,479);MINTO(x1,clip.x1);MINTO(y1,clip.y1);
    MINTO(x1,(int)(d->dim&65535u)-1);MINTO(y1,(int)(d->dim>>16)-1);
    if(gfx_op(c)==GFX_OP_BLIT){
        const gfx_surf_raw *s=&t->s[gfx_src(c)];int sx=si16(c->w[1]),sy=si16(c->w[1]>>16);
        MAXTO(x0,dx-sx);MAXTO(y0,dy-sy);
        MINTO(x1,dx-sx+(int)(s->dim&65535u)-1);MINTO(y1,dy-sy+(int)(s->dim>>16)-1);
    }
#undef MAXTO
#undef MINTO
    if(!w||!h||x0>x1||y0>y1)return 0;
    *lo=(unsigned)y0>>log2;*hi=(unsigned)y1>>log2;return 1;
}
static int packet_check(const gfx_surface_table *t,const gfx_cmd *p,const gfx_pb_config *cfg)
{
    int rc=gfx_op(p)==GFX_OP_BLIT_INST?gfx_instance_packet_check(t,p):gfx_cmd_check(t,p);
    if(rc)return rc;
    if(gfx_op(p)==GFX_OP_FILL||gfx_op(p)==GFX_OP_BLIT||gfx_op(p)==GFX_OP_BLIT_INST){
        if(gfx_dst(p)!=cfg->strips.dst_id)return GFX_E_BAD_FIELD;
        if(gfx_op(p)!=GFX_OP_FILL && gfx_surf_space(&t->s[gfx_src(p)])==0){
            const gfx_surf_raw *s=&t->s[gfx_src(p)],*d=&t->s[cfg->strips.dst_id];
            uint64_t se=(uint64_t)s->base+((uint64_t)(s->dim>>16)-1)*(1ull<<(s->ctrl&31u))+(s->dim&65535u)*4u;
            uint64_t de=(uint64_t)d->base+((uint64_t)(d->dim>>16)-1)*(1ull<<(d->ctrl&31u))+(d->dim&65535u)*4u;
            if(s->base<de&&d->base<se)return GFX_E_OVERLAP;
        }
    }
    return 0;
}
static int build_core(const gfx_surface_table *t,const gfx_cmd *input,uint32_t n,
                 const gfx_instance_view *v,uint32_t vn,const gfx_pb_config *cfg,
                 gfx_pb_workspace *pb,gfx_pb_error *e,int *started)
{
    if(e)*e=(gfx_pb_error){0,0,UINT32_MAX,0,0};
    uint32_t required=gfx_pb_bytes(cfg);
    if(!t||!pb||!pb->host||!required||pb->bytes<required||(pb->dev_base&127u)||
       (uint64_t)pb->dev_base+pb->bytes>0x100000000ull||n>cfg->draw_cap||(!input&&n)||
       host_overlap(pb->host,pb->bytes,input,n*32u))
        return fail(e,GFX_E_BAD_FIELD,0,UINT32_MAX,0,3);
    int rc=gfx_strip_config_check(t,&cfg->strips);
    if(rc)return fail(e,rc,0,UINT32_MAX,0,1);
    uint32_t counts[32]={0},off[32]={0},cursor[32]={0},used=0,m=0,span=0;
    uint32_t refoff=meta_end(cfg);gfx_rect clip={0,0,639,479};
    *started=1;
    memset(pb->host,0,required);put32(pb->host,GFX_PB_MAGIC);put32(pb->host+4,1);put32(pb->host+8,1);
    for(uint32_t i=0;i<n;i++){
        const gfx_cmd *p=&input[i];rc=packet_check(t,p,cfg);
        if(rc)return fail(e,rc,i,UINT32_MAX,p->w[7],1);
        uint32_t op=gfx_op(p);if(op==GFX_OP_END)break;
        if(op==GFX_OP_SET_CLIP)clip=clip_set(p);
        uint8_t *meta=pb->host+512u+i*64u;
        for(unsigned w=0;w<8;w++)put32(meta+w*4,p->w[w]);
        put32(meta+32,pair(clip.x0,clip.y0));put32(meta+36,pair(clip.x1,clip.y1));put32(meta+40,i);used++;
        if(op==GFX_OP_NOP||op==GFX_OP_SET_CLIP)continue;
        uint32_t items=op==GFX_OP_BLIT_INST?p->w[2]:1u;
        for(uint32_t j=0;j<items;j++){
            const uint32_t identity=op==GFX_OP_BLIT_INST?j:UINT32_MAX;
            gfx_cmd c;rc=scalar(p,op==GFX_OP_BLIT_INST?j:UINT32_MAX,v,vn,&c);
            if(rc)return fail(e,rc,i,identity,p->w[7],2);
            unsigned lo,hi;if(!bounds(t,&c,clip,&lo,&hi,cfg->strips.strip_log2))continue;
            if(hi>=cfg->strips.lstrips)return fail(e,GFX_E_BAD_FIELD,i,identity,p->w[7],3);
            for(unsigned b=lo;b<=hi;b++){
                if(counts[b]>=cfg->ref_cap)return fail(e,GFX_E_BAD_FIELD,i,identity,p->w[7],3);
                counts[b]++;
            }
        }
    }
    for(unsigned b=0;b<cfg->strips.lstrips;b++){
        off[b]=span;span+=(counts[b]*8u+15u)&~15u;m+=counts[b];
        if(m>cfg->ref_cap||refoff+span>pb->bytes)return fail(e,GFX_E_BAD_FIELD,0,UINT32_MAX,0,3);
        put32(pb->host+64u+b*8u,off[b]);put32(pb->host+68u+b*8u,counts[b]);
    }
    for(uint32_t i=0;i<used;i++){
        const uint8_t *meta=pb->host+512u+i*64u;gfx_cmd p;decode(meta,&p);
        uint32_t op=gfx_op(&p);if(op==GFX_OP_NOP||op==GFX_OP_SET_CLIP)continue;
        clip=(gfx_rect){si16(get32(meta+32)),si16(get32(meta+32)>>16),si16(get32(meta+36)),si16(get32(meta+36)>>16)};
        uint32_t items=op==GFX_OP_BLIT_INST?p.w[2]:1;
        for(uint32_t j=0;j<items;j++){
            gfx_cmd c;rc=scalar(&p,op==GFX_OP_BLIT_INST?j:UINT32_MAX,v,vn,&c);
            if(rc)return fail(e,rc,i,j,p.w[7],2);
            unsigned lo,hi;if(!bounds(t,&c,clip,&lo,&hi,cfg->strips.strip_log2))continue;
            for(unsigned b=lo;b<=hi;b++){
                uint8_t *r=pb->host+refoff+off[b]+8u*cursor[b]++;
                put32(r,i);put32(r+4,op==GFX_OP_BLIT_INST?j:UINT32_MAX);
            }
        }
    }
    for(unsigned b=0;b<cfg->strips.lstrips;b++)if(cursor[b]!=counts[b])return fail(e,GFX_E_BAD_FIELD,0,UINT32_MAX,0,3);
    put32(pb->host+12,cfg->strips.lstrips);put32(pb->host+16,used);put32(pb->host+20,m);
    put32(pb->host+24,512);put32(pb->host+28,refoff);put32(pb->host+8,2);
    return 0;
}
int gfx_pb_build(const gfx_surface_table *t,const gfx_cmd *input,uint32_t n,
                 const gfx_instance_view *v,uint32_t vn,const gfx_pb_config *cfg,
                 gfx_pb_workspace *pb,gfx_pb_error *e)
{
    gfx_pb_error local;gfx_pb_error *result=e?e:&local;
    int started=0;
    int rc=build_core(t,input,n,v,vn,cfg,pb,result,&started);
    if(rc&&started){
        put32(pb->host+8,3u);put32(pb->host+32,result->code);put32(pb->host+36,result->index);
        put32(pb->host+40,result->instance);put32(pb->host+44,result->stage);
    }
    return rc;
}
int exec_sw_run_pb(const gfx_surface_table *t,const gfx_mem *mem,const gfx_instance_view *pb,
                    const gfx_instance_view *v,uint32_t vn,const gfx_pb_config *cfg,gfx_pb_stats *stats,gfx_pb_error *e)
{
    if(e)*e=(gfx_pb_error){0,0,UINT32_MAX,0,0};
    if(stats)memset(stats,0,sizeof *stats);
    if(!t||!mem||!pb||!pb->host||!gfx_pb_bytes(cfg)||pb->bytes<gfx_pb_bytes(cfg))return fail(e,2,0,UINT32_MAX,0,5);
    const uint8_t *p=pb->host;uint32_t n=get32(p+16),m=get32(p+20),refoff=get32(p+28),sum=0;
    if(get32(p)!=GFX_PB_MAGIC||get32(p+4)!=1||get32(p+8)!=2||get32(p+12)!=cfg->strips.lstrips||
       get32(p+24)!=512||refoff!=meta_end(cfg)||n>cfg->draw_cap||m>cfg->ref_cap)return fail(e,2,0,UINT32_MAX,0,5);
    for(unsigned b=0;b<cfg->strips.lstrips;b++){
        uint32_t off=get32(p+64+b*8),cnt=get32(p+68+b*8);
        if((off&15u)||cnt>cfg->ref_cap||(uint64_t)refoff+off+(((uint64_t)cnt*8u+15u)&~15ull)>pb->bytes)
            return fail(e,9,0,UINT32_MAX,0,5);
        sum+=cnt;
    }
    if(sum!=m)return fail(e,2,0,UINT32_MAX,0,5);
    const gfx_surf_raw *bg=&t->s[cfg->strips.bg_id],*dst=&t->s[cfg->strips.dst_id];
    if(gfx_strip_config_check(t,&cfg->strips))return fail(e,4,0,UINT32_MAX,0,1);
    for(unsigned b=0;b<cfg->strips.lstrips;b++){
        for(unsigned row=b<<cfg->strips.strip_log2;row<(b+1u)<<cfg->strips.strip_log2;row++){
            uint64_t ba=(uint64_t)bg->base+((uint64_t)row<<(bg->ctrl&31u));
            uint64_t da=(uint64_t)dst->base+((uint64_t)row<<(dst->ctrl&31u));
            if(ba<mem->dev_base||da<mem->dev_base||ba+2560u>(uint64_t)mem->dev_base+mem->bytes||da+2560u>(uint64_t)mem->dev_base+mem->bytes)
                return fail(e,9,0,UINT32_MAX,0,6);
            memmove(mem->host+(size_t)(da-mem->dev_base),mem->host+(size_t)(ba-mem->dev_base),2560u);
        }
        uint32_t off=get32(p+64+b*8),cnt=get32(p+68+b*8);
        for(uint32_t r=0;r<cnt;r++){
            const uint8_t *ref=p+refoff+off+r*8u;uint32_t idx=get32(ref),inst=get32(ref+4);
            if(idx>=n)return fail(e,2,0,UINT32_MAX,0,5);
            const uint8_t *meta=p+512+idx*64u;gfx_cmd packet,c;decode(meta,&packet);
            if(get32(meta+40)!=idx)return fail(e,2,idx,inst,packet.w[7],5);
            for(unsigned w=11;w<16;w++)if(get32(meta+w*4))return fail(e,2,idx,inst,packet.w[7],5);
            int rc=packet_check(t,&packet,cfg);if(rc)return fail(e,rc,idx,UINT32_MAX,packet.w[7],5);
            if(!(gfx_op(&packet)==1||gfx_op(&packet)==2||gfx_op(&packet)==5))return fail(e,2,idx,inst,packet.w[7],5);
            rc=scalar(&packet,inst,v,vn,&c);if(rc)return fail(e,rc,idx,inst,packet.w[7],5);
            exec_sw_state es;es.clip=(gfx_rect){si16(get32(meta+32)),si16(get32(meta+32)>>16),si16(get32(meta+36)),si16(get32(meta+36)>>16)};
            if(!(es.clip.x0==0&&es.clip.y0==0&&es.clip.x1==-1&&es.clip.y1==-1)&&
               (es.clip.x0<0||es.clip.x0>es.clip.x1||es.clip.x1>639||
                es.clip.y0<0||es.clip.y0>es.clip.y1||es.clip.y1>479))return fail(e,2,idx,inst,packet.w[7],5);
            int lo=(int)(b<<cfg->strips.strip_log2),hi=(int)((b+1u)<<cfg->strips.strip_log2)-1;
            if(es.clip.y0<lo)es.clip.y0=lo;
            if(es.clip.y1>hi)es.clip.y1=hi;
            exec_sw_stats st={0};rc=exec_sw_run(&es,t,mem,&c,1,1,&st,NULL);
            if(rc)return fail(e,rc,idx,inst,packet.w[7],5);
            if(stats){stats->refs++;stats->pixels+=st.pixel_total-st.src_oob;stats->key_skip+=st.key_skip;}
        }
    }
    if(stats){stats->packets=n;for(uint32_t i=0;i<n;i++){gfx_cmd c;decode(p+512+i*64,&c);if(gfx_op(&c)==5)stats->instances+=c.w[2];}}
    return 0;
}
