#ifndef GFX_RESOURCE_POOL_H
#define GFX_RESOURCE_POOL_H
#include "gfx/gfx.h"
/* Trusted graphics service; clients of the public platform API never see it. */
int gfx_resource_bind(gfx_mem *mem,gfx_cmd *commands[2],uint32_t capacities[2]);
#endif
