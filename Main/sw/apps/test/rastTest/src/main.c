/******************************************************************************
* @file main.c: R1 双目标入口；板上执行与硬件对拍由 R3 承接
******************************************************************************/
#include "board.h"
#include "gfx/rast_ref.h"

static uint32_t color[640 * 16];
static uint16_t depth[640 * 16];
static _Alignas(32) rast_record records[RAST_MAX_FAN];

int main(void)
{
    fx16_mat4 projection;
    rast_view_vertex v[3] = {{-65536, 65536, -2 * 65536, 0, 0, 0xff804020},
                              {65536, 65536, -2 * 65536, 16, 0, 0xff804020},
                              {-65536, -65536, -2 * 65536, 0, 16, 0xff804020}};
    board_init();
    int fail = fx16_perspective(&projection, 16384, 65536, 65536, 9 * 65536);
    int n = rast_setup_view(v, &projection, 65536, 9 * 65536, RAST_VCOLOR_EN,
                            records, RAST_MAX_FAN, NULL);
    if (n != 1) fail = 1;
    for (unsigned i = 0; i < 640 * 16; ++i) depth[i] = 65535;
    rast_target t = {color, depth, 640, 8, NULL, 0, 0, 0, 0};
    if (n > 0 && rast_ref_run(records, (unsigned)n, &t)) fail = 1;
    if (depth[8 * 640 + 200] == 65535 || color[8 * 640 + 200] != 0x00804020) fail = 1;
    board_puts(fail ? "rastTest fail 1\ndone\n" : "rastTest fail 0\ndone\n");
    return fail ? 1 : 0;
}
