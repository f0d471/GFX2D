#include "r4_cup.h"

/* Reuse the project's unchanged CC0 cup model, without the S3L rasterizer. */
typedef int32_t S3L_Unit;
typedef uint16_t S3L_Index;
#include "../../cup3d/src/cup_model.h"

int r4_cup_emit(uint32_t tick, rast_record *out, unsigned capacity)
{
    static rast_view_vertex transformed[CUP_VERTEX_COUNT];
    static fx16_mat4 projection;
    static int initialized;
    const rast_viewport vp = {160,120,320,240};
    const fx16_t c = fx_cos((uint16_t)(tick * 192u)) * 4;
    const fx16_t s = fx_sin((uint16_t)(tick * 192u)) * 4;
    const fx16_t tilt_c = 60547, tilt_s = 25080; /* Q16.16, fixed 22.5 degree tilt */
    unsigned produced = 0;
    if (!initialized) {
        if (fx16_perspective(&projection,4096,87381,65536,20*65536)) return -1;
        initialized = 1;
    }
    for (unsigned i=0;i<CUP_VERTEX_COUNT;++i) {
        fx16_t x=cupVertices[i*3]*128, y=cupVertices[i*3+1]*128;
        fx16_t z=cupVertices[i*3+2]*128;
        fx16_t rx=fx16_mul(x,c)+fx16_mul(z,s);
        fx16_t rz=fx16_mul(z,c)-fx16_mul(x,s);
        transformed[i]=(rast_view_vertex){rx,fx16_mul(y,tilt_c)-fx16_mul(rz,tilt_s),
            fx16_mul(y,tilt_s)+fx16_mul(rz,tilt_c)-4*65536,0,0,0};
    }
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
        int64_t nx=ay*bz-az*by,ny=az*bx-ax*bz,nz=ax*by-ay*bx;
        int64_t den=(nx<0?-nx:nx)+(ny<0?-ny:ny)+(nz<0?-nz:nz);
        int64_t light=nx+2*ny-3*nz;
        if(light<0)light=-light;
        uint32_t shade=den?(uint32_t)(56+199*light/(3*den)):56u;
        uint32_t color=0xff000000u|(shade<<16)|((shade*246u/255u)<<8)|(shade*230u/255u);
        for(unsigned k=0;k<3;++k)v[k].color=color;
        if (facing<0) {rast_view_vertex tmp=v[1];v[1]=v[2];v[2]=tmp;}
        int n=rast_setup_view_vp(v,3,&projection,65536,20*65536,&vp,
                                  RAST_VCOLOR_EN,out+produced,capacity-produced,0);
        if(n<0)return n;
        produced+=(unsigned)n;
    }
    return (int)produced;
}
