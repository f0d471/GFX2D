/******************************************************************************
* @file main.c: 引擎构型的运行时功能判据
*
* @brief 同一程序检查 DDR 的六方程、色键、多命令读流、错误恢复和行外保护字。
*        TEX 与 PB 按实际 CAPS 执行，CRC 的期望值由外部检查器独立生成。
******************************************************************************/
#include <stdint.h>
#include <string.h>
#include "board.h"
#include "gfx_engine.h"
#include "gfx/gfx.h"
#include "core/crc32.h"

static const uint32_t dst_base = 0x02000000u;
static const uint32_t src_base = 0x00600000u;
static const uint32_t list_base = 0x00400000u;
static int failures;

static void hex32(uint32_t value)
{
    char text[9];
    uint32_t i;
    for (i = 0; i < 8u; i++)
        text[i] = "0123456789abcdef"[(value >> (28u - 4u * i)) & 15u];
    text[8] = '\0';
    board_puts(text);
}

static gfx_cmd packet(uint32_t equation)
{
    gfx_cmd result = {{0}};
    result.w[0] = 0x00010002u | (equation << 8);
    if (equation == GFX_EQ_CONST)
        result.w[0] |= 64u << 24;
    result.w[1] = 1u;
    result.w[2] = 1u;
    result.w[3] = 0x00030006u;
    result.w[7] = 0x5a123456u;
    return result;
}

static void result_line(const char *name, uint32_t crc, uint32_t code,
                        uint32_t index, uint32_t tag, uint32_t bad)
{
    board_puts("CASE "); board_puts(name); board_puts(" crc "); hex32(crc);
    board_puts(" code "); hex32(code); board_puts(" index "); hex32(index);
    board_puts(" tag "); hex32(tag); board_puts(" bad "); hex32(bad);
    board_puts("\n");
    if (bad != 0u)
        failures++;
}

static void run_case(const char *name, gfx_cmd command, uint32_t expected,
                     uint32_t wanted_code, uint32_t count, uint32_t tex, uint32_t format)
{
    volatile uint32_t *dst = (volatile uint32_t *)(uintptr_t)dst_base;
    uint32_t *src = (uint32_t *)(uintptr_t)src_base;
    gfx_cmd *list = (gfx_cmd *)(uintptr_t)list_base;
    uint32_t code = 0xffu, index = 0u, tag = 0u, bad = 0u, i;
    crc32_ctx crc;

    if (gfx_engine_soft_reset() != GFX_ENG_OK) {
        failures++; board_puts("FAIL reset\n"); return;
    }
    for (i = 0; i < 256u; i++) {
        dst[i] = 0x00102030u;
        src[i] = 0x80604020u;
    }
    if (tex && gfx_engine_tex_write(src, 0u, 1024u) != GFX_ENG_OK)
        bad++;
    if (gfx_engine_surf_set(0, dst_base, 64u, 4u, 8u, GFX_FMT_XRGB8888) != GFX_ENG_OK ||
        gfx_engine_surf_set_space(1, tex ? 0u : src_base, 64u, 4u, 8u,
                                  tex, format) != GFX_ENG_OK)
        bad++;
    for (i = 0; i < count; i++)
        list[i] = command;
    memset(&list[count], 0, sizeof(gfx_cmd));
    list[count].w[0] = GFX_OP_END;
    if (gfx_engine_submit(list_base, count + 1u) != GFX_ENG_OK ||
        gfx_engine_wait(&code, &index, &tag) != GFX_ENG_OK)
        bad++;
    if (code != wanted_code || (wanted_code && (index != 0u || tag != command.w[7])))
        bad++;
    board_dcache_invalidate_range((const void *)(uintptr_t)dst_base, 1024u);
    for (i = 0; i < 256u; i++) {
        uint32_t want = !wanted_code && i % 64u >= 1u && i % 64u < 7u && i / 64u < 3u
                      ? expected : 0x00102030u;
        if (dst[i] != want)
            bad++;
    }
    crc32_begin(&crc);
    crc32_update(&crc, (const void *)(uintptr_t)dst_base, 1024u);
    result_line(name, crc32_end(&crc), code, index, tag, bad);
}

