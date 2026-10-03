#ifndef GFX2D_GAME_RENDERER_H
#define GFX2D_GAME_RENDERER_H

#include <stdint.h>
#include "gfx/gfx.h"

/* 同一份命令表的两位执行者。PC 的 hw 标签沿 SW8 判据仍用软件模拟；
   RV32 的 hw 才会提交到 B6 引擎。 */
int renderer_prepare(int hw, const gfx_surface_table *tab);
int renderer_run(int hw, const gfx_surface_table *tab, const gfx_mem *mem,
                 exec_sw_state *es, exec_sw_error *err, const gfx_list *list,
                 exec_sw_stats *st, uint32_t *cycles);
int renderer_perf_read(uint32_t values[9]);
int renderer_pipeline_enabled(int hw);
int renderer_pipeline_step(const gfx_list *list, int dst, uint32_t *exec_cycles,
                           uint32_t *ready_time, int *presented);
int renderer_pipeline_flush(int *presented);
int renderer_compare_shadow(int dst, int shadow, const gfx_surface_table *tab,
                            const gfx_mem *mem, exec_sw_state *es,
                            exec_sw_error *err, const gfx_list *list);

#endif
