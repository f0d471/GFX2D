/* 条带批量构造与逐次 gfx_blit 的差分判据，覆盖错误优先级与原子溢出。 */
#include "gfx/gfx.h"
#include <stdio.h>
#include <string.h>

static unsigned char a[GFX_STRIP_TABLE_BYTES], b[GFX_STRIP_TABLE_BYTES];
static unsigned rng = 0x47109u;
static unsigned random32(void)
{
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return rng;
}
int main(void)
{
    gfx_surface_table tab;
    gfx_surface dst = {0x200000u,640u,480u,12u,0u,0u};
    gfx_surface src = {0x100000u,64u,64u,8u,0u,0u};
    gfx_strip_config cfg = {30u,4u,0u,0u};
    unsigned scene, i;
    gfx_surface_reset(&tab);
    if(gfx_surface_set(&tab,0,&dst) || gfx_surface_set(&tab,1,&src) ||
       gfx_surface_set(&tab,2,&dst)) return 2;
    for(scene=0;scene<10000u;scene++) {
        gfx_list x,y;
        gfx_blit_pos pos[48];
        gfx_blit_tmpl tmpl = {1,0,16,16,GFX_EQ_COPY,0,1,0};
        unsigned scalar=0u, batch;
        memset(a,0xa5,sizeof a);memset(b,0xa5,sizeof b);
        if(gfx_list_begin_strips(&x,a,sizeof a,&tab,&cfg,0) ||
           gfx_list_begin_strips(&y,b,sizeof b,&tab,&cfg,0))return 3;
        for(i=0;i<48u;i++) {
            pos[i]=(gfx_blit_pos){.sx=(int)(random32()%80u)-8,.sy=(int)(random32()%80u)-8,
                                  .dx=(int)(random32()%720u)-40,.dy=(int)(random32()%560u)-40};
        }
        tmpl.eq=(int)(scene%6u);tmpl.c=tmpl.eq==GFX_EQ_CONST?128u:0u;
        if(scene%7u==0u)tmpl.src=0;       /* 源等于目的，逐条重叠检查。 */
        if(scene%11u==0u)tmpl.src=2;      /* 不同槽位别名反馈。 */
        if(scene%13u==0u)tmpl.eq=7;       /* 模板非法。 */
        if(scene%17u==0u)pos[9].dx=40000; /* 坐标不能无损编码。 */
        if(scene%19u==0u)tmpl.w=0;       /* 零面积不能跳过检查。 */
        if(scene%23u==0u) {
            ((unsigned *)(void *)a)[1]=GFX_STRIP_CAP;
            ((unsigned *)(void *)b)[1]=GFX_STRIP_CAP;
        }
        if(scene%29u==0u) {
            if(gfx_set_clip(&x,(gfx_rect){20,15,380,450}) ||
               gfx_set_clip(&y,(gfx_rect){20,15,380,450}))return 4;
        }
        if(scene%31u==0u) {gfx_list_end(&x);gfx_list_end(&y);}
        for(i=0;i<48u;i++)
            if(!gfx_blit(&x,tmpl.src,pos[i].sx,pos[i].sy,tmpl.dst,pos[i].dx,pos[i].dy,
                         tmpl.w,tmpl.h,tmpl.eq,tmpl.c,tmpl.key_en,tmpl.key))scalar++;
        batch=gfx_blit_run(&y,&tmpl,pos,48u);
        if(batch!=scalar || x.used!=y.used || x.first_error!=y.first_error ||
           x.rejected!=y.rejected || x.overflow!=y.overflow || x.ended!=y.ended ||
           memcmp(a,b,sizeof a)) {
            printf("FAIL scene=%u scalar=%u batch=%u used=%u/%u error=%d/%d\n",
                   scene,scalar,batch,x.used,y.used,x.first_error,y.first_error);return 1;
        }
    }
    puts("PASS: 10000 strip batch/scalar scenes; bytes, counts, errors, closed, alias, overflow");
    return 0;
}
