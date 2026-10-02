/* sw6_frames_dump：SW6 场景 A 的整帧命令表导出器（引擎 P1 B6 的仿真输入）
 *
 * 与 pix_cases_dump --frame 同一套文件约定（init/cmd/surf/exp/cnt/stats/meta），
 * 命令表来自游戏本体的场景 A（sw6_scene_a.c，SW6 验收时的同一份代码）：
 * 每帧一条全屏 FILL 打底（SW6_BG_COLOR）加 sw6_a_build 追加的方块命令，
 * 连续两帧（第二帧画在第一帧的结果上）。HUD、双缓冲与录像回放不在这张表里，
 * 它们属应用层；B6 的判据是「引擎画出的帧与软件执行器逐字相同」，导出器把
 * 软件执行器跑完的期望内存一并给出。
 *
 * 素材内容是本工具自己生成的确定性图案（图集与棋盘的格子内容不影响场景 A 的
 * 命令形状，sw6_a_build 只按格子号取源坐标），不复用 assets.pak：
 * 仿真输入要在任何一台机器上逐位可复现，不依赖素材打包步骤。
 *
 *   sw6_frames_dump PREFIX_PREFIX   导出两帧到 PREFIX0 与 PREFIX1（帧 0、帧 1）
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx/gfx.h"
#include "board.h"

#include "sw6_scene_a.h"
#include "sw6_cfg.h"

/* 设备窗口与 SW3 整帧用例同一布局（GFX_CASE_*，见 gfx.h）：帧缓冲在前，
   图集与棋盘放进帧缓冲结束（0x1E1000）到纹理基址（0x200000）之间的空档，
   整个窗口仍是 0x210000 字节，与整帧用例共用同一张从机。 */
#define DEV_BYTES       GFX_CASE_DEV_BYTES
#define ATLAS_BASE      0x001F0000u
#define CHECKER_BASE    0x001FC000u
#define ATLAS_W         128u    /* 8 列 × 16 像素，48 个格子 = 8 × 6 */
#define ATLAS_H         96u
#define ATLAS_SLOG2     9u      /* 128 × 4 = 512 = 2^9 */
#define CHECKER_W       96u     /* 3 列 × 32 像素 */
#define CHECKER_H       32u
#define CHECKER_SLOG2   9u
#define LIST_CAP        (2u + SW6_POOL_MAX)     /* 打底 FILL + 全池 BLIT/FILL，含 END */

static void write_words(const char *name, const uint8_t *mem, uint32_t bytes)
{
    FILE *f = fopen(name, "w");
    uint32_t i;
    if (!f) { perror(name); exit(2); }
    for (i = 0; i < bytes/16u; i++) {
        const uint32_t *w = (const uint32_t *)(const void *)(mem + 16u*i);
        fprintf(f, "%08x%08x%08x%08x\n", w[3], w[2], w[1], w[0]);
    }
    fclose(f);
}

static void write_commands(const char *name, const gfx_cmd *cmd, uint32_t n)
{
    FILE *f = fopen(name, "w");
    uint32_t i, j;
    if (!f) { perror(name); exit(2); }
    for (i = 0; i < n; i++) {
        for (j = 8; j != 0; j--) fprintf(f, "%08x", cmd[i].w[j-1]);
        fputc('\n', f);
    }
    fclose(f);
}

static void write_surfaces(const char *name, const gfx_surface_table *t)
{
    FILE *f = fopen(name, "w");
    uint32_t i;
    if (!f) { perror(name); exit(2); }
    for (i = 0; i < GFX_SURF_N; i++)
        fprintf(f, "%08x%08x%08x\n", t->s[i].ctrl, t->s[i].dim, t->s[i].base);
    fclose(f);
}

static void file_name(char *dst, size_t size, const char *prefix, const char *ext)
{
    if (snprintf(dst, size, "%s.%s", prefix, ext) >= (int)size) {
        fprintf(stderr, "output path too long\n"); exit(2);
    }
}

