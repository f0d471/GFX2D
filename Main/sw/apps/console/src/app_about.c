/******************************************************************************
* @file app_about.c: 系统信息页（SW10）
*
* @brief  游戏机的「关于本机」：版本、内核 tick 与堆、任务数、场/交换/中断
*         计数、引擎能力位。一屏只读信息，SW 执行器直绘，BOMB/HOME 退出。
******************************************************************************/

#include <stddef.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

#include "board.h"

#include "core/format.h"

#include "gfx/gfx.h"

#ifdef __riscv
#include "gfx_engine.h"
#endif

#include "assets.h"
#include "stg_slots.h"
#include "game/stg_text.h"

#include "renderer.h"
#include "rtos_irq.h"

#include "appfw.h"

#define ABOUT_BG   0xFF0A0E16u
#define ABOUT_X    120
#define ABOUT_Y0   120
#define ABOUT_DY   28

static const stg_font s_font = {
    STG_SLOT_FONT, ASSET_FONT8X16_CELL_W, ASSET_FONT8X16_CELL_H,
    ASSET_FONT8X16_COLS, 0x20u, ASSET_FONT8X16_CELLS, 0x00000000u,
};
static const stg_font s_font_cyan = {
    STG_SLOT_FONT_CYAN, ASSET_FONT_CYAN_CELL_W, ASSET_FONT_CYAN_CELL_H,
    ASSET_FONT_CYAN_COLS, 0x20u, ASSET_FONT_CYAN_CELLS, ASSET_FONT_CYAN_KEY,
};

static int s_hw;      /* 与菜单同一策略：引擎渲染，无引擎退 SW */

static int about_enter(appfw_ctx *c)
{
    if (stg_runtime_surfaces_stg(c->rt) != 0)
        return -1;
    s_hw = (renderer_prepare(1, &c->rt->tab) == 0) ? 1 : 0;
    return 0;
}

static void about_leave(appfw_ctx *c)
{
    (void)c;
}

static void line(appfw_ctx *c, gfx_list *l, int dst, uint32_t row,
                 const char *label, const char *value)
{
    (void)stg_text_draw(l, &s_font_cyan, dst, ABOUT_X,
                        ABOUT_Y0 + (int)(row * ABOUT_DY), label, NULL);
    (void)stg_text_draw(l, &s_font, dst, ABOUT_X + 220,
                        ABOUT_Y0 + (int)(row * ABOUT_DY), value, NULL);
    (void)c;
}

static void about_frame(appfw_ctx *c, uint32_t keys, uint32_t frame)
{
    const uint32_t li  = c->seq & 1u;
    const int      dst = li ? STG_SLOT_FB1 : STG_SLOT_FB0;
    gfx_list       l;
    gfx_rect       all;
    char           v[5][32];
    core_out       o;
    uint32_t       i;
    exec_sw_stats  st;
    uint32_t       cyc, ticket;

    (void)keys;
    (void)frame;

    all.x0 = 0;
    all.y0 = 0;
    all.x1 = (int32_t)FB_WIDTH - 1;
    all.y1 = (int32_t)FB_HEIGHT - 1;

    gfx_list_begin(&l, c->rt->cmds[li], c->rt->cap[li], &c->rt->tab);
    (void)gfx_set_clip(&l, all);
    (void)gfx_fill(&l, dst, all, ABOUT_BG, GFX_EQ_COPY, 0u);
    (void)stg_text_draw(&l, &s_font_cyan, dst, ABOUT_X, 64, "SYSTEM", NULL);

    for (i = 0u; i < 5u; i++)
        v[i][0] = '\0';

    core_out_init(&o, v[0], (uint32_t)sizeof v[0]);
    core_out_u32(&o, (uint32_t)xTaskGetTickCount());
    line(c, &l, dst, 0, "UPTIME MS", v[0]);

    core_out_init(&o, v[1], (uint32_t)sizeof v[1]);
    core_out_u32(&o, (uint32_t)xPortGetFreeHeapSize());
    line(c, &l, dst, 1, "HEAP FREE", v[1]);

    core_out_init(&o, v[2], (uint32_t)sizeof v[2]);
    core_out_u32(&o, (uint32_t)uxTaskGetNumberOfTasks());
    line(c, &l, dst, 2, "TASKS", v[2]);

    core_out_init(&o, v[3], (uint32_t)sizeof v[3]);
    core_out_u32(&o, board_frame_count());
    core_out_ch(&o, '/');
    core_out_u32(&o, board_swap_count());
    line(c, &l, dst, 3, "FIELD/SWAP", v[3]);

    core_out_init(&o, v[4], (uint32_t)sizeof v[4]);
    core_out_u32(&o, board_irq_frame_events());
    core_out_ch(&o, '/');
    core_out_u32(&o, board_irq_engine_events());
    core_out_ch(&o, '/');
    core_out_u32(&o, board_irq_faults());
    line(c, &l, dst, 4, "IRQ A/B/FAULT", v[4]);

#ifdef __riscv
    {
        uint32_t caps = 0u;

        if (gfx_engine_caps_read(&caps) == GFX_ENG_OK) {
            core_out_init(&o, v[0], (uint32_t)sizeof v[0]);
            core_out_hex32(&o, caps);
            line(c, &l, dst, 5, "ENGINE CAPS", v[0]);
        }
    }
#endif

    (void)stg_text_draw(&l, &s_font, dst, ABOUT_X, 420,
                        "GFX2D CONSOLE  FREERTOS V202212.01  SW10", NULL);
    (void)gfx_list_end(&l);

    if (renderer_run(s_hw, &c->rt->tab, &c->rt->mem, &c->rt->es, &c->rt->err,
                     &l, &st, &cyc) == GFX_OK
        && board_present_request(dst, &ticket) == 0) {
        /* 与菜单同一节奏：睡帧信号量让出 CPU，醒来确认交换 */
        rtos_wait_frame(50);
        if (!board_present_finished(ticket))
            (void)board_present_wait(ticket);
        c->shown = dst;
    }
    c->seq++;
}

const appfw_app g_app_about = {
    "about", "system info",
    about_enter,
    about_leave,
    about_frame,
};
