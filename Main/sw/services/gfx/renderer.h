#ifndef GFX2D_GAME_RENDERER_H
#define GFX2D_GAME_RENDERER_H

#include <stdint.h>
#include "gfx/gfx.h"
#include "gfx/census.h"
#include "gfx/frame.h"
int renderer_submit_plan(int hw,gfx_frame_plan *plan,const gfx_mem *mem,
                         exec_sw_state *es,exec_sw_error *err,exec_sw_stats *stats,
                         uint32_t *cycles,uint32_t *ticket);

#ifndef GFX2D_STRIP_LOG2
#define GFX2D_STRIP_LOG2 4
#endif

/* 后端 3 使用 ABI v2；其余后端保留直通表。cap 的单位仍为 gfx_cmd。 */
int renderer_list_begin(int hw, gfx_list *list, gfx_cmd *buf, uint32_t cap,
                         const gfx_surface_table *tab, int dst);
int renderer_list_census(const gfx_surface_table *tab, const gfx_list *list,
                          gfx_census *census);

/* 同一份命令表的两位执行者。PC 上的 hw 后端仍用软件模拟（与统计判据同口径）；
   RV32 的 hw 才会提交到引擎。 */
int renderer_prepare(int hw, const gfx_surface_table *tab);
int renderer_run(int hw, const gfx_surface_table *tab, const gfx_mem *mem,
                 exec_sw_state *es, exec_sw_error *err, const gfx_list *list,
                 exec_sw_stats *st, uint32_t *cycles);
int renderer_run_view(const gfx_surface_table *tab,const gfx_mem *mem,
                      exec_sw_state *es,exec_sw_error *err,const gfx_list *list,
                      exec_sw_stats *stats,uint32_t *cycles,unsigned width);
int renderer_perf_read(uint32_t values[9]);
int renderer_pipeline_enabled(int hw);
/* 先收尾上一张表（done_st、exec_cycles、presented 都属于它），再提交 list。
   硬件的 done_st 由引擎 PERF 增量换算，没有在途表时清零。 */
int renderer_pipeline_step(const gfx_list *list, int dst, exec_sw_stats *done_st,
                           uint32_t *exec_cycles, uint32_t *ready_time, int *presented);
int renderer_pipeline_flush(int *presented);
/* Recover physical ownership before any new surface/framebuffer use. */
int renderer_recover(void);
int renderer_compare_shadow(int dst, int shadow, const gfx_surface_table *tab,
                            const gfx_mem *mem, exec_sw_state *es,
                            exec_sw_error *err, const gfx_list *list,
                            const exec_sw_stats *expected);

#endif
