/******************************************************************************
* @file renderer.c: 同一张命令表的软件与 B6 硬件执行入口
******************************************************************************/

#include "renderer.h"
#include "board.h"
#include "stg_main.h"
#include "core/format.h"

#ifdef __riscv
#include "gfx_engine.h"
static int s_pending;
static int s_pending_dst;
static uint32_t s_pending_event, s_pending_start;

static void engine_error(int code, uint32_t index, uint32_t tag)
{
    char buf[96];
    core_out out;
    core_out_init(&out, buf, (uint32_t)sizeof buf);
    core_out_str(&out, "engine err code ");
    core_out_u32(&out, (uint32_t)code);
    core_out_str(&out, " index ");
    core_out_u32(&out, index);
    core_out_str(&out, " tag ");
    core_out_u32(&out, tag);
    core_out_str(&out, "\r\n");
    board_puts(buf);
}
#endif

int renderer_pipeline_enabled(int hw)
{
#if defined(__riscv) && SW9_PERF
    return hw;
#else
    (void)hw;
    return 0;
#endif
}

#ifdef __riscv
static int pending_finish(uint32_t *cycles, int *presented, uint32_t *ready_time)
{
    uint32_t code = 0u, index = 0u, tag = 0u, ticket;
    int rc;

    *presented = -1;
    if (!s_pending) {
        *cycles = 0u;
        *ready_time = board_cycles();
        return 0;
    }
    while (board_irq_engine_events() == s_pending_event) {
        if ((uint32_t)(board_cycles() - s_pending_start) > BOARD_HZ) {
            *ready_time = board_cycles();
            *cycles = *ready_time - s_pending_start;
            engine_error(-1, 0u, 0u);
            return -1;
        }
    }
    rc = gfx_engine_wait(&code, &index, &tag);
    if (rc != GFX_ENG_OK || code != 0u) {
        *ready_time = board_cycles();
        *cycles = *ready_time - s_pending_start;
        engine_error((rc != GFX_ENG_OK) ? rc : (int)code, index, tag);
        return -1;
    }
    *ready_time = board_cycles();  /* 此刻以前是 CPU 与引擎并行的工作，不含等场 */
    *cycles = *ready_time - s_pending_start;
    if (board_present_request(s_pending_dst, &ticket) != 0 ||
        board_present_wait(ticket) != 0)
        return -1;
    *presented = s_pending_dst;
    s_pending = 0;
    return 0;
}
#endif

int renderer_pipeline_step(const gfx_list *list, int dst, uint32_t *exec_cycles,
                           uint32_t *ready_time, int *presented)
{
#ifdef __riscv
    if (pending_finish(exec_cycles, presented, ready_time) != 0)
        return -1;
    s_pending_event = board_irq_engine_events();
    s_pending_start = board_cycles();
    if (gfx_engine_submit((uint32_t)(uintptr_t)list->buf, list->used) != GFX_ENG_OK) {
        engine_error(-2, 0u, 0u);
        return -1;
    }
    s_pending_dst = dst;
    s_pending = 1;
    return 0;
#else
    (void)list; (void)dst; (void)exec_cycles; (void)ready_time; (void)presented;
    return -1;
#endif
}

int renderer_pipeline_flush(int *presented)
{
#ifdef __riscv
    uint32_t cycles, ready;
    return pending_finish(&cycles, presented, &ready);
#else
    *presented = -1;
    return 0;
#endif
}

int renderer_prepare(int hw, const gfx_surface_table *tab)
{
#ifdef __riscv
    uint32_t i;

    if (!hw)
        return 0;
    if (s_pending)
        return -1;             /* 切换前必须显式 flush，不能复位仍在途的命令表 */
    if (gfx_engine_soft_reset() != GFX_ENG_OK)
        return -1;
    for (i = 0u; i < GFX_SURF_N; i++) {
        const gfx_surf_raw *s = &tab->s[i];
        if ((s->ctrl & 0x80000000u) == 0u)
            continue;
        if (gfx_engine_surf_set((int)i, s->base, s->dim & 0xFFFFu,
                                s->dim >> 16, s->ctrl & 0x1Fu,
                                (s->ctrl >> 12) & 0xFu) != GFX_ENG_OK)
            return -1;
    }
    board_engine_irq_enable();
#else
    (void)hw;
    (void)tab;
#endif
    return 0;
}

