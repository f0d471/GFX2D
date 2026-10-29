#ifndef GFX_RASTER_SETUP_PORT_H
#define GFX_RASTER_SETUP_PORT_H
#include "gfx/rast_setup.h"
/* Keep the fixed RISC-V hot path connected to the original constant-bound
   entry points. Software canvas polymorphism belongs to the PC capability. */
static inline int gfx_setup_polygon(const rast_vertex *v,unsigned count,const rast_viewport *vp,
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

static inline int gfx_setup_view(const rast_view_vertex *v,unsigned count,const fx16_mat4 *projection,
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
