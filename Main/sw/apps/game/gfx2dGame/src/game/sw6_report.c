/******************************************************************************
* @file sw6_report.c: 三个 CRC 口径与全部日志行
*
* @brief  实现见 sw6_report.h。应用里不出现 printf，行都由 lib/core/format 拼，
*         经 board_puts 出去（板上是 UART，PC 是 stdout，两边逐字节相同）。
******************************************************************************/

#include <stddef.h>

#include "board.h"

#include "core/crc32.h"
#include "core/format.h"

#include "sw6_report.h"

/* --- CRC ----------------------------------------------------------------- */

/* 逐行喂、每行只取 x0 起 px 个像素，跳过行跨 4096 与可见 2560 之间的间隙。 */
static uint32_t crc_rows(const uint32_t *base, uint32_t y0, uint32_t y1,
                         uint32_t x0, uint32_t px)
{
    crc32_ctx      ctx;
    uint32_t       y;
    const uint8_t *p0 = (const uint8_t *)base;

    crc32_begin(&ctx);
    for (y = y0; y < y1; y++)
        crc32_update(&ctx, p0 + (size_t)y * FB_STRIDE_BYTES + (size_t)x0 * 4u,
                     px * 4u);
    return crc32_end(&ctx);
}

uint32_t sw6_crc_full(const uint32_t *base)
{
    return crc_rows(base, 0u, FB_HEIGHT, 0u, FB_WIDTH);
}

uint32_t sw6_crc_scene(const uint32_t *base)
{
    crc32_ctx      ctx;
    uint32_t       y;
    const uint8_t *p0        = (const uint8_t *)base;
    const uint32_t y_hud_end = SW6_HUD_Y + SW6_HUD_H;

    /* HUD 矩形所在的那几行只取左边 400 像素，其余行取整行 */
    crc32_begin(&ctx);
    for (y = 0u; y < FB_HEIGHT; y++) {
        if (y < y_hud_end)
            crc32_update(&ctx, p0 + (size_t)y * FB_STRIDE_BYTES, SW6_HUD_X * 4u);
        else
            crc32_update(&ctx, p0 + (size_t)y * FB_STRIDE_BYTES, FB_WIDTH * 4u);
    }
    return crc32_end(&ctx);
}

uint32_t sw6_crc_hud(const uint32_t *base)
{
    return crc_rows(base, SW6_HUD_Y, SW6_HUD_Y + SW6_HUD_H, SW6_HUD_X, SW6_HUD_W);
}

/* --- 行 ------------------------------------------------------------------ */

void sw6_log_pak(uint32_t total_len, uint32_t pack_crc)
{
    char     buf[64];
    core_out o;

    core_out_init(&o, buf, (uint32_t)sizeof buf);
    core_out_str(&o, "pak ");
    core_out_u32(&o, total_len);
    core_out_ch(&o, ' ');
    core_out_hex32(&o, pack_crc);
    core_out_str(&o, "\r\n");
    board_puts(buf);
}

void sw6_log_scene(uint32_t i, uint32_t scrc)
{
    char     buf[64];
    core_out o;

    core_out_init(&o, buf, (uint32_t)sizeof buf);
    core_out_str(&o, "scene ");
    core_out_u32(&o, i);
    core_out_str(&o, " scrc ");
    core_out_hex32(&o, scrc);
    core_out_str(&o, "\r\n");
    board_puts(buf);
}

void sw6_log_eq(char kind, const uint32_t eq[4])
{
    char     buf[96];
    core_out o;
    uint32_t i;

    core_out_init(&o, buf, (uint32_t)sizeof buf);
    core_out_str(&o, "eq ");
    core_out_ch(&o, kind);
    for (i = 0u; i < 4u; i++) {
        core_out_ch(&o, ' ');
        core_out_u32(&o, eq[i]);
    }
    core_out_str(&o, "\r\n");
    board_puts(buf);
}

void sw6_log_frame(const sw6_frame_rec *r)
{
    char     buf[288];
    core_out o;

    core_out_init(&o, buf, (uint32_t)sizeof buf);

    core_out_str(&o, "f ");
    core_out_u32(&o, r->frame);
    core_out_ch(&o, ' ');
    core_out_ch(&o, r->stage);
    core_out_str(&o, " p ");
    core_out_u32(&o, r->pass);
    core_out_str(&o, " s ");
    if (r->scene < 0)
        core_out_ch(&o, '-');
    else
        core_out_u32(&o, (uint32_t)r->scene);

    core_out_str(&o, " keys ");
    core_out_hex32(&o, r->keys);
    core_out_str(&o, " n ");
    core_out_u32(&o, r->n);
    core_out_str(&o, " k ");
    core_out_u32(&o, r->k);
    core_out_str(&o, " cmd ");
    core_out_u32(&o, r->cmd);
    core_out_str(&o, " pix ");
    core_out_u32(&o, r->pix);
    core_out_str(&o, " ksk ");
    core_out_u32(&o, r->ksk);

    core_out_str(&o, " hud ");
    if (r->hud_cmd == SW6_NONE)
        core_out_ch(&o, '-');
    else
        core_out_u32(&o, r->hud_cmd);
    core_out_ch(&o, ' ');
    if (r->hud_pix == SW6_NONE)
        core_out_ch(&o, '-');
    else
        core_out_u32(&o, r->hud_pix);

    core_out_str(&o, " off ");
    core_out_u32(&o, r->off);
    core_out_str(&o, " cyc ");
    core_out_u32(&o, r->cyc_in);
    core_out_ch(&o, ' ');
    core_out_u32(&o, r->cyc_bld);
    core_out_ch(&o, ' ');
    core_out_u32(&o, r->cyc_exe);

    core_out_str(&o, " crc ");
    core_out_hex32(&o, r->crc);
    core_out_str(&o, " scrc ");
    core_out_hex32(&o, r->scrc);
    core_out_str(&o, " hcrc ");
    core_out_hex32(&o, r->hcrc);
    core_out_str(&o, "\r\n");

    board_puts(buf);
}

void sw6_log_end(uint32_t fail)
{
    char     buf[48];
    core_out o;

    core_out_init(&o, buf, (uint32_t)sizeof buf);
    core_out_str(&o, "fail ");
    core_out_u32(&o, fail);
    core_out_str(&o, "\r\ndone\r\n");
    board_puts(buf);
}
