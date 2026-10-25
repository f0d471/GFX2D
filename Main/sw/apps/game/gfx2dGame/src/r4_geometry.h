#ifndef GFX2D_R4_GEOMETRY_H
#define GFX2D_R4_GEOMETRY_H

#include "gfx/rast_setup.h"
#ifndef R4_DISABLE_VERTEX_CACHE
#define R4_DISABLE_VERTEX_CACHE 0
#endif

/* Project shared, wholly near/far-inside vertices once. Returns -1 when the
   caller must use the generic clipped path. All R1 rounding sites are retained. */
int r4_project_inside(const rast_view_vertex *v, unsigned count,
                      const fx16_mat4 *projection, fx16_t near_z, fx16_t far_z,
                      const rast_viewport *vp, rast_vertex *out);

uint32_t r4_light_color(const int16_t normal[3], const int16_t light[3],
                        const int16_t highlight[3], uint32_t base);

/* Keep the fixed RISC-V hot path connected to the original constant-bound
   entry points. Software canvas polymorphism belongs to the PC capability. */
static inline int r4_setup_polygon(const rast_vertex *v,unsigned count,const rast_viewport *vp,
                                   const rast_frame *frame,uint32_t flags,rast_record *out,
                                   unsigned capacity,rast_setup_stats *stats)
{
#ifdef __riscv
    (void)frame;
    return rast_setup_polygon_vp(v,count,vp,flags,out,capacity,stats);
#else
    return rast_setup_polygon_frame(v,count,vp,frame,flags,out,capacity,stats);
#endif
}

static inline int r4_setup_view(const rast_view_vertex *v,unsigned count,const fx16_mat4 *projection,
                                fx16_t near_z,fx16_t far_z,const rast_viewport *vp,
                                const rast_frame *frame,uint32_t flags,rast_record *out,
                                unsigned capacity,rast_setup_stats *stats)
{
#ifdef __riscv
    (void)frame;
    return rast_setup_view_vp(v,count,projection,near_z,far_z,vp,flags,out,capacity,stats);
#else
    return rast_setup_view_frame(v,count,projection,near_z,far_z,vp,frame,flags,out,capacity,stats);
#endif
}

#endif
