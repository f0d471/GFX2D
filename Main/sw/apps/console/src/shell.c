/******************************************************************************
* @file shell.c: 菜单应用（SW10）
*
* @brief  画面：深色底 + 标题图（premult ALPHA，同游戏标题的画法）+ 纵向列表
*         （选中行青字库 + 光标小飞船，未选中白字库）+ 页脚键位提示 + 版本/fps。
*         素材全部来自同一份 pak（FONT/FONT_CYAN/TITLE/SHIPS），表面槽用 stg
*         那一套（stg_runtime_surfaces_stg 一次登记全）。
*
*         键位（只在菜单内）：上下移动光标、BOMB 启动选中项、
*         MODE1/2/3 直达第 1~3 项（按下沿触发）。
*         绘制走 SW 执行器：菜单不依赖引擎状态，引擎留给应用用。
*         第一帧打一行 `menu crc <hex> sel <n>`：板上判据的渲染锚点。
******************************************************************************/

#include <stddef.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

#include "board.h"

#include "core/crc32.h"
#include "core/format.h"

#include "gfx/gfx.h"

#include "assets.h"
#include "stg_slots.h"
#include "game/stg_text.h"

#include "renderer.h"
#include "stg_modes.h"

#include "rtos_irq.h"

#include "appfw.h"
#include "shell.h"

/* 与游戏一致的字体几何（sw6_hud 的那套；青字库同规格） */
static const stg_font s_font = {
    STG_SLOT_FONT, ASSET_FONT8X16_CELL_W, ASSET_FONT8X16_CELL_H,
    ASSET_FONT8X16_COLS, 0x20u, ASSET_FONT8X16_CELLS, 0x00000000u,
};
static const stg_font s_font_cyan = {
    STG_SLOT_FONT_CYAN, ASSET_FONT_CYAN_CELL_W, ASSET_FONT_CYAN_CELL_H,
    ASSET_FONT_CYAN_COLS, 0x20u, ASSET_FONT_CYAN_CELLS, ASSET_FONT_CYAN_KEY,
};

#define MENU_BG      0xFF100E1Au      /* ARGB：近黑深蓝 */
#define MENU_Y0      216
#define MENU_STEP   36
#define CURSOR_X    196
#define NAME_X      244
#define DESC_X      420
#define FOOT_Y      440

static uint32_t s_sel;             /* 光标条目 */
static uint32_t s_prev_keys;
static uint32_t s_fps_t0, s_fps_s0, s_fps;
static uint32_t s_cyc_build, s_cyc_exec, s_cyc_present;   /* 最近一帧的三段 */
static int      s_first_logged;
static int      s_hw;              /* 1=引擎渲染，0=SW 兜底（引擎不在的老位流） */

static int shell_enter(appfw_ctx *c)
{
    if (stg_runtime_surfaces_stg(c->rt) != 0)
        return -1;
    /* 整屏菜单交引擎渲染（CPU 的 SW 光栅在 100MHz 上只有 ~3fps）；
       引擎不在时退 SW，菜单仍可用只是慢 */
    s_hw = (renderer_prepare(1, &c->rt->tab) == 0) ? 1 : 0;
    s_sel          = 0u;
    s_prev_keys    = 0u;
    s_fps_t0       = board_cycles();
    s_fps_s0       = board_swap_count();
    s_fps          = 0u;
    s_first_logged = 0;
    return 0;
}

static void shell_leave(appfw_ctx *c)
{
    (void)c;    /* renderer_run 同步等表执行完才返回，没有在途的引擎表 */
}

