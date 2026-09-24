/* E7 前期研究：色键命中率的**主机侧**独立复核。
 *
 * 板上 execSwTest 量到的 384x256 色键窗口命中 47616 个像素，
 * 而「8x8 棋盘格的一半」算出来是 49152，差 1536。差从哪来必须先排除
 * 「板上执行器与主机侧不一致」这一条，再决定是改判据还是查代码。
 *
 * 这里直接编 lib/gfx 的执行器，用一块内存当 DDR，铺同一份棋盘格，
 * 量几种窗口尺寸下的命中数，并与逐像素的期望值对照。
 *
 *   gcc -I Main/sw/lib Main/tools/check/e7_key_probe.c Main/sw/lib/gfx/{list,check,exec_sw}.c
 *       Main/sw/lib/core/{format,crc32}.c -o /tmp/e7_key_probe
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx/gfx.h"

#define KEY_COLOR 0x00123456u

static uint8_t *g_mem;
static uint32_t g_bytes = 4u * 1024u * 1024u;

static void poke(uint32_t dev, uint32_t v)
{
    if (dev + 4u > g_bytes)
        return;
    *(uint32_t *)(void *)(g_mem + dev) = v;
}

static uint32_t peek(uint32_t dev)
{
    if (dev + 4u > g_bytes)
        return 0u;
    return *(uint32_t *)(void *)(g_mem + dev);
}

/* 与板上 seed_tex 的 XRGB + 色键分支逐字节相同。 */
static void seed(uint32_t base, uint32_t w, uint32_t h, uint32_t sl)
{
    uint32_t x, y;

    for (y = 0u; y < h; y++) {
        for (x = 0u; x < w; x++) {
            const uint32_t rr = (x * 7u + y * 3u) & 0xFFu;
            const uint32_t gg = (y * 5u + 17u) & 0xFFu;
            const uint32_t bb = (x + y * 2u) & 0xFFu;
            uint32_t v = (rr << 16) | (gg << 8) | bb;

            if ((((x >> 2) + (y >> 2)) & 1u) != 0u)
                v = KEY_COLOR;
            poke(base + (y << sl) + (x << 2), v);
        }
    }
}

static int surf(gfx_surface_table *t, int id, uint32_t base,
                uint32_t w, uint32_t h, uint32_t sl)
{
    gfx_surface s;

    s.base = base; s.width = (uint16_t)w; s.height = (uint16_t)h;
    s.stride_log2 = (uint8_t)sl; s.space = GFX_SPACE_DDR;
    s.format = GFX_FMT_XRGB8888;
    return gfx_surface_set(t, id, &s);
}

/* 逐像素算一遍期望命中数：不依赖执行器的任何实现。 */
static uint32_t expect_key(uint32_t x0, uint32_t y0, uint32_t w, uint32_t h)
{
    uint32_t n = 0u, x, y;

    for (y = y0; y < y0 + h; y++)
        for (x = x0; x < x0 + w; x++)
            if ((((x >> 2) + (y >> 2)) & 1u) != 0u)
                n++;
    return n;
}

int main(void)
{
    static const uint32_t cases[][4] = {
        {   0,   0,   8,   8 },
        {   0,   0,  64,  64 },
        {   4,   4,  12,  12 },
        {   0,   0, 384, 256 },
        { 128,  96, 384, 256 },
        {   1,   1, 383, 255 },
        {   3,   7,  12,  12 }
    };
    const uint32_t src_base = 0x00000000u;
    const uint32_t dst_base = 0x00200000u; /* 与 0..1MB 源纹理不重叠 */
    uint32_t i, failures = 0u;

    g_mem = malloc(g_bytes);
    if (g_mem == NULL)
        return 2;
    memset(g_mem, 0, g_bytes);

    seed(src_base, 512u, 512u, 11u);

    for (i = 0u; i < sizeof cases / sizeof cases[0]; i++) {
        const uint32_t x0 = cases[i][0], y0 = cases[i][1];
        const uint32_t w = cases[i][2], h = cases[i][3];
        gfx_surface_table t;
        gfx_mem           m;
        exec_sw_state     es;
        exec_sw_stats     st;
        gfx_list          l;
        gfx_cmd           c[4];
        uint32_t          exp = expect_key(x0, y0, w, h);
        int               rc;

        memset(g_mem + dst_base, 0, 480u << 12);
        gfx_surface_reset(&t);
        (void)surf(&t, 0, dst_base, 640u, 480u, 12u);
        (void)surf(&t, 1, src_base, 512u, 512u, 11u);

        m.dev_base = 0u;
        m.host     = g_mem;
        m.bytes    = g_bytes;

        exec_sw_reset(&es);
        exec_sw_stats_clear(&st);
        gfx_list_begin(&l, c, 4u, &t);
        (void)gfx_blit(&l, 1, (int)x0, (int)y0, 0, 200, 100,
                       (int)w, (int)h, GFX_EQ_COPY, 0u, 1, KEY_COLOR);
        (void)gfx_list_end(&l);

        rc = exec_sw_run(&es, &t, &m, c, l.used, l.used, &st, NULL);

        printf("win %3u,%3u %3ux%3u rc %d px %u wr %u ksk %u oob %u"
               "  expect_keys %u  diff %d\n",
               x0, y0, w, h, rc, st.pixel_total, st.pixel_written,
               st.key_skip, st.src_oob, exp,
               (int)st.key_skip - (int)exp);
        if (rc != GFX_OK || st.key_skip != exp || st.src_oob != 0u)
            failures++;

        /* 独立复核：被写回的目的像素里不许有键色。 */
        {
            uint32_t x, y, bad = 0u;

            for (y = 100u; y < 100u + h; y++) {
                for (x = 200u; x < 200u + w; x++) {
                    uint32_t d = peek(dst_base + (y << 12) + (x << 2));

                    if (d == KEY_COLOR)
                        bad++;
                }
            }
            if (bad != 0u) {
                printf("   ** 目的面里有 %u 个键色像素被写进去（应为 0）\n", bad);
                failures++;
            }
        }
    }

    free(g_mem);
    return failures == 0u ? 0 : 1;
}
