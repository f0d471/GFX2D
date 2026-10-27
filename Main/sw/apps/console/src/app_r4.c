/******************************************************************************
* @file app_r4.c: 三维演示适配（SW10）
*
* @brief  把 gfx2dGame 的模式 4（R4 三维走廊/茶杯）接进应用框架，逐帧行为与
*         裸机 mode_frame 的模式 4 分支一致（30Hz 逻辑、present 由 r4 内部按
*         锚定节拍发出）。键位沿用裸机：SWAP=硬/软光栅、SLOW=L1/L2 阶梯、
*         MODE3=走廊/茶杯互切；切换走帧边界重进。退出走 HOME。
******************************************************************************/

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "board.h"

#include "gfx/gfx.h"

#include "renderer.h"
#include "r4.h"
#include "stg_slots.h"

#include "appfw.h"

#ifndef GFX2D_R4_HW_DFLT
#define GFX2D_R4_HW_DFLT 1
#endif
#ifndef GFX2D_R4_LADDER_DFLT
#define GFX2D_R4_LADDER_DFLT 2
#endif

static uint32_t s_prev_keys;
static uint32_t s_hw     = GFX2D_R4_HW_DFLT;
static uint32_t s_ladder = GFX2D_R4_LADDER_DFLT;
static uint32_t s_scene;
static int      s_restart;

/* HOME 和弦 = 三模式键同按（appfw 检测）。appfw.h 的键位契约：MODE 键归
   框架，应用处理它们会与 HOME 冲突——MODE3 是本应用的热键，而 HOME 组合
   里含 MODE3 位，不避让的话按住 HOME 的每一帧都被当成场景切换、帧边界
   重进（TEX 重装 + 清屏 + 等场 ≈250ms/帧），8 帧的和弦窗口永远凑不满。 */
#define R4_HOME_COMBO (BOARD_KEY_MODE1 | BOARD_KEY_MODE2 | BOARD_KEY_MODE3)

static int r4_enter(appfw_ctx *c)
{
    r4_scene_select(s_scene);
    if (stg_runtime_surfaces_r4(c->rt) != 0)
        return -1;
    /* 先复位引擎并登记 R4 的表面表，再让 r4 写 TEX 窗口（R3 板测的同序） */
    if (renderer_prepare(1, &c->rt->tab) != 0)
        return -1;
    if (r4_mode_enter(&c->rt->tab, &c->rt->mem, (int)s_hw, s_ladder) != 0)
        return -1;
    /* 预热：先呈现清过的非活动面，再清旧前景；统计从显示稳定后起算
       （与裸机 mode_enter 模式 4 的块逐行对应） */
    {
        const int blank = c->shown == STG_SLOT_FB0 ? STG_SLOT_FB1 : STG_SLOT_FB0;

        memset(board_fb(blank), 0, FB_BYTES);
        board_present(blank);
        memset(board_fb(blank ^ 1), 0, FB_BYTES);
        board_wait_frame();
        (void)board_display_stats_clear();
        r4_timing_start();
        c->shown = blank;
        c->seq   = (c->seq & ~1u) | (uint32_t)(blank ^ 1);
    }
    s_prev_keys = 0u;
    s_restart   = 0;
    return 0;
}

static void r4_leave(appfw_ctx *c)
{
    int presented;

    if (renderer_pipeline_flush(&presented) == 0 && presented >= 0)
        c->shown = presented;
    r4_mode_leave();
}

static void r4_frame(appfw_ctx *c, uint32_t keys, uint32_t frame)
{
    const uint32_t pressed = keys & ~s_prev_keys;
    r4_out        out;

    s_prev_keys = keys;

    if ((keys & R4_HOME_COMBO) == R4_HOME_COMBO)
        return;                      /* HOME 和弦进行中：热键全部让位给 runner */

    if (s_restart) {                 /* 帧边界重进：场景/后端/阶梯的切换都走这 */
        r4_mode_leave();
        if (r4_enter(c) != 0)
            appfw_log("r4 enter err\r\n");
        return;
    }
    if (pressed & BOARD_KEY_MODE3) {
        s_scene  ^= 1u;
        s_restart = 1;
        return;
    }
    if (pressed & BOARD_KEY_SWAP) {
        s_hw     ^= 1u;
        s_restart = 1;
        return;
    }
    if (pressed & BOARD_KEY_SLOW) {
        s_ladder = (s_ladder >= 2u) ? 1u : 2u;
        s_restart = 1;
        return;
    }

    {
        const int dst = (c->seq & 1u) ? STG_SLOT_FB1 : STG_SLOT_FB0;
        const int rc  = r4_mode_frame(&c->rt->tab, &c->rt->mem, dst,
                                      keys | r4_pending_keys(), &out);

        if (rc == 0)
            c->shown = dst;          /* r4 成功路径已确认呈现 */
        else
            c->rt->fails++;
        c->seq++;
    }
    (void)frame;
}

const appfw_app g_app_r4 = {
    "r4", "3d corridor/cup",
    r4_enter,
    r4_leave,
    r4_frame,
};
