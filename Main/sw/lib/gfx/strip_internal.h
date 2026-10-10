#ifndef GFX_STRIP_INTERNAL_H
#define GFX_STRIP_INTERNAL_H

/* 仅构造层内部使用：调用前已按十步语义校验记录；v2 的裁剪、反馈与容量仍在此检查。 */
int gfx_strip_append_checked(gfx_list *list, const gfx_cmd *cmd);
int gfx_strip_plan_add(gfx_list *l,const gfx_cmd *c,const gfx_rect *r,
                       uint32_t first,uint32_t last,int32_t dx,int32_t dy,
                       int32_t sx,int32_t sy);
int gfx_strip_plan_seal(gfx_list *l);
int gfx_geometry_full(gfx_list *l,const gfx_cmd *c,gfx_rect *r);

#endif
