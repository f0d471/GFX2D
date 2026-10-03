/******************************************************************************
* @file sw6_hud.h: 右上角的 HUD
*
* @brief  固定在 `x=400..639, y=0..159`（spec 2.3）：先一条纯黑 FILL 铺底色，
*         再用 SW5 的 8×16 字库以 BLIT 加色键 `0x00000000` 逐字画九行。
*         底色纯黑是判据要的：HUD 矩形的内容就只有黑底与 0x00FFFFFF 两种像素，
*         检查脚本才能按字库文本独立重画整块来对账。
*
*         **判据模式下实时量一律显示 `--`**（`SW FPS` 与三段耗时）：它们来自实时测量，
*         进了画面就会让同一份输入两次跑出不同的整帧 CRC。演示模式显示实测值，
*         反例注错 INJECT=SW6_LIVE_HUD 让判据模式照样显示实测值。
******************************************************************************/
#ifndef SW6_HUD_DECL_H
#define SW6_HUD_DECL_H

#include <stdint.h>

#include "gfx/gfx.h"

#include "sw6_cfg.h"
#include "sw6_scene_a.h"

typedef struct {
    uint32_t n;             /* HUD 的 N 行 */
    uint32_t k;             /* HUD 的 K 行 */
    uint32_t sw_fps;        /* 实时量：最近一秒的呈现次数 */
    int      sw_fps_valid;  /* 0 = 还没有完整的一秒，SW FPS 显示 -- 而不是 0 */
    uint32_t hw_fps;
    int      hw_fps_valid;
    uint32_t cyc_in;        /* 实时量：三段耗时 */
    uint32_t cyc_bld;
    uint32_t cyc_exe;
    int      live;          /* 1 = 显示实时量 */
    uint32_t prev_cmd;      /* 上一帧最终的统计 */
    uint32_t prev_pix;
    int      prev_valid;    /* 0 = 首帧，CMD／PIX 两行显示 -- */
} sw6_hud_in;

/* 追加 HUD 的命令。out_cmd／out_pix 回填本帧 HUD 实际画出来的上一帧统计
   （首帧是 SW6_NONE），供逐帧日志的 hud 列使用。 */
void sw6_hud_build(gfx_list *l, int dst, const sw6_hud_in *in,
                   uint32_t *out_cmd, uint32_t *out_pix);

/* HUD 段里被构造层拒绝的追加次数（正常路径恒为 0），主循环并进 fail。 */
uint32_t sw6_hud_bad(void);

#endif /* SW6_HUD_DECL_H */