/* 确定性初始图案：与 pix_cases_dump 的定向用例同一族 LCG，保证任何机器逐位相同。 */
static void fill_pattern(uint8_t *mem, uint32_t base, uint32_t bytes)
{
    uint32_t i;
    for (i = 0; i < bytes/4u; i++) {
        uint32_t v = (base/4u + i) * 0x9e3779b9u + (i >> 3);
        v ^= v >> 13; v *= 0x85ebca6bu; v ^= v >> 16;
        ((uint32_t *)(void *)(mem + base))[i] = v;
    }
}

/* 跑一张表并导出这一帧的全部文件。init 取 buf 的当前内容，期望是执行后的 buf。 */
static void export_frame(const char *prefix, uint8_t *buf, const gfx_surface_table *t,
                         const gfx_cmd *cmds, uint32_t n,
                         exec_sw_state *es, exec_sw_stats *stotal)
{
    gfx_mem m;
    exec_sw_stats st;
    exec_sw_error err;
    char name[512];
    FILE *f;

    m.dev_base = 0; m.host = buf; m.bytes = DEV_BYTES;

    file_name(name, sizeof(name), prefix, "init.hex");
    write_words(name, buf, DEV_BYTES);
    file_name(name, sizeof(name), prefix, "cmd.hex");
    write_commands(name, cmds, n);
    file_name(name, sizeof(name), prefix, "surf.hex");
    write_surfaces(name, t);

    exec_sw_stats_clear(&st);
    if (exec_sw_run(es, t, &m, cmds, n, n, &st, &err)) {
        fprintf(stderr, "%s: software executor error %d at %u\n",
                prefix, err.code, err.index); exit(2);
    }
    stotal->pixel_total += st.pixel_total;
    stotal->src_oob     += st.src_oob;
    stotal->key_skip    += st.key_skip;

    file_name(name, sizeof(name), prefix, "exp.hex");
    write_words(name, buf, DEV_BYTES);
    file_name(name, sizeof(name), prefix, "cnt.hex");
    f = fopen(name, "w");
    if (!f) { perror(name); exit(2); }
    fprintf(f, "%08x\n%08x\n%08x\n", n, 0u, DEV_BYTES/16u);
    fclose(f);
    file_name(name, sizeof(name), prefix, "stats.hex");
    f = fopen(name, "w");
    if (!f) { perror(name); exit(2); }
    fprintf(f, "%08x\n%08x\n%08x\n", st.pixel_total, st.src_oob, st.key_skip);
    fclose(f);
    file_name(name, sizeof(name), prefix, "meta.txt");
    f = fopen(name, "w");
    if (!f) { perror(name); exit(2); }
    fprintf(f, "name=%s\nfirst_list=%u\nsecond_list=%u\n", prefix, n, 0u);
    fprintf(f, "pixels=%u src_oob=%u key=%u\n", st.pixel_total, st.src_oob, st.key_skip);
    fclose(f);
    printf("SW6 FRAME %s: commands=%u pixels=%u written=%u\n",
           prefix, n, st.pixel_total, st.pixel_written);
}

