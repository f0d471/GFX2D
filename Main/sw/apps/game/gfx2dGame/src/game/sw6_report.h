/******************************************************************************
* @file sw6_report.h: 三个 CRC 口径与全部日志行
*
* @brief  日志格式的权威是 spec 2.7 节。逐行以 `\r\n` 结尾（与 UART 逐字节相同），
*         按空白切分即可解析，行里不出现字符本身。
*
*         三个 CRC 都逐行喂、只取可见的那一段、跳过行跨间隙：
*           crc   整帧 640×480，每行前 2560 字节；
*           scrc  去掉 HUD 矩形的部分：第 0~159 行只取 x=0..399（1600 字节），
*                 第 160~479 行取整行 2560 字节；
*           hcrc  HUD 矩形本身：第 0~159 行各取 x=400..639（960 字节）。
*         板级层不参与：帧缓冲与离屏图的布局都是 640×480、行跨 4096。
******************************************************************************/
#ifndef SW6_REPORT_H
#define SW6_REPORT_H

#include <stdint.h>

#include "sw6_cfg.h"

/* 日志里“这一格还没有值”（HUD 的上一帧统计、首帧）用 SW6_NONE，打出来是 '-'。 */
#define SW6_NONE 0xFFFFFFFFu

uint32_t sw6_crc_full(const uint32_t *base);
uint32_t sw6_crc_scene(const uint32_t *base);
uint32_t sw6_crc_hud(const uint32_t *base);

/* pak <总长> <整包CRC 8位> */
void sw6_log_pak(uint32_t total_len, uint32_t pack_crc);

/* scene <i> scrc <8位> */
void sw6_log_scene(uint32_t i, uint32_t scrc);

/* eq <P|A> <copy> <alpha> <add> <straight> */
void sw6_log_eq(char kind, const uint32_t eq[4]);

/* 一帧一行（spec 2.7 的 f 行） */
typedef struct {
    uint32_t frame;
    char     stage;         /* 'A' 或 'B' */
    uint32_t pass;          /* 1 或 2 */
    int32_t  scene;         /* 本帧搬的离屏图号；A 帧传 -1，打 '-' */
    uint32_t keys;
    uint32_t n;
    uint32_t k;
    uint32_t cmd;           /* 本帧 st.cmd，不含 END */
    uint32_t pix;           /* 本帧 st.pixel_written */
    uint32_t ksk;           /* 本帧 st.key_skip */
    uint32_t hud_cmd;       /* 本帧 HUD 实际画出来的上一帧统计；SW6_NONE 打 '-' */
    uint32_t hud_pix;
    uint32_t off;           /* 本帧部分出屏的场景命令数 */
    uint32_t cyc_in;
    uint32_t cyc_bld;
    uint32_t cyc_exe;
    uint32_t crc;
    uint32_t scrc;
    uint32_t hcrc;
} sw6_frame_rec;

void sw6_log_frame(const sw6_frame_rec *r);

/* fail <n> 与 done 两行 */
void sw6_log_end(uint32_t fail);

#endif /* SW6_REPORT_H */
