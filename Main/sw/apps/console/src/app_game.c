/******************************************************************************
* @file app_game.c: 弹幕游戏适配（SW10）
*
* @brief  把 gfx2dGame 的模式 2（stg_modes 的 game 入口）接进应用框架，
*         逐帧行为与裸机 mode_frame 的模式 2 分支一致：
*           dst 按全局 seq 奇偶轮换（跨应用连续，双缓冲不变量不变）；
*           Tab 切后端（软件->HW/DDR->HW/TEX->软件，TEX 热集没装好就跳过），
*           切换在触发帧结束后的下一帧生效，带 backend 行；
*           裸机的「上+下 8 帧切模式」在这里不接：模式切换归菜单，退出走 HOME。
*         游戏自己的日志（nmax/err 行）经 stg_modes_set_log 进同一队列。
******************************************************************************/

#include <stddef.h>
#include <stdint.h>

#include "board.h"

#include "core/format.h"

#include "gfx/gfx.h"

#include "frontend_game.h"
#include "renderer.h"
#include "stg_modes.h"
#include "stg_slots.h"

#include "appfw.h"

static uint32_t s_prev_keys;
static uint32_t s_fps[4], s_fps_valid[4];
static uint32_t s_fps_t0, s_fps_s0;
static uint32_t s_field_t0;
static uint32_t s_period_sum, s_period_n;   /* 帧入口到帧入口的迭代周期 */
static uint32_t s_build_sum;                /* out.cyc_exec：组帧+提交+引擎等待 */
static uint32_t s_frames;
static uint32_t s_frm_prev;

static int game_enter(appfw_ctx *c)
{
    if (frontend_game_reset() != 0)
        return -1;
    if (stg_runtime_surfaces_stg(c->rt) != 0)
        return -1;
    if (stg_runtime_backend_apply(c->rt, (uint32_t)c->rt->backend) != 0
        && (uint32_t)c->rt->backend >= 2u)
        c->rt->backend = 1;      /* 覆盖层失败：退回 HW/DDR */
    if (renderer_prepare(c->rt->backend, &c->rt->tab) != 0)
        return -1;
    stg_modes_reset();
    stg_mode_game_enter();
    s_prev_keys  = 0u;
    s_fps_t0     = board_cycles();
    s_fps_s0     = board_swap_count();
    s_field_t0   = board_frame_count();
    s_period_sum = 0u;
    s_period_n   = 0u;
    s_build_sum  = 0u;
    s_frames     = 0u;
    s_frm_prev   = board_cycles();
    return 0;
}

static void game_leave(appfw_ctx *c)
{
    int presented;

    if (renderer_pipeline_flush(&presented) == 0 && presented >= 0)
        c->shown = presented;
    stg_modes_reset();
}

static stg_main_ctx game_ctx(appfw_ctx *c)
{
    stg_main_ctx x;

    x.tab     = &c->rt->tab;
    x.mem     = &c->rt->mem;
    x.es      = &c->rt->es;
    x.err     = &c->rt->err;
    x.cmds[0] = c->rt->cmds[0];
    x.cmds[1] = c->rt->cmds[1];
    x.cap[0]  = c->rt->cap[0];
    x.cap[1]  = c->rt->cap[1];
    x.hw      = c->rt->backend;
    return x;
}

static void backend_switch(appfw_ctx *c, uint32_t frame)
{
    char     buf[64];
    core_out o;
    uint32_t nb = (uint32_t)c->rt->backend + 1u;
    int      presented;

    if (nb >= 4u)
        nb = 0u;
    if (nb >= 2u && !c->rt->tex_ready)
        nb = 0u;                        /* 热集没装载成功：跳过 HW/TEX 与 STRIP */
    if (renderer_pipeline_flush(&presented) == 0 && presented >= 0)
        c->shown = presented;
    (void)frontend_game_reset();
    if (stg_runtime_backend_apply(c->rt, nb) != 0) {
        nb = 1u;
        (void)stg_runtime_backend_apply(c->rt, nb);
    }
    c->rt->backend = (int)nb;
    if (renderer_prepare(c->rt->backend, &c->rt->tab) != 0)
        appfw_log("engine prepare err\r\n");

    core_out_init(&o, buf, (uint32_t)sizeof buf);
    core_out_str(&o, c->rt->backend == 0 ? "backend sw frame "
                  : (c->rt->backend == 1 ? "backend hw frame "
                  : (c->rt->backend == 2 ? "backend hw-tex frame "
                                         : "backend hw-strip frame ")));
    core_out_u32(&o, frame);
    core_out_str(&o, "\r\n");
    appfw_log(buf);
}

