#ifndef GFX_PB_H
#define GFX_PB_H
#include "gfx/gfx.h"
#define GFX_PB_MAGIC 0x31425046u
#define GFX_PB_DRAW_DEFAULT 1024u
#define GFX_PB_REF_DEFAULT 8192u
#ifndef GFX_PB_DRAW_CAP
#define GFX_PB_DRAW_CAP GFX_PB_DRAW_DEFAULT
#endif
#ifndef GFX_PB_REF_CAP
#define GFX_PB_REF_CAP GFX_PB_REF_DEFAULT
#endif
typedef struct gfx_pb_config {
    gfx_strip_config strips;
    uint32_t draw_cap,ref_cap;
} gfx_pb_config;
typedef struct gfx_pb_workspace {
    uint8_t *host;
    uint32_t dev_base,bytes,generation;
} gfx_pb_workspace;
typedef struct {
    uint32_t code,index,instance,tag,stage;
} gfx_pb_error;
typedef struct {
    uint32_t packets,instances,refs,pixels,key_skip;
} gfx_pb_stats;
/* 返回按 128 B 对齐的完整租用长度；非法参数返回 0。 */
uint32_t gfx_pb_bytes(const gfx_pb_config *cfg);
/* 独立 CPU 参考构建，不在硬件生产帧的构造阶段调用。 */
int gfx_pb_build(const gfx_surface_table *tab,const gfx_cmd *input,uint32_t count,
                 const gfx_instance_view *views,uint32_t view_count,
                 const gfx_pb_config *cfg,gfx_pb_workspace *pb,gfx_pb_error *error);
/* PB/实例只通过显式只读窗口解析，执行状态不回写旧直通 clip。 */
int exec_sw_run_pb(const gfx_surface_table *tab,const gfx_mem *mem,
                    const gfx_instance_view *pb,const gfx_instance_view *views,uint32_t view_count,
                    const gfx_pb_config *cfg,gfx_pb_stats *stats,gfx_pb_error *error);
#endif