static void run_pb(void)
{
    const uint32_t bg_base = 0x02400000u;
    volatile uint32_t *dst = (volatile uint32_t *)(uintptr_t)dst_base;
    uint32_t *bg = (uint32_t *)(uintptr_t)bg_base;
    gfx_cmd *list = (gfx_cmd *)(uintptr_t)list_base;
    gfx_engine_pb params;
    uint32_t code = 0xffu, index = 0u, tag = 0u, bad = 0u, i;
    crc32_ctx crc;

    gfx_engine_soft_reset();
    for (i = 0; i < 16384u; i++) {
        dst[i] = 0x00deadbeu;
        bg[i] = 0x00102030u;
    }
    if (gfx_engine_surf_set(0, dst_base, 640u, 16u, 12u, 0u) != GFX_ENG_OK ||
        gfx_engine_surf_set(1, src_base, 64u, 4u, 8u, 1u) != GFX_ENG_OK ||
        gfx_engine_surf_set(2, bg_base, 640u, 16u, 12u, 0u) != GFX_ENG_OK)
        bad++;
    list[0] = packet(GFX_EQ_COPY);
    memset(&params, 0, sizeof(params));
    params.base = src_base + 0x20000u; params.bytes = 0x20000u;
    params.draw_cap = 16u; params.ref_cap = 1024u;
    params.lstrips = 1u; params.strip_log2 = 4u; params.bg_id = 2u; params.dst_id = 0u;
    if (gfx_engine_submit_pb(list_base, 1u, &params) != GFX_ENG_OK ||
        gfx_engine_wait(&code, &index, &tag) != GFX_ENG_OK || code != 0u)
        bad++;
    board_dcache_invalidate_range((const void *)(uintptr_t)dst_base, 65536u);
    for (i = 0; i < 16384u; i++) {
        uint32_t want = i % 1024u >= 640u ? 0x00deadbeu :
                        i % 1024u >= 1u && i % 1024u < 7u && i / 1024u < 3u
                        ? 0x00604020u : 0x00102030u;
        if (dst[i] != want)
            bad++;
    }
    crc32_begin(&crc);
    crc32_update(&crc, (const void *)(uintptr_t)dst_base, 65536u);
    result_line("pb", crc32_end(&crc), code, index, tag, bad);
}

static void run_strip(void)
{
    const uint32_t bg_base = 0x02400000u;
    volatile uint32_t *dst = (volatile uint32_t *)(uintptr_t)dst_base;
    uint32_t *bg = (uint32_t *)(uintptr_t)bg_base;
    uint32_t *header = (uint32_t *)(uintptr_t)list_base;
    gfx_cmd *list = (gfx_cmd *)(uintptr_t)(list_base + 32768u);
    uint32_t code = 0xffu, index = 0u, tag = 0u, bad = 0u, i;
    crc32_ctx crc;
    int submitted;

    for (i = 0; i < 16384u; i++) {
        dst[i] = 0x00deadbeu; bg[i] = 0x00102030u;
    }
    if (gfx_engine_surf_set(0, dst_base, 640u, 16u, 12u, 0u) != GFX_ENG_OK ||
        gfx_engine_surf_set(1, src_base, 64u, 4u, 8u, 1u) != GFX_ENG_OK ||
        gfx_engine_surf_set(2, bg_base, 640u, 16u, 12u, 0u) != GFX_ENG_OK)
        bad++;
    for (i = 0; i < 32u; i++) header[i] = 0u;
    header[0] = 1u; list[0] = packet(GFX_EQ_COPY);
    submitted = gfx_engine_submit_strips(list_base, 1u, 4u, 2u, 0u);
    if (submitted == GFX_ENG_E_CAPS) {
        if (bad) failures++;
        else board_puts("STRIP absent ok\n");
        return;
    }
    if (submitted != GFX_ENG_OK || gfx_engine_wait(&code, &index, &tag) != GFX_ENG_OK || code)
        bad++;
    board_dcache_invalidate_range((const void *)(uintptr_t)dst_base, 65536u);
    for (i = 0; i < 16384u; i++) {
        uint32_t want = i % 1024u >= 640u ? 0x00deadbeu :
                        i % 1024u >= 1u && i % 1024u < 7u && i / 1024u < 3u
                        ? 0x00604020u : 0x00102030u;
        if (dst[i] != want) bad++;
    }
    crc32_begin(&crc); crc32_update(&crc, (const void *)(uintptr_t)dst_base, 65536u);
    result_line("strip", crc32_end(&crc), code, index, tag, bad);
}

