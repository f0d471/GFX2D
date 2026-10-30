#ifndef CONSOLE_PRESENT_H
#define CONSOLE_PRESENT_H
#include <stdint.h>
#include "game/stg_draw.h"
/* Content presentation policy; BSP only owns canvases, windows and swaps. */
/* 0: configured render surface; 1: fixed legacy/diagnostic path; <0: error. */
int console_present_begin(gfx_surface_table *surfaces,int index,stg_draw_view *view);
int console_present_request(int index,uint32_t *ticket);
#endif
