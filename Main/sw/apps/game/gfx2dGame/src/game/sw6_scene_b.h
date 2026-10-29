/******************************************************************************
* @file sw6_scene_b.h: 场景 B —— 三张整屏图的大图切换
*
* @brief  三张 640×480 的整屏图在**初始化时**预合成进 DDR 离屏层
*         （`REGION_OFFSCREEN`，设计文档 2.1 节给它的用途就是背景预合成），
*         每帧只做一次整屏 BLIT COPY，把选中的那张搬到后缓冲。
*
*         每张的构图用素材包现有条目与程序生成的 FILL，不往素材包里新增整图。
*         离屏层布局：`OFF_i = 离屏层起点 + i × SW6_OFF_SLOT_STEP`，每张 640×480、
*         XRGB8888、行跨 4096；第四个 2 MiB 槽留给「调试抓帧」。
*
*         切换规则写死：当前这张已显示满 k 帧就换下一张（s = (s+1) mod 3），
*         k 在显示中途变小时下一帧即按新 k 判；k=1 时逐帧 0→1→2→0。
******************************************************************************/
#ifndef SW6_SCENE_B_H
#define SW6_SCENE_B_H

#include <stdint.h>

#include "gfx/gfx.h"

#include "sw6_cfg.h"
#include "sw6_scene_a.h"   /* sw6_build_stat */

/* 预合成一次提交需要的东西。cmds／cap 是命令表缓冲（主程序给 CMDLIST_A）。 */
typedef struct {
    const gfx_surface_table *tab;
    const gfx_mem           *mem;
    exec_sw_state           *es;
    exec_sw_error           *err;
    gfx_cmd                 *cmds;
    uint32_t                 cap;
} sw6_b_ctx;

typedef struct sw6_scene_b {
    uint32_t s;                 /* 本帧搬哪一张 */
    uint32_t shown;             /* 当前这张已显示帧数 */
    uint32_t k;
    uint32_t ref[SW6_SCENES];   /* 三张离屏图的 scrc 参考值（口径同日志的 scrc） */
} sw6_scene_b;

void sw6_b_begin(sw6_scene_b *b, uint32_t k0);

/* 三张离屏图各一次提交：SET_CLIP、全屏 FILL 打底、构图命令、END。
   每次提交后跑一遍软件执行器并算 scrc 参考值。返回 0，或 GFX_E_* 错误码／
   GFX_BUILD_*，或板级负错误码；失败时 *out_bad 记下被拒的追加次数。 */
int  sw6_b_precompose(sw6_scene_b *b, const sw6_b_ctx *ctx,
                      uint32_t eq_out[4], uint32_t *out_bad);

/* 上下键的按下沿：k 在 1~120 内增减 1。 */
void sw6_b_adjust(sw6_scene_b *b, int up, int down);

/* 本帧该搬哪一张，同时推进显示计数。 */
uint32_t sw6_b_step(sw6_scene_b *b);

#endif /* SW6_SCENE_B_H */