static void shell_frame(appfw_ctx *c, uint32_t keys, uint32_t frame)
{
    const uint32_t pressed = keys & ~s_prev_keys;
    const uint32_t li      = c->seq & 1u;
    const int      dst     = li ? STG_SLOT_FB1 : STG_SLOT_FB0;
    uint32_t       count, i;
    gfx_list       l;
    gfx_rect       all;
    uint32_t       bad = 0u;

    s_prev_keys = keys;

    /* --- 输入 --- */
    (void)appfw_apps(&count);
    if (pressed & BOARD_KEY_UP)
        s_sel = (s_sel == 0u) ? count - 1u : s_sel - 1u;
    if (pressed & BOARD_KEY_DOWN)
        s_sel = (s_sel + 1u) % count;
    if (pressed & BOARD_KEY_BOMB)
        appfw_request(appfw_app_at(s_sel));
    if (pressed & BOARD_KEY_MODE1)
        appfw_request(appfw_app_at(0));
    else if (pressed & BOARD_KEY_MODE2 && count > 1u)
        appfw_request(appfw_app_at(1));
    else if (pressed & BOARD_KEY_MODE3 && count > 2u)
        appfw_request(appfw_app_at(2));

    /* --- 组帧 --- */
    {
    const uint32_t t0 = board_cycles();
    all.x0 = 0;
    all.y0 = 0;
    all.x1 = (int32_t)FB_WIDTH - 1;
    all.y1 = (int32_t)FB_HEIGHT - 1;

    gfx_list_begin(&l, c->rt->cmds[li], c->rt->cap[li], &c->rt->tab);
    bad += (uint32_t)(gfx_set_clip(&l, all) != 0);
    bad += (uint32_t)(gfx_fill(&l, dst, all, MENU_BG, GFX_EQ_COPY, 0u) != 0);

    /* 标题：premult ALPHA，画法同游戏标题 */
    bad += (uint32_t)(gfx_blit(&l, STG_SLOT_TITLE, 0, 0, dst,
                               ((int)FB_WIDTH - (int)ASSET_TITLE_W) / 2, 44,
                               (int)ASSET_TITLE_W, (int)ASSET_TITLE_H,
                               GFX_EQ_ALPHA, 0u, 0, 0u) != 0);

    /* 列表：光标小飞船（ships 第 0 格，黑键透明）+ 名称 + 说明 */
    {
        const appfw_app *const *apps = appfw_apps(&count);
        char sel_line[24];
        core_out o;

        for (i = 0u; i < count; i++) {
            const int          y = MENU_Y0 + (int)(i * MENU_STEP);
            const stg_font    *f = (i == s_sel) ? &s_font_cyan : &s_font;

            if (i == s_sel) {
                bad += (uint32_t)(gfx_blit(&l, STG_SLOT_SHIPS, 0, 0, dst,
                                           CURSOR_X,
                                           y - ((int)ASSET_SHIPS_CELL_H - (int)f->cell_h) / 2,
                                           (int)ASSET_SHIPS_CELL_W, (int)ASSET_SHIPS_CELL_H,
                                           GFX_EQ_COPY, 0u, 1, ASSET_SHIPS_KEY) != 0);
            }
            bad += stg_text_draw(&l, f, dst, NAME_X, y, apps[i]->name, NULL);
            bad += stg_text_draw(&l, &s_font, dst, DESC_X, y, apps[i]->desc, NULL);
        }

        /* 选中条目号（判据锚点：菜单行） */
        core_out_init(&o, sel_line, (uint32_t)sizeof sel_line);
        core_out_str(&o, "SEL ");
        core_out_u32(&o, s_sel);
        bad += stg_text_draw(&l, &s_font, dst, NAME_X, MENU_Y0 + (int)(count * MENU_STEP),
                             sel_line, NULL);
    }

    /* 页脚、版本与 fps */
    bad += stg_text_draw(&l, &s_font, dst, 76, FOOT_Y,
                         "UP/DOWN SELECT  BOMB START  1-3 DIRECT  1+2+3 HOLD=HOME", NULL);
    bad += stg_text_draw(&l, &s_font, dst, 508, 8, "CONSOLE SW10", NULL);
    {
        char     line[16];
        core_out o;

        core_out_init(&o, line, (uint32_t)sizeof line);
        core_out_u32(&o, s_fps);
        bad += stg_text_draw(&l, &s_font, dst, 604, FOOT_Y, line, NULL);
    }

    bad += (uint32_t)(gfx_list_end(&l) != 0);
    s_cyc_build = board_cycles() - t0;
    }

    /* --- 执行（引擎优先）与呈现 --- */
    {
        exec_sw_stats st;
        uint32_t      cyc;
        uint32_t      ticket;
        const uint32_t t1 = board_cycles();

        if (renderer_run(s_hw, &c->rt->tab, &c->rt->mem, &c->rt->es, &c->rt->err,
                         &l, &st, &cyc) != GFX_OK)
            bad++;
        s_cyc_exec = board_cycles() - t1;
        {
            const uint32_t t2 = board_cycles();
            if (s_hw != 0)
                board_dcache_invalidate_all();      /* 引擎写回的帧，CPU 读 CRC 前失效 */
            if (board_present_request(dst, &ticket) != 0) {
                bad++;
            } else {
                /* 睡到场起始（帧信号量），把 CPU 让给 monitor/idle；
                   醒来确认交换，未完成再走上界的轮询兜底 */
                rtos_wait_frame(50);
                if (!board_present_finished(ticket))
                    bad += (uint32_t)(board_present_wait(ticket) != 0);
            }
            c->shown = dst;
            s_cyc_present = board_cycles() - t2;
        }
    }
    c->seq++;

    if (board_cycles() - s_fps_t0 >= (uint32_t)BOARD_HZ) {
        char     buf[96];
        core_out o;

        s_fps    = board_swap_count() - s_fps_s0;
        s_fps_s0 = board_swap_count();
        s_fps_t0 = board_cycles();
        core_out_init(&o, buf, (uint32_t)sizeof buf);
        core_out_str(&o, "menu fps ");
        core_out_u32(&o, s_fps);
        core_out_str(&o, " irqA ");
        core_out_u32(&o, board_irq_frame_events());
        core_out_str(&o, " tick ");
        core_out_u32(&o, (uint32_t)xTaskGetTickCount());
        core_out_str(&o, " bld ");
        core_out_u32(&o, s_cyc_build);
        core_out_str(&o, " exe ");
        core_out_u32(&o, s_cyc_exec);
        core_out_str(&o, " prs ");
        core_out_u32(&o, s_cyc_present);
        core_out_str(&o, "\r\n");
        appfw_log(buf);
    }

    if (!s_first_logged && c->shown >= 0) {
        char     buf[96];
        core_out o;

        s_first_logged = 1;
        board_dcache_invalidate_all();
        core_out_init(&o, buf, (uint32_t)sizeof buf);
        core_out_str(&o, "menu crc ");
        core_out_hex32(&o, stg_modes_frame_crc(board_fb(c->shown)));
        core_out_str(&o, " sel ");
        core_out_u32(&o, s_sel);
        core_out_str(&o, "\r\n");
        appfw_log(buf);
    }

    (void)frame;
    (void)bad;
}

const appfw_app g_app_menu = {
    "menu", "console menu",
    shell_enter,
    shell_leave,
    shell_frame,
};

const appfw_app *shell_app(void)
{
    return &g_app_menu;
}
