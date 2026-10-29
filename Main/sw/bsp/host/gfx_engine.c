/* The host has no physical APB engine. Shared runtime uses capability results
 * instead of ISA conditionals; software renderer/model paths stay separate. */
#include "gfx_engine.h"
int gfx_engine_caps_read(uint32_t *caps)
{
    if(caps)*caps=0;
    return GFX_ENG_E_ABSENT;
}
int gfx_engine_tex_write(const void *src,uint32_t offset,uint32_t bytes)
{
    (void)src;(void)offset;(void)bytes;return GFX_ENG_E_ABSENT;
}
int gfx_engine_tex_read(void *dst,uint32_t offset,uint32_t bytes)
{
    (void)dst;(void)offset;(void)bytes;return GFX_ENG_E_ABSENT;
}
