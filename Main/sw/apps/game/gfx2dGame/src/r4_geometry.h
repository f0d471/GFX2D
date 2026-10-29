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

#include "raster_setup_port.h"
#endif
