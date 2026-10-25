#include <limits.h>
#include "r4_geometry.h"

static int dot_positive(const int16_t a[3], const int16_t b[3])
{
    int32_t v = ((int32_t)a[0]*b[0] + (int32_t)a[1]*b[1] + (int32_t)a[2]*b[2])/16384;
    return v < 0 ? 0 : v > 16384 ? 16384 : v;
}

uint32_t r4_light_color(const int16_t normal[3], const int16_t light[3],
                        const int16_t highlight[3], uint32_t base)
{
    unsigned diffuse = 58u + (unsigned)dot_positive(normal,light)*170u/16384u;
    unsigned spec = (unsigned)dot_positive(normal,highlight);
    uint32_t color = 0xff000000u;
    spec=spec*spec/16384u;spec=spec*spec/16384u;spec=spec*spec/16384u;
    spec=spec*80u/16384u;
    for(unsigned k=0;k<3;++k) {
        unsigned shift=k*8u,channel=((base>>shift)&255u)*diffuse/255u+spec;
        color|=(channel>255u?255u:channel)<<shift;
    }
    return color;
}

/* The Q40 reciprocal is rounded down once per shared positive denominator.
   For n<=2^36 its truncation error in the quotient is <1/16. A single exact
   remainder repair recovers floor(n/d), then R1's sign-aware half rule applies.
   All inputs outside this proven product bound retain the generic divider. */
static fx16_t projected_div(fx16_t a,fx16_t d,uint32_t reciprocal)
{
    uint64_t n=(uint64_t)(a<0?-(int64_t)a:a)*65536u;
    if(!reciprocal || n>(UINT64_C(1)<<36))return fx16_div(a,d);
    uint64_t q=n*reciprocal>>40;
    uint64_t remainder=n-q*(uint32_t)d;
    if(remainder>=(uint32_t)d){++q;remainder-=(uint32_t)d;}
    q+=a<0?remainder>(uint32_t)d/2u:remainder>=((uint32_t)d+1u)/2u;
    return (fx16_t)(a<0?-(int64_t)q:(int64_t)q);
}

static fx16_t mul_term(fx16_t a,fx16_t b)
{
    return !a||!b?0:a==FX16_ONE?b:b==FX16_ONE?a:
        a== -FX16_ONE?fx16_sat(-(int64_t)b):fx16_mul(a,b);
}

int r4_project_inside(const rast_view_vertex *v, unsigned count,
                      const fx16_mat4 *p, fx16_t near_z, fx16_t far_z,
                      const rast_viewport *vp, rast_vertex *out)
{
    int sparse = !p->v[1] && !p->v[4] && !p->v[8] && !p->v[9] &&
                 !p->v[12] && !p->v[13];
    int32_t previous_z = INT32_MAX, depth = 0, clip_w = 0;
    uint32_t reciprocal=0;
    fx16_t cache_x[2]={0},cache_y[2]={0},value_x[2]={0},value_y[2]={0};
    unsigned count_x=0,count_y=0;
    for (unsigned i = 0; i < count; ++i) {
        fx16_vec4 clip;
        int64_t x, y;
        if (v[i].z > -near_z || v[i].z < -far_z) return -1;
        if (sparse) {
            clip.v[0] = fx16_sat((int64_t)mul_term(p->v[0], v[i].x) +
                                 mul_term(p->v[2], v[i].z) + p->v[3]);
            clip.v[1] = fx16_sat((int64_t)mul_term(p->v[5], v[i].y) +
                                 mul_term(p->v[6], v[i].z) + p->v[7]);
            if (v[i].z != previous_z) {
                clip.v[2] = fx16_sat((int64_t)mul_term(p->v[10], v[i].z) + p->v[11]);
                clip_w = fx16_sat((int64_t)mul_term(p->v[14], v[i].z) + p->v[15]);
                reciprocal=0;count_x=count_y=0;
                if(clip_w>=8192) {
                    reciprocal=(uint32_t)fx16_div(16777216,clip_w);
                    if((uint64_t)reciprocal*(uint32_t)clip_w>(UINT64_C(1)<<40))--reciprocal;
                }
            }
            clip.v[3] = clip_w;
        } else {
            fx16_vec4 in = {{v[i].x, v[i].y, v[i].z, FX16_ONE}};
            fx16_mat_vec(p, &in, &clip);
        }
        if (clip.v[3] <= 0) return -1;
        if (!sparse || v[i].z != previous_z) {
            int64_t z = fx16_round_div(((int64_t)fx16_div(clip.v[2], clip.v[3]) +
                                        FX16_ONE) * 65535, 2 * FX16_ONE);
            depth = (int32_t)(z < 0 ? 0 : z > 65535 ? 65535 : z);
        }
        previous_z = v[i].z;
        fx16_t ndc_x,ndc_y;
        unsigned ix=0,iy=0;
        if(sparse) {
            while(ix<count_x && cache_x[ix]!=clip.v[0])++ix;
            while(iy<count_y && cache_y[iy]!=clip.v[1])++iy;
        }
        if(sparse && ix<count_x)ndc_x=value_x[ix];
        else {
            ndc_x=projected_div(clip.v[0],clip.v[3],sparse?reciprocal:0);
            if(sparse && count_x<2){cache_x[count_x]=clip.v[0];value_x[count_x++]=ndc_x;}
        }
        if(sparse && iy<count_y)ndc_y=value_y[iy];
        else {
            ndc_y=projected_div(clip.v[1],clip.v[3],sparse?reciprocal:0);
            if(sparse && count_y<2){cache_y[count_y]=clip.v[1];value_y[count_y++]=ndc_y;}
        }
        x = (int64_t)vp->x * 16 + fx16_viewport(ndc_x, (int32_t)vp->w, 0);
        y = (int64_t)vp->y * 16 + fx16_viewport(ndc_y, (int32_t)vp->h, 1);
        if (x < -(1 << 28) || x > (1 << 28) || y < -(1 << 28) || y > (1 << 28)) return -1;
        out[i] = (rast_vertex){(int32_t)x, (int32_t)y, (uint16_t)depth, v[i].u, v[i].v, v[i].color};
    }
    return 0;
}