int main(void)
{
    static const uint32_t expected[6] = {
        0x00604020u, 0x00685038u, 0x00706050u,
        0x0024282cu, 0x00060806u, 0x00383028u
    };
    static const char *names[6] = {"copy", "alpha", "add", "const", "mul", "straight"};
    uint32_t caps = 0u, i, started;
    gfx_cmd command;
    board_init(); crc32_init();
    // 留出调试器退出与加载边界记录时间，全部判据输出归属本次加载。
    started = board_cycles();
    while ((uint32_t)(board_cycles() - started) < 150000000u) {}
    board_puts("featureTest\n");
    if (gfx_engine_caps_read(&caps) != GFX_ENG_OK) {
        board_puts("FAIL engine absent\nfail 1\ndone\n"); return 0;
    }
    board_puts("caps "); hex32(caps); board_puts("\n");
    command = packet(0u); command.w[0] = 1u; command.w[1] = 0u; command.w[4] = 0x00a0b0c0u;
    run_case("fill", command, 0x00a0b0c0u, 0u, 1u, 0u, 1u);
    for (i = 0; i < 6u; i++)
        run_case(names[i], packet(i), expected[i], 0u, 1u, 0u, i == 5u ? 2u : 1u);
    command = packet(0u); command.w[0] |= 1u << 12; command.w[5] = 0x80604020u;
    run_case("key", command, 0x00102030u, 0u, 1u, 0u, 1u);
    run_case("sequence", packet(0u), expected[0], 0u, 64u, 0u, 1u);
    command = packet(0u); command.w[0] = 0xfeu;
    run_case("opcode", command, 0u, 1u, 1u, 0u, 1u);
    run_case("recovery", packet(0u), expected[0], 0u, 1u, 0u, 1u);
    command = packet(0u); command.w[0] = 0x00010005u;
    command.w[1] = src_base + 0x10000u; command.w[2] = 1u;
    command.w[5] = 4u; command.w[6] = 1u;
    *(uint32_t *)(uintptr_t)command.w[1] = 1u;
    run_case((caps & GFX_CAPS_INST_M) ? "instance" : "inst-absent", command,
             expected[0], (caps & GFX_CAPS_INST_M) ? 0u : 1u, 1u, 0u, 1u);
    if (caps & GFX_CAPS_TEX_M)
        run_case("tex", packet(0u), expected[0], 0u, 1u, 1u, 1u);
    else {
        uint32_t word = 0u;
        if (gfx_engine_tex_write(&word, 0u, 4u) != GFX_ENG_E_CAPS)
            failures++;
        else board_puts("TEX absent ok\n");
    }
    if (caps & GFX_CAPS_BIN_M)
        run_pb();
    else board_puts("PB absent ok\n");
    run_strip();
    gfx_engine_soft_reset();
    board_puts(failures ? "fail 1\n" : "fail 0\n");
    board_puts("done\n");
    return 0;
}
