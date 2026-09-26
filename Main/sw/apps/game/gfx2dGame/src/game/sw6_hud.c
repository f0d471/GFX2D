/******************************************************************************
* @file sw6_hud.c: 右上角的 HUD
*
* @brief  实现见 sw6_hud.h。九行的行表见 spec 2.3；每行按实际文字逐字画，
*         不补空格到行尾；行内的空格也照画一条 BLIT（整格命中色键、不写像素），
*         这样命令数与像素数的口径只有一套。
******************************************************************************/

#include <stddef.h>

#include "board.h"

#include "core/format.h"

#include "stg_text.h"
#include "sw6_hud.h"
#include "sw6_report.h"

#include "assets.h"

/* 一条 BLIT 或底色 FILL 被拒时记这里：主循环把它并进 fail。
   正常路径恒为 0。 */
static uint32_t s_hud_bad;

uint32_t sw6_hud_bad(void)
{
    return s_hud_bad;
}

/* 一行文字：tag + 数值或 `--`。 */
static void fmt(char *buf, uint32_t cap, const char *tag, uint32_t v, int has)
{
    core_out o;

    core_out_init(&o, buf, cap);
    core_out_str(&o, tag);
    if (has)
        core_out_u32(&o, v);
    else
        core_out_str(&o, "--");
}

/* 逐字 BLIT（共用的 stg_text_draw，SW7 2.6 节）。字符都在 0x20~0x7E 之内，落不进第 96 格（空的黑格）。 */
static void draw_text(gfx_list *l, int dst, uint32_t line, const char *s)
{
    static const stg_font font = {
        SW6_SLOT_FONT, SW6_FONT_CELL_W, SW6_FONT_CELL_H, SW6_FONT_COLS,
        SW6_FONT_FIRST, ASSET_FONT8X16_CELLS, SW6_FONT_KEY,
    };

    s_hud_bad += stg_text_draw(l, &font, dst, (int)SW6_HUD_X,
                               (int)(SW6_HUD_Y + line * SW6_FONT_CELL_H), s, NULL);
}

void sw6_hud_build(gfx_list *l, int dst, const sw6_hud_in *in,
                   uint32_t *out_cmd, uint32_t *out_pix)
{
    char     line[40];
    gfx_rect box;

    box.x0 = (int32_t)SW6_HUD_X;
    box.y0 = (int32_t)SW6_HUD_Y;
    box.x1 = (int32_t)(SW6_HUD_X + SW6_HUD_W - 1u);
    box.y1 = (int32_t)(SW6_HUD_Y + SW6_HUD_H - 1u);
    if (gfx_fill(l, dst, box, SW6_HUD_BG, GFX_EQ_COPY, 0u) != 0)
        s_hud_bad++;

    fmt(line, (uint32_t)sizeof line, "SW FPS ", in->sw_fps, in->live && in->sw_fps_valid);
    draw_text(l, dst, 0u, line);
    draw_text(l, dst, 1u, "HW FPS --");

    fmt(line, (uint32_t)sizeof line, "N ", in->n, 1);
    draw_text(l, dst, 2u, line);
    fmt(line, (uint32_t)sizeof line, "K ", in->k, 1);
    draw_text(l, dst, 3u, line);

    fmt(line, (uint32_t)sizeof line, "CMD ", in->prev_cmd, in->prev_valid);
    draw_text(l, dst, 4u, line);
    fmt(line, (uint32_t)sizeof line, "PIX ", in->prev_pix, in->prev_valid);
    draw_text(l, dst, 5u, line);

    fmt(line, (uint32_t)sizeof line, "LOG ", in->cyc_in, in->live);
    draw_text(l, dst, 6u, line);
    fmt(line, (uint32_t)sizeof line, "BLD ", in->cyc_bld, in->live);
    draw_text(l, dst, 7u, line);
    fmt(line, (uint32_t)sizeof line, "EXE ", in->cyc_exe, in->live);
    draw_text(l, dst, 8u, line);

    if (out_cmd != NULL)
        *out_cmd = in->prev_valid ? in->prev_cmd : SW6_NONE;
    if (out_pix != NULL)
        *out_pix = in->prev_valid ? in->prev_pix : SW6_NONE;
}
