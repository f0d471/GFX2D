#ifndef GFX2D_R4_CUP_H
#define GFX2D_R4_CUP_H
#include "gfx/rast_setup.h"
#define R4_CUP_INPUT_TRIANGLES 396u
/* Shared geometry for both executors. Uses all 207 indexed vertices/396 faces. */
int r4_cup_emit(uint32_t tick, rast_record *out, unsigned capacity);
#endif
