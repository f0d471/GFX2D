#ifndef GFX_FRAME_H
#define GFX_FRAME_H
#include "gfx/gfx.h"
#define GFX_FRAME_MAX_PASSES 3u
#define GFX_FRAME_MAX_LEASES 12u
#define GFX_PASS_NO_DEP UINT32_MAX
/* arena 的页在初始化时登记，后续只经 begin/release 修改 owner。 */
typedef struct {uint32_t base,bytes,generation,owner;} gfx_lease;
typedef struct {gfx_lease page[GFX_FRAME_MAX_LEASES];uint32_t count,next_ticket;} gfx_frame_arena;
typedef struct {uint32_t base,bytes,generation,lease;} gfx_pass_resource;
typedef struct {
    gfx_surface_table surfaces;
    const gfx_list *draw;
    gfx_pass_resource target,initial,reads[4],writes[4];
    uint32_t read_count,write_count,dependency,completion_ticket,elapsed_cycles;
    exec_sw_stats executed;
    uint32_t perf[9];
    int load,initialized;
} gfx_pass;
typedef struct gfx_frame_plan {
    gfx_frame_arena *arena;
    gfx_pass pass[GFX_FRAME_MAX_PASSES];
    uint32_t frame_id,mask,ticket,count,completed;
    int first_error,sealed,published,drained;
} gfx_frame_plan;
int gfx_frame_arena_add(gfx_frame_arena *a,uint32_t base,uint32_t bytes,uint32_t generation);
int gfx_frame_begin(gfx_frame_plan *p,gfx_frame_arena *a,uint32_t frame_id,uint32_t leases);
int gfx_frame_add_pass(gfx_frame_plan *p,const gfx_pass *pass);
int gfx_frame_seal(gfx_frame_plan *p);
/* 提交与完成由渲染器驱动；失败提交必须先排空才能 release。 */
int gfx_frame_publish(gfx_frame_plan *p,uint32_t *ticket);
int gfx_frame_complete(gfx_frame_plan *p,uint32_t pass_id,int error,int drained);
int gfx_frame_release(gfx_frame_plan *p,uint32_t ticket);
#endif