static void game_frame(appfw_ctx *c, uint32_t keys, uint32_t frame)
{
    const uint32_t t_in    = board_cycles();
    const uint32_t pressed = keys & ~s_prev_keys;
    const int      dst     = (c->seq & 1u) ? STG_SLOT_FB1 : STG_SLOT_FB0;
    stg_main_ctx   ctx     = game_ctx(c);
    stg_hud_live   live;
    stg_mode_out   out;

    if (s_frm_prev != 0u) {
        s_period_sum += t_in - s_frm_prev;   /* 迭代周期（帧入口到帧入口） */
        s_period_n++;
    }
    s_frm_prev = t_in;
    s_prev_keys = keys;

    if (pressed & BOARD_KEY_SWAP)
        backend_switch(c, frame);

    ctx = game_ctx(c);       /* backend_switch 可能改了 backend，重取 */
    if (!renderer_pipeline_enabled(c->rt->backend) && dst == c->shown) {
        appfw_log("fb err dst==shown\r\n");
        c->rt->fails++;
    }

    live.sw_fps       = s_fps[0];
    live.sw_fps_valid = (int)s_fps_valid[0];
    live.hw_fps       = s_fps[c->rt->backend ? (uint32_t)c->rt->backend : 1u];
    live.hw_fps_valid = (int)s_fps_valid[c->rt->backend ? (uint32_t)c->rt->backend : 1u];
    live.seg_text     = 0;

    stg_mode_game_frame(&ctx, dst, keys, &live, &out);
    s_build_sum += out.cyc_exec;
    s_frames++;
    if (out.presented_dst >= 0)
        c->shown = out.presented_dst;
    c->seq++;

    if (board_cycles() - s_fps_t0 >= (uint32_t)BOARD_HZ) {
        const uint32_t fps    = board_swap_count() - s_fps_s0;
        const uint32_t dfield = board_frame_count() - s_field_t0;
        const uint32_t avg_p  = s_period_n ? s_period_sum / s_period_n : 0u;
        const uint32_t avg_c  = s_frames ? s_build_sum / s_frames : 0u;
        uint32_t       isum, in, avg_b;
        char           buf[128];
        core_out       o;

        appfw_loop_cycles(&isum, &in);
        avg_b = in ? isum / in : 0u;
        s_fps[c->rt->backend ? (uint32_t)c->rt->backend : 0u] = fps;
        s_fps_valid[c->rt->backend ? (uint32_t)c->rt->backend : 0u] = 1u;
        s_fps_s0 = board_swap_count();
        s_field_t0 = board_frame_count();
        s_fps_t0 = board_cycles();
        /* 「游戏在 console 内不退化」的板上证据行：一秒窗口里 swap 数必须
           等于场数（每场恰好一次交换 = 零丢帧的既有口径），fps 应为 60。
           per = 迭代周期（帧入口到帧入口），busy = runner 循环体的忙耗时
           （appfw 计），cyc = 组帧+提交+引擎等待；per-busy 差值就是 runner
           被阻塞/抢占走的时间。 */
        core_out_init(&o, buf, (uint32_t)sizeof buf);
        core_out_str(&o, "stg fps ");
        core_out_u32(&o, fps);
        core_out_str(&o, " field ");
        core_out_u32(&o, dfield);
        core_out_str(&o, " swap ");
        core_out_u32(&o, fps);
        core_out_str(&o, " backend ");
        core_out_u32(&o, (uint32_t)c->rt->backend);
        core_out_str(&o, " per ");
        core_out_u32(&o, avg_p);
        core_out_str(&o, " busy ");
        core_out_u32(&o, avg_b);
        core_out_str(&o, " cyc ");
        core_out_u32(&o, avg_c);
        core_out_str(&o, " n ");
        core_out_u32(&o, s_frames);
        core_out_str(&o, "\r\n");
        appfw_log(buf);
        s_period_sum = 0u;
        s_period_n   = 0u;
        s_build_sum  = 0u;
        s_frames     = 0u;
    }
}

const appfw_app g_app_stg = {
    "stg", "danmaku game",
    game_enter,
    game_leave,
    game_frame,
};
