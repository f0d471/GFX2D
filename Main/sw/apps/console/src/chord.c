/******************************************************************************
* @file chord.c: HOME 和弦检测（纯逻辑；契约见 chord.h）
******************************************************************************/

#include "chord.h"

void chord_init(chord *c, uint32_t combo, uint32_t hold)
{
    c->combo = combo;
    c->hold  = hold;
    c->n     = 0u;
    c->armed = 1;
}

uint32_t chord_step(chord *c, uint32_t keys)
{
    if ((keys & c->combo) != c->combo) {
        c->n     = 0u;
        c->armed = 1;
        return 0u;
    }
    if (!c->armed)
        return 0u;
    if (++c->n < c->hold)
        return 0u;
    c->armed = 0u;
    c->n     = 0u;
    return CHORD_EV_FIRE;
}
