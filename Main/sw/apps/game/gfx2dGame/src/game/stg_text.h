/******************************************************************************
* @file stg_text.h: 字库的逐字绘制（基础 Demo 的 HUD 与游戏的 HUD 共用）
*
* @brief  文字绘制的共用函数。从 sw6_hud.c 的逐字 BLIT 抽出来：字库的几何
*         （表面槽、格子宽高、每行格数、首字符、格数、键色）由调用方给，所以这里不认识槽号、
*         也不含 assets.h，app 测试能直接编。基础 Demo 的 HUD 改成调它之后，生成的命令逐条不变。
*
*         规则（照 sw6_hud.c 原来的写法）：第 i 个字符的格号 gi = (uint8_t)c - first，
*         gi 小于格数才画一条 BLIT（COPY 加色键），源左上角 ((gi % cols) * cw, (gi / cols) * ch)；
*         不论画没画，x 都前进一格。空格也照画一条（整格命中色键、不写像素）。
******************************************************************************/
#ifndef STG_TEXT_H
#define STG_TEXT_H

#include <stdint.h>

#include "gfx/gfx.h"

typedef struct {
    int      slot;          /* 字库图集的表面槽 */
    uint32_t cell_w;        /* 一格的宽高（像素） */
    uint32_t cell_h;
    uint32_t cols;          /* 图集每行几格 */
    uint32_t first;         /* 第 0 格是哪个字符 */
    uint32_t cells;         /* 格数：gi 不小于它的字符不画 */
    uint32_t key;           /* 色键 */
} stg_font;

/* 在 (x, y) 起逐字画 s。返回被构造层拒绝的条数（正常路径为 0）；画出来的条数加进 *drawn（可为 NULL）。 */
uint32_t stg_text_draw(gfx_list *l, const stg_font *f, int dst, int x, int y, const char *s,
                       uint32_t *drawn);

/* 字符串的像素宽度（字符数乘格宽），给右对齐与居中用。 */
uint32_t stg_text_width(const stg_font *f, const char *s);

#endif /* STG_TEXT_H */
