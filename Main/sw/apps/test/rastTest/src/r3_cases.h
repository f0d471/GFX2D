#ifndef R3_CASES_H
#define R3_CASES_H
#include "gfx/rast_setup.h"
#define R3_CASE_COUNT 11u
#define R3_SCENE_RECORDS 16u
#define R3_BACKGROUND 0x12345678u
#define R3_HUD_COLOR 0x00fedcbau
#define R3_KEY 0xff000000u
int r3_scene(unsigned frame, unsigned textured, rast_record *out, unsigned capacity);
uint32_t r3_texel(unsigned word);
#endif
