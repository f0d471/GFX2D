/******************************************************************************
* @file stg_runtime.h: 弹幕游戏与三维演示的共享启动运行时（SW10）
*
* @brief  从 gfx2dGame/main.c 的启动序列抽出的公共段：DDR 分区窗口、素材包
*         装载与对账、命令表、stg/r4 两套表面槽、TEX 热集装载与后端覆盖层。
*         console 与 gfx2dGame 共用实现，包含旧判据的注错与影子对拍能力。
*
*         一个进程只有一个实例：r4_set_hooks 的钩子没有上下文参数，
*         用 stg_runtime_set_active 登记当前实例。
******************************************************************************/
#ifndef STG_RUNTIME_H
#define STG_RUNTIME_H

#include <stdint.h>

#include "core/assetpak.h"
#include "gfx/gfx.h"

typedef struct {
    assetpak          pak;         /* 装好的素材包（boot 后有效） */
    gfx_surface_table tab;         /* 表面表：随 surfaces_* 重建 */
    gfx_mem           mem;         /* 素材区->离屏末端的窗口 */
    exec_sw_state     es;          /* 软件执行器状态（命令表共享） */
    exec_sw_error     err;
    gfx_cmd          *cmds[2];     /* 两张命令表（REGION_CMDLIST_A/B） */
    uint32_t          cap[2];
    uint32_t          backend;     /* 0 软件，1 硬件/DDR，2 硬件/TEX，3 硬件/STRIP */
    int               tex_ready;   /* 热集装载并 CRC 自校验通过（HW/TEX 可用） */
    int               tex_on;      /* 表面表的热集槽当前处于 TEX 覆盖层之下 */
    uint32_t          tex_off[3];  /* 各热集槽在 TEX 窗口内的字节偏移 */
    uint32_t          tex_ddr[3];  /* 各热集槽的 DDR 基址（恢复其它后端用） */
    uint32_t          fails;       /* 启动与对账的失败累计（判据口径：0 才算干净） */
#if (defined(G1_TEX_COMPARE) && G1_TEX_COMPARE) || (defined(G4_STRIP_COMPARE) && G4_STRIP_COMPARE)
    uint32_t          tex_shadow[4096];
#endif
} stg_runtime;

/* 登记钩子用的当前实例（r4_set_hooks 无上下文参数）。 */
void stg_runtime_set_active(stg_runtime *rt);

/* 内存窗口 + 素材包装载对账 + 命令表。失败时打一行 err 并返回 -1。 */
int  stg_runtime_boot(stg_runtime *rt);
int  stg_runtime_boot_mem(stg_runtime *rt);
int  stg_runtime_boot_asset(stg_runtime *rt);
int  stg_runtime_surfaces_demo(stg_runtime *rt);
int  stg_runtime_has_raster(void);

/* stg 那套表面槽（stg_slots.h）：FB0/1 + 字库与霓虹素材（含 bg_layers）。 */
int  stg_runtime_surfaces_stg(stg_runtime *rt);

/* r4 那套表面槽：FB0/1 + 字库/大数字/青字库/标题 + 独立三维背景面。 */
int  stg_runtime_surfaces_r4(stg_runtime *rt);

/* TEX 热集装载（标准圆弹/小圆弹/火花）+ 分块读回 CRC 自校验。
   板上失败返回 -1（该次上电 HW/TEX 不可用）；宿主无 TEX 窗口直接置 ready=0 返回 0。 */
int  stg_runtime_tex_load(stg_runtime *rt);

/* 热集槽在 DDR 基址与 TEX 偏移之间切换（HW/TEX 后端的覆盖层）。 */
int  stg_runtime_tex_overlay(stg_runtime *rt, int on);

/* 切后端前的表面表准备；HW/TEX 不可用或覆盖层失败返回 -1（调用方退回 HW/DDR）。 */
int  stg_runtime_backend_apply(stg_runtime *rt, uint32_t backend, uint32_t mode);

/* 给 r4 装钩子：atlas 装载（TEX 写入+读回 CRC）、TEX 被覆盖标记、热集恢复。 */
void stg_runtime_r4_install_hooks(void);

#endif /* STG_RUNTIME_H */
