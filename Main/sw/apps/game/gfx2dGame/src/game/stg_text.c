/******************************************************************************
* @file stg_text.c: 字库的逐字绘制
*
* @brief  口径见 stg_text.h。除法与取模只在每个字符上做一次，不在弹的逐颗循环里。
******************************************************************************/

#include "stg_text.h"

uint32_t stg_text_draw(gfx_list *l, const stg_font *f, int dst, int x, int y, const char *s,
                       uint32_t *drawn)
{
    uint32_t i, bad = 0u, n = 0u;

    for (i = 0u; s[i] != '\0'; i++) {
        const uint32_t gi = (uint32_t)(uint8_t)s[i] - f->first;

        if (gi < f->cells) {
            const int rc = gfx_blit(l, f->slot,
                                    (int)((gi % f->cols) * f->cell_w),
                                    (int)((gi / f->cols) * f->cell_h),
                                    dst, x, y, (int)f->cell_w, (int)f->cell_h,
                                    GFX_EQ_COPY, 0u, 1, f->key);
            if (rc != 0)
                bad++;
            else
                n++;
        }
        x += (int)f->cell_w;
    }
    if (drawn != 0)
        *drawn += n;
    return bad;
}

uint32_t stg_text_width(const stg_font *f, const char *s)
{
    uint32_t i;

    for (i = 0u; s[i] != '\0'; i++)
        ;
    return i * f->cell_w;
}
