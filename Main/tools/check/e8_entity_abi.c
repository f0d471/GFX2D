/* 独立 ABI 对照：实体路径生成的 BLIT 必须与 gfx_blit 字节相同。 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "core/entity.h"
#include "gfx/gfx.h"

_Static_assert(EB_SCREEN_W == GFX_SCREEN_W && EB_SCREEN_H == GFX_SCREEN_H,
               "E8 游戏区必须与命令 ABI 一致");

int main(void)
{
    gfx_surface_table tab;
    gfx_surface src = { 0x00100000u, 96u, 96u, 9u, GFX_SPACE_DDR, GFX_FMT_XRGB8888 };
    gfx_surface dst = { 0x00200000u, 640u, 480u, 12u, GFX_SPACE_DDR, GFX_FMT_XRGB8888 };
    gfx_cmd ref[2];
    uint32_t raw[EB_CMD_WORDS];
    uint32_t sprite;

    gfx_surface_reset(&tab);
    if (gfx_surface_set(&tab, EB_SLOT_SRC, &src) != GFX_OK ||
        gfx_surface_set(&tab, EB_SLOT_DST, &dst) != GFX_OK)
        return 2;

    for (sprite = 0u; sprite < 64u; sprite++) {
        eb_ent e = { 100 * EB_ONE, 120 * EB_ONE, 0, 0, (uint16_t)sprite, 1u };
        gfx_list list;
        uint32_t rng = 0x2468ace1u;

        memset(raw, 0xA5, sizeof raw);
        gfx_list_begin(&list, ref, 2u, &tab);
        if (eb_frame_aos_cmd(&e, 1u, &rng, raw, 0) != 1u ||
            gfx_blit(&list, EB_SLOT_SRC, (int)(sprite & 7u) * 12,
                     (int)(sprite >> 3) * 12, EB_SLOT_DST, 100, 120,
                     12, 12, GFX_EQ_COPY, 0u, 1, 0x00123456u) != GFX_OK ||
            list.used != 1u || memcmp(raw, ref[0].w, sizeof raw) != 0) {
            fprintf(stderr, "BLIT ABI 不一致: sprite=%u\n", sprite);
            return 1;
        }
    }
    {
        eb_ent e[2] = {
            { 100 * EB_ONE, 120 * EB_ONE, 0, 0, 0u, 1u },
            { 101 * EB_ONE, 121 * EB_ONE, 0, 0, 63u, 1u }
        };
        gfx_list list;
        uint32_t raw_pair[2u * EB_CMD_WORDS];
        uint32_t rng = 0x2468ace1u;

        gfx_list_begin(&list, ref, 2u, &tab);
        if (eb_frame_aos_cmd(e, 2u, &rng, raw_pair, 0) != 2u ||
            gfx_blit(&list, EB_SLOT_SRC, 0, 0, EB_SLOT_DST, 100, 120,
                     12, 12, GFX_EQ_COPY, 0u, 1, 0x00123456u) != GFX_OK ||
            gfx_blit(&list, EB_SLOT_SRC, 84, 84, EB_SLOT_DST, 101, 121,
                     12, 12, GFX_EQ_COPY, 0u, 1, 0x00123456u) != GFX_OK ||
            list.used != 2u || memcmp(raw_pair, ref, sizeof raw_pair) != 0) {
            fputs("BLIT ABI 多记录索引不一致\n", stderr);
            return 1;
        }
    }
    {
        static const int32_t xy[8][2] = {
            { EB_DEAD_X_LO - 1, 0 }, { EB_DEAD_X_HI + 1, 0 },
            { 0, EB_DEAD_Y_LO - 1 }, { 0, EB_DEAD_Y_HI + 1 },
            { EB_DEAD_X_LO, 0 }, { EB_DEAD_X_HI, 0 },
            { 0, EB_DEAD_Y_LO }, { 0, EB_DEAD_Y_HI }
        };
        uint32_t i;

        for (i = 0u; i < 8u; i++) {
            eb_ent e = { xy[i][0], xy[i][1], 0, 0, 0u, 1u };
            uint32_t mem_words[(EB_SOA_BYTES(1) + 3u) / 4u];
            int32_t *x, *y, *vx, *vy;
            uint16_t *spr, *flags;
            uint32_t rng_a = 0x2468ace1u, rng_s = 0x2468ace1u;
            uint32_t resp_a = 0u, resp_s = 0u;
            const uint32_t expected = i < 4u ? 1u : 0u;

            eb_soa_split(mem_words, 1u, &x, &y, &vx, &vy, &spr, &flags);
            x[0] = xy[i][0]; y[0] = xy[i][1];
            vx[0] = 0; vy[0] = 0; spr[0] = 0u; flags[0] = 1u;
            eb_frame_aos(&e, 1u, &rng_a, &resp_a);
            eb_frame_soa(x, y, vx, vy, spr, flags, 1u, &rng_s, &resp_s);
            if (resp_a != expected || resp_s != expected ||
                eb_digest_aos(&e, 1u) != eb_digest_soa(x, y, vx, vy, spr, flags, 1u)) {
                fprintf(stderr, "AoS/SoA 出屏边界不一致: case=%u\n", i);
                return 1;
            }
        }
    }
    puts("e8 entity BLIT ABI 64/64 + two-record label + x/y bounds 8/8 ok");
    return 0;
}
