/* Shared product HUD geometry. Simulation and legacy replay layouts are separate. */
#ifndef MYRIAD_HUD_H
#define MYRIAD_HUD_H
#include <stdint.h>
#ifndef GFX_PRODUCT_HUD
#define GFX_PRODUCT_HUD 0
#endif
#define MYRIAD_HUD_WIDTH 160u
#define MYRIAD_HUD_X 480
#define MYRIAD_HUD_TEXT_X 492
#define MYRIAD_HUD_RIGHT 628
#define MYRIAD_HUD_BG 0x00050912u
#define MYRIAD_HUD_LINE 0x0035f2ffu
#define MYRIAD_HUD_RULE 0x00224a5cu
/* Seventeen 8px glyphs fit between the two 12px margins. Paired rows reserve
 * seven glyphs for a label and up to ten for a right-aligned uint32 value. */
static inline void myriad_hud_fit(char *out,const char *text,unsigned columns)
{
    unsigned i=0;
    while(i<columns && text[i]) {out[i]=text[i];++i;}
    out[i]=0;
}
static inline int myriad_hud_value_x(const char *text)
{
    unsigned n=0;while(text[n] && n<10u)++n;
    return MYRIAD_HUD_RIGHT-(int)n*8;
}
#endif
