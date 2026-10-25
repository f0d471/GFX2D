#include "r4_cup.h"
#include "r4_geometry.h"
#include "r4_models.h"

/* Reuse the project's unchanged CC0 cup model, without the S3L rasterizer. */
typedef int32_t S3L_Unit;
typedef uint16_t S3L_Index;
#include "../../cup3d/src/cup_model.h"

int r4_cup_emit(uint32_t tick, rast_record *out, unsigned capacity)
{
    const rast_viewport vp = {160,120,320,240};
    return r4_cup_emit_view(tick,&vp,out,capacity);
}

int r4_cup_emit_view(uint32_t tick, const rast_viewport *viewport,
                     rast_record *out, unsigned capacity)
{
    static rast_view_vertex transformed[CUP_VERTEX_COUNT];
    static rast_vertex screen[CUP_VERTEX_COUNT];
    static fx16_mat4 projection;
    static unsigned projection_w,projection_h;
    if(!viewport || !viewport->w || !viewport->h)return -1;
    const rast_viewport vp = *viewport;
    const rast_frame frame = {(unsigned)(vp.x+vp.w)>RAST_WIDTH?(unsigned)(vp.x+vp.w):RAST_WIDTH,
                              RAST_HEIGHT};
    const fx16_t c = fx_cos((uint16_t)(tick * 192u)) * 4;
    const fx16_t s = fx_sin((uint16_t)(tick * 192u)) * 4;
    const fx16_t tilt_c = 60547, tilt_s = 25080; /* Q16.16, fixed 22.5 degree tilt */
    unsigned produced = 0;
    const int16_t light_view[3]={-7600,10800,-9600};
    const int16_t half_view[3]={-4200,6400,-14500};
    int16_t light[3],half[3];
    if (projection_w!=vp.w || projection_h!=vp.h) {
        fx16_t aspect=fx16_div((fx16_t)vp.w*FX16_ONE,(fx16_t)vp.h*FX16_ONE);
        if (fx16_perspective(&projection,4096,aspect,65536,20*65536)) return -1;
        projection_w=vp.w;projection_h=vp.h;
    }
    for(unsigned k=0;k<2;++k) {
        const int16_t *source=k?half_view:light_view;
        int16_t *dest=k?half:light;
        fx16_t y=fx16_mul(source[1]*4,tilt_c)+fx16_mul(source[2]*4,tilt_s);
        fx16_t z=fx16_mul(source[2]*4,tilt_c)-fx16_mul(source[1]*4,tilt_s);
        dest[0]=(int16_t)((fx16_mul(source[0]*4,c)-fx16_mul(z,s))/4);
        dest[1]=(int16_t)(y/4);
        dest[2]=(int16_t)((fx16_mul(source[0]*4,s)+fx16_mul(z,c))/4);
    }
    for (unsigned i=0;i<CUP_VERTEX_COUNT;++i) {
        fx16_t x=cupVertices[i*3]*128, y=cupVertices[i*3+1]*128;
        fx16_t z=cupVertices[i*3+2]*128;
        fx16_t rx=fx16_mul(x,c)+fx16_mul(z,s);
        fx16_t rz=fx16_mul(z,c)-fx16_mul(x,s);
        transformed[i]=(rast_view_vertex){rx,fx16_mul(y,tilt_c)-fx16_mul(rz,tilt_s),
            fx16_mul(y,tilt_s)+fx16_mul(rz,tilt_c)-4*65536,0,0,
            r4_light_color(r4_cup_normals[i],light,half,0x00c1d9e4)};
    }
    int cached=!R4_DISABLE_VERTEX_CACHE &&
        !r4_project_inside(transformed,CUP_VERTEX_COUNT,&projection,65536,20*65536,&vp,screen);
    for (unsigned i=0;i<CUP_TRIANGLE_COUNT;++i) {
        rast_view_vertex v[3];
        for (unsigned k=0;k<3;++k) {
            v[k]=transformed[cupTriangles[i*3+k]];
        }
        /* Two-sided cup, including inner walls. Orient each view-space face
           for the R1 top-left rasterizer; both executors receive identical records. */
        int64_t ax=(int64_t)v[1].x-v[0].x, ay=(int64_t)v[1].y-v[0].y;
        int64_t az=(int64_t)v[1].z-v[0].z, bx=(int64_t)v[2].x-v[0].x;
        int64_t by=(int64_t)v[2].y-v[0].y, bz=(int64_t)v[2].z-v[0].z;
        int64_t facing=(ay*bz-az*by)*v[0].x+(az*bx-ax*bz)*v[0].y+(ax*by-ay*bx)*v[0].z;
        if (facing<0) {rast_view_vertex tmp=v[1];v[1]=v[2];v[2]=tmp;}
        int n;
        if(cached) {
            rast_vertex tri[3];
            for(unsigned k=0;k<3;++k)tri[k]=screen[cupTriangles[i*3+k]];
            if(facing<0){rast_vertex tmp=tri[1];tri[1]=tri[2];tri[2]=tmp;}
            n=r4_setup_polygon(tri,3,&vp,&frame,RAST_VCOLOR_EN,out+produced,capacity-produced,0);
        } else n=r4_setup_view(v,3,&projection,65536,20*65536,&vp,&frame,
                                  RAST_VCOLOR_EN,out+produced,capacity-produced,0);
        if(n<0)return n;
        produced+=(unsigned)n;
    }
    return (int)produced;
}