int main(int argc, char **argv)
{
    uint8_t *buf;
    gfx_surface_table table;
    static gfx_cmd cmds[LIST_CAP];
    gfx_list list;
    sw6_scene_a scene;
    sw6_build_stat fs;
    exec_sw_state es;
    exec_sw_stats total;
    gfx_rect full;

    if (argc != 3) {
        fprintf(stderr, "usage: sw6_frames_dump <帧0前缀> <帧1前缀>\n");
        return 2;
    }
    buf = (uint8_t *)calloc(1u, DEV_BYTES);
    if (!buf) return 2;

    /* 初始图案铺满整个窗口，图集与棋盘再用各自的格子图案盖一遍：
       图集按 16×16 格子给递增的花样，ALPHA 方程要求预乘（每字节通道不大于 A）。 */
    fill_pattern(buf, 0, DEV_BYTES);
    {
        uint32_t cx, cy;
        for (cy = 0; cy < ATLAS_H/16u; cy++)
            for (cx = 0; cx < ATLAS_W/16u; cx++) {
                uint32_t x, y;
                uint8_t a = (uint8_t)(0x30u + 8u*(cx + cy));
                for (y = 0; y < 16u; y++)
                    for (x = 0; x < 16u; x++) {
                        uint8_t *px = buf + ATLAS_BASE
                                    + ((cy*16u + y) << ATLAS_SLOG2) + ((cx*16u + x) << 2);
                        px[0] = (uint8_t)(a/2u + x*3u);     /* B */
                        px[1] = (uint8_t)(a/2u + y*5u);     /* G */
                        px[2] = (uint8_t)(a/2u + cx*7u);    /* R */
                        px[3] = a;                          /* A（预乘基准） */
                    }
            }
        for (cy = 0; cy < CHECKER_H/32u; cy++)
            for (cx = 0; cx < CHECKER_W/32u; cx++) {
                uint32_t x, y;
                uint32_t k = ((cx + cy) & 1u) ? 0xFF2060A0u : 0xFFA06020u;
                for (y = 0; y < 32u; y++)
                    for (x = 0; x < 32u; x++)
                        *(uint32_t *)(void *)(buf + CHECKER_BASE
                            + ((cy*32u + y) << CHECKER_SLOG2) + ((cx*32u + x) << 2)) = k;
            }
    }

    gfx_surface_reset(&table);
    {
        gfx_surface s;
        s.base = GFX_CASE_FB_BASE; s.width = FB_WIDTH; s.height = FB_HEIGHT;
        s.stride_log2 = GFX_CASE_FB_STRIDE_LOG2;
        s.space = GFX_SPACE_DDR; s.format = GFX_FMT_XRGB8888;
        if (gfx_surface_set(&table, SW6_SLOT_FB0, &s)) goto bad_surface;
        s.base = CHECKER_BASE; s.width = CHECKER_W; s.height = CHECKER_H;
        s.stride_log2 = CHECKER_SLOG2;
        s.format = GFX_FMT_XRGB8888;
        if (gfx_surface_set(&table, SW6_SLOT_CHECKER, &s)) goto bad_surface;
        s.base = ATLAS_BASE; s.width = ATLAS_W; s.height = ATLAS_H;
        s.stride_log2 = ATLAS_SLOG2;
        s.format = GFX_FMT_ARGB8888_PM;
        if (gfx_surface_set(&table, SW6_SLOT_ATLAS, &s)) goto bad_surface;
    }

    exec_sw_reset(&es);
    exec_sw_stats_clear(&total);
    full.x0 = 0; full.y0 = 0; full.x1 = (int)FB_WIDTH - 1; full.y1 = (int)FB_HEIGHT - 1;

    {
        const char *prefix[2] = { argv[1], argv[2] };
        uint32_t i;

        sw6_a_begin(&scene, SW6_SEED_DEMO, SW6_N_DEFAULT);
        for (i = 0; i < 2u; i++) {
            if (i != 0u)
                sw6_a_update(&scene);
            gfx_list_begin(&list, cmds, LIST_CAP, &table);
            if (gfx_fill(&list, SW6_SLOT_FB0, full, SW6_BG_COLOR, GFX_EQ_COPY, 0u))
                goto bad_command;
            sw6_a_build(&scene, &list, SW6_SLOT_FB0, &fs);
            if (gfx_list_end(&list))
                goto bad_command;
            if (fs.bad != 0u) {
                fprintf(stderr, "frame %u: %u commands rejected by the builder\n", i, fs.bad);
                return 2;
            }
            export_frame(prefix[i], buf, &table, cmds, list.used, &es, &total);
        }
    }
    printf("SW6 FRAMES: total pixels=%u src_oob=%u key=%u\n",
           total.pixel_total, total.src_oob, total.key_skip);
    free(buf);
    return 0;

bad_surface:
    fprintf(stderr, "surface setup failed\n");
    return 2;
bad_command:
    fprintf(stderr, "command construction failed at index %u\n", list.used);
    return 2;
}
