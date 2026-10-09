#ifndef GFX_STRIP_INTERNAL_H
#define GFX_STRIP_INTERNAL_H

/* 仅构造层内部使用：调用前已按十步语义校验记录；v2 的裁剪、反馈与容量仍在此检查。 */
int gfx_strip_append_checked(gfx_list *list, const gfx_cmd *cmd);

#endif