int renderer_run(int hw, const gfx_surface_table *tab, const gfx_mem *mem,
                 exec_sw_state *es, exec_sw_error *err, const gfx_list *list,
                 exec_sw_stats *st, uint32_t *cycles)
{
    const uint32_t t0 = board_cycles();
    int rc;

#ifdef __riscv
    if (hw) {
        const uint32_t event0 = board_irq_engine_events();
        uint32_t code = 0u, index = 0u, tag = 0u;

        exec_sw_stats_clear(st);
        rc = gfx_engine_submit((uint32_t)(uintptr_t)list->buf, list->used);
        if (rc != GFX_ENG_OK)
            goto hw_fail;
        while (board_irq_engine_events() == event0) {
            if ((uint32_t)(board_cycles() - t0) > BOARD_HZ) {
                rc = GFX_ENG_E_TIMEOUT;
                goto hw_fail;
            }
        }
        rc = gfx_engine_wait(&code, &index, &tag);
        if (rc != GFX_ENG_OK)
            goto hw_fail;
        err->code = (int)code;
        err->index = index;
        err->tag = tag;
        if (code != 0u)
            engine_error((int)code, index, tag);
        *cycles = board_cycles() - t0;
        return (int)code;
hw_fail:
        err->code = rc;
        err->index = 0u;
        err->tag = 0u;
        engine_error(rc, 0u, 0u);
        *cycles = board_cycles() - t0;
        return rc;
    }
#else
    (void)hw;
#endif
    exec_sw_stats_clear(st);
    rc = exec_sw_run(es, tab, mem, list->buf, list->used, list->used, st, err);
    *cycles = board_cycles() - t0;
    return rc;
}

int renderer_perf_read(uint32_t values[9])
{
#ifdef __riscv
    return gfx_engine_perf_read(values, 9u);
#else
    uint32_t i;
    for (i = 0u; i < 9u; i++)
        values[i] = 0u;
    return 0;
#endif
}

/* 诊断路径：软件按原表画 dst；硬件临时将同一槽映射到另一面执行同一命令表。 */
int renderer_compare_shadow(int dst, int shadow, const gfx_surface_table *tab,
                            const gfx_mem *mem, exec_sw_state *es,
                            exec_sw_error *err, const gfx_list *list)
{
#ifdef __riscv
    const gfx_surf_raw *orig = &tab->s[dst];
    const gfx_surf_raw *other = &tab->s[shadow];
    volatile uint32_t *probe;
    uint32_t cycles;
    exec_sw_stats st;
    int rc, restore;

    if (gfx_engine_surf_set(dst, other->base, gfx_surf_width(orig),
                            gfx_surf_height(orig), gfx_surf_stride_log2(orig),
                            gfx_surf_format(orig)) != GFX_ENG_OK)
        return -1;
    board_dcache_invalidate_all();
    probe = (volatile uint32_t *)board_fb(shadow);
    *probe = 0xDEADBEEFu; /* 故意留下旧缓存行；整帧 COPY 会覆盖 DDR 中这个哨兵 */
    (void)*probe;         /* 本核写缺失不分配缓存行，必须再读一次装入 */
    rc = renderer_run(1, tab, mem, es, err, list, &st, &cycles);
    restore = gfx_engine_surf_set(dst, orig->base, gfx_surf_width(orig),
                                   gfx_surf_height(orig), gfx_surf_stride_log2(orig),
                                   gfx_surf_format(orig));
    if (rc != GFX_OK || restore != GFX_ENG_OK)
        return -1;
#ifndef INJECT_SW9_SKIP_INVALIDATE
    board_dcache_invalidate_all();
#endif
    return 0;
#else
    (void)dst; (void)shadow; (void)tab; (void)mem;
    (void)es; (void)err; (void)list;
    return -1;
#endif
}
