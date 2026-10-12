/******************************************************************************
* @file stg_chord.c: 模式框架的和弦检测
*
* @brief  口径见 stg_chord.h。
******************************************************************************/

#include "stg_chord.h"

void stg_chord_init(stg_chord *c)
{
    c->ud_n     = 0u;
    c->lr_n     = 0u;
    c->ud_armed = 1u;
    c->lr_armed = 1u;
}

/* 一组和弦：两个键都按住时计数，满 STG_CHORD_HOLD 帧且可以触发就触发一次；
   两个键都松开才重新可以触发。只按住其中一个时计数清零，但不重新武装。 */
static int one(uint32_t keys, uint32_t a, uint32_t b, uint32_t *n, uint32_t *armed)
{
    const int both = (keys & a) != 0u && (keys & b) != 0u;
    const int none = (keys & a) == 0u && (keys & b) == 0u;

    if (none)
        *armed = 1u;
    if (!both) {
        *n = 0u;
        return 0;
    }
    if (*n < STG_CHORD_HOLD)
        (*n)++;
    if (*n >= STG_CHORD_HOLD && *armed) {
        *armed = 0u;
#ifdef INJECT_CHORD_RETRIGGER
        *armed = 1u;                /* 注错：不等松开 */
        *n     = 0u;
#endif
        return 1;
    }
    return 0;
}

uint32_t stg_chord_step(stg_chord *c, uint32_t keys, uint32_t up, uint32_t down,
                        uint32_t left, uint32_t right)
{
    uint32_t ev = 0u;

    if (one(keys, up, down, &c->ud_n, &c->ud_armed))
        ev |= STG_CHORD_NEXT_MODE;
    if (one(keys, left, right, &c->lr_n, &c->lr_armed))
        ev |= STG_CHORD_SWAP;
    return ev;
}
