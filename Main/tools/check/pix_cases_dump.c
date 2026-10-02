/* pix_cases_dump：引擎 P1 B4 的金标准导出器
 *
 * 链接 sw/lib/gfx（list.c 造命令、check.c 检查、exec_sw.c 执行），导出初始内存、
 * 表面表、命令表与软件执行器跑完之后的期望内存，给 sim/engine/pix/tb_engine_path.v
 * 喂 B1~B4 的真实通路。输出是 $readmemh 能读的 128 位字，按总线字节序。
 *
 *   pix_cases_dump PREFIX              定向用例：目的表面每行都跨 4KB；六个方程的 FILL
 *                                      （ALPHA、ALPHA_STRAIGHT 的常量 alpha 不是 255）；
 *                                      源、目的相位 4×4 的 BLIT；XRGB 源配 ALPHA 类方程；
 *                                      色键与「只有高字节不同」的近似字；源越界；奇偶行数的多行命令
 *   pix_cases_dump --hazard PREFIX     跨命令的写后读：上一条命令最后一行还没写回，下一条命令
 *                                      就读它的尾部（两组：640 像素两行的 ALPHA、整行两行的 FILL，
 *                                      各接一颗 16 像素的 ALPHA_STRAIGHT）
 *   pix_cases_dump --sprites PREFIX    16 颗互不重叠的 16×16 ALPHA 弹幕（量两种提交方式的周期数）
 *   pix_cases_dump --frame N PREFIX    SW3 十一个整帧用例之一（0..10），附命令表边界的元数据
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gfx/gfx.h"

#define MEM_BYTES 0x80000u
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
    if (snprintf(dst, size, "%s.%s.hex", prefix, ext) >= (int)size) {
        fprintf(stderr, "output path too long\n"); exit(2);
    }
}

static int set_surface(gfx_surface_table *t, int id, uint32_t base, int fmt)
{
    gfx_surface s;
    s.base = base; s.width = 640; s.height = 32;
    s.stride_log2 = 12; s.space = GFX_SPACE_DDR; s.format = (uint8_t)fmt;
    return gfx_surface_set(t, id, &s);
}

static void write_counts(const char *prefix, uint32_t n1, uint32_t n2,
                         const exec_sw_stats *stats, uint32_t window_words)
{
    char name[512];
    FILE *f;

    /* cnt/stats 是 B6 整帧仿真的机器可读输入（$readmemh）：两张表的条数、
       软件执行器的统计与本用例的设备窗口（128 位字数）。meta.txt 供人读与
       run_frames.sh 的 CRC 核对，两者并存。 */
    if (snprintf(name, sizeof(name), "%s.cnt.hex", prefix) >= (int)sizeof(name)) exit(2);
    f = fopen(name, "w");
    if (!f) { perror(name); exit(2); }
    fprintf(f, "%08x\n%08x\n%08x\n", n1, n2, window_words);
    fclose(f);

    if (snprintf(name, sizeof(name), "%s.stats.hex", prefix) >= (int)sizeof(name)) exit(2);
    f = fopen(name, "w");
    if (!f) { perror(name); exit(2); }
    fprintf(f, "%08x\n%08x\n%08x\n", stats->pixel_total, stats->src_oob, stats->key_skip);
    fclose(f);
}

static int export_frame(uint32_t idx, const char *prefix)
{
    uint8_t *buf = (uint8_t *)calloc(1u, GFX_CASE_DEV_BYTES);
    gfx_mem m;
    gfx_surface_table table;
    gfx_cmd cmds[2u * GFX_CASE_MAX_CMD];
    exec_sw_state es;
    exec_sw_stats stats;
    exec_sw_error err;
    char name[512];
    uint32_t n1, n2;
    FILE *meta;

    if (!buf || idx >= GFX_CASE_N) { free(buf); return 2; }
    m.dev_base = 0; m.host = buf; m.bytes = GFX_CASE_DEV_BYTES;
    n1 = gfx_case_build(idx, &table, &m, GFX_CASE_FB_BASE,
                        GFX_CASE_TEX_BASE, cmds, &n2);
    if (!n1 || n1 > GFX_CASE_MAX_CMD || n2 > GFX_CASE_MAX_CMD) {
        fprintf(stderr, "invalid frame command count\n"); free(buf); return 2;
    }
    file_name(name, sizeof(name), prefix, "init");
    write_words(name, buf, GFX_CASE_DEV_BYTES);
    file_name(name, sizeof(name), prefix, "cmd");
    write_commands(name, cmds, n1+n2);
    file_name(name, sizeof(name), prefix, "surf");
    write_surfaces(name, &table);
    exec_sw_reset(&es); exec_sw_stats_clear(&stats);
    if (exec_sw_run(&es, &table, &m, cmds, n1, n1, &stats, &err) ||
        (n2 && exec_sw_run(&es, &table, &m, cmds+n1, n2, n2, &stats, &err))) {
        fprintf(stderr, "frame %u software executor error %d at %u\n",
                idx, err.code, err.index); free(buf); return 2;
    }
    file_name(name, sizeof(name), prefix, "exp");
    write_words(name, buf, GFX_CASE_DEV_BYTES);
    write_counts(prefix, n1, n2, &stats, GFX_CASE_DEV_BYTES/16u);
    if (snprintf(name, sizeof(name), "%s.meta.txt", prefix) >= (int)sizeof(name)) {
        free(buf); return 2;
    }
    meta = fopen(name, "w");
    if (!meta) { perror(name); free(buf); return 2; }
    fprintf(meta, "name=%s\nfirst_list=%u\nsecond_list=%u\n",
            gfx_case_name(idx), n1, n2);
    fprintf(meta, "pixels=%u src_oob=%u key=%u\n",
            stats.pixel_total, stats.src_oob, stats.key_skip);
    fclose(meta);
    printf("FRAME %u %s: lists=%u+%u pixels=%u written=%u\n",
           idx, gfx_case_name(idx), n1, n2, stats.pixel_total, stats.pixel_written);
    free(buf);
    return 0;
}

int main(int argc, char **argv)
{
    uint8_t *buf;
    gfx_mem m;
    gfx_surface_table table;
    gfx_cmd cmds[64];
    gfx_list list;
    exec_sw_state es;
    exec_sw_stats stats;
    exec_sw_error err;
    char name[512];
    uint32_t i, v;
    int eq, ls, ld, src;
    gfx_rect rect;

    if (argc == 4 && strcmp(argv[1], "--frame") == 0) {
        char *end;
        unsigned long idx = strtoul(argv[2], &end, 10);
        if (!argv[2][0] || *end || idx >= GFX_CASE_N) {
            fprintf(stderr, "frame index must be 0..%u\n", GFX_CASE_N-1); return 2;
        }
        return export_frame((uint32_t)idx, argv[3]);
    }
    int hazard = 0, sprites = 0;
    if (argc == 3 && strcmp(argv[1], "--hazard") == 0) {
        hazard = 1; argv++; argc--;
    } else if (argc == 3 && strcmp(argv[1], "--sprites") == 0) {
        sprites = 1; argv++; argc--;
    }
    if (argc != 2) {
        fprintf(stderr, "usage: pix_cases_dump PREFIX | --hazard PREFIX | --sprites PREFIX | --frame 0..10 PREFIX\n");
        return 2;
    }
    buf = (uint8_t *)malloc(MEM_BYTES);
    if (!buf) return 2;
    for (i = 0; i < MEM_BYTES/4u; i++) {
        v = i * 0x9e3779b9u + (i >> 3);
        v ^= v >> 13; v *= 0x85ebca6bu; v ^= v >> 16;
        if (i*4u < 0x30000u) v &= 0x00ffffffu;
        if (i*4u >= 0x30000u && (i % 7u) == 0u) v = 0x80abcdefu;      /* 色键字 */
        else if (i*4u >= 0x30000u && (i % 11u) == 0u) v = 0x7fabcdefu; /* 近似字：必须照写 */
        ((uint32_t *)(void *)buf)[i] = v;
    }
    m.dev_base = 0; m.host = buf; m.bytes = MEM_BYTES;
    gfx_surface_reset(&table);
    if (set_surface(&table, 0, 0x1ff4u, GFX_FMT_XRGB8888) ||
        set_surface(&table, 1, 0x30008u, GFX_FMT_XRGB8888) ||
        set_surface(&table, 2, 0x4000cu, GFX_FMT_ARGB8888_PM) ||
        set_surface(&table, 3, 0x50004u, GFX_FMT_ARGB8888_ST)) {
        fprintf(stderr, "surface setup failed\n"); return 2;
    }
    gfx_list_begin(&list, cmds, 64, &table);
    if (hazard) {
        /* 第一组：两行 640 像素的 ALPHA（第 26、27 行），接一颗画在第 27 行尾部的 16 像素 */
        if (gfx_blit(&list, 2, 0, 0, 0, 0, 26, 640, 2, GFX_EQ_ALPHA, 0, 0, 0) ||
            gfx_blit(&list, 3, 0, 5, 0, 600, 27, 16, 1, GFX_EQ_ALPHA_STRAIGHT, 0, 0, 0))
            goto bad_command;
        /* 第二组：两行整行 FILL（第 28、29 行），接一颗画在第 29 行尾部的 16 像素 */
        rect.x0 = 0; rect.y0 = 28; rect.x1 = 639; rect.y1 = 29;
        if (gfx_fill(&list, 0, rect, 0x37112233u, GFX_EQ_COPY, 0) ||
            gfx_blit(&list, 3, 0, 6, 0, 610, 29, 16, 1, GFX_EQ_ALPHA_STRAIGHT, 0, 0, 0))
            goto bad_command;
        goto list_done;
    }
    if (sprites) {
        /* 源取预乘表面的前 16 行，目的第 8~23 行一字排开，彼此不重叠 */
        for (i = 0; i < 16u; i++)
            if (gfx_blit(&list, 2, (int)(i * 17u) % 600, 0, 0, 20 + (int)i * 37, 8, 16, 16,
                         GFX_EQ_ALPHA, 0, 0, 0)) goto bad_command;
        goto list_done;
    }
    for (eq = 0; eq < 6; eq++) {
        rect.x0 = (eq*3) % 4; rect.y0 = eq;
        rect.x1 = rect.x0 + 3 + eq*5 - 1; rect.y1 = eq;
        /* ALPHA 的常量要满足预乘约束；ALPHA 类两个方程的 alpha 都不是 255，
           否则 a 取错来源也看不出来。其余方程不看 alpha。 */
        uint32_t k = (eq == GFX_EQ_ALPHA) ? 0x80402011u
                   : (eq == GFX_EQ_ALPHA_STRAIGHT) ? 0x80a35c2cu
                   : 0x37a35c27u + (uint32_t)eq;
        if (gfx_fill(&list, 0, rect, k,
                     eq, (uint8_t)(eq == GFX_EQ_CONST ? 31 + eq*35 : 0)))
            goto bad_command;
    }
    for (ls = 0; ls < 4; ls++) for (ld = 0; ld < 4; ld++) {
        eq = (ls*4+ld) % 6;
        src = (eq == GFX_EQ_ALPHA) ? 2 :
              (eq == GFX_EQ_ALPHA_STRAIGHT) ? 3 : 1;
        if (gfx_blit(&list, src, ls, 6+ls*4+ld, 0, ld, 6+ls*4+ld,
                     17+((ls+ld)%4), 1, eq,
                     (uint8_t)(eq == GFX_EQ_CONST ? 17+ls*41+ld*13 : 0),
                     ((ls+ld)&1), 0x80abcdefu)) goto bad_command;
    }
    if (gfx_blit(&list, 1, 0, 22, 0, 0, 22, 32, 3,
                 GFX_EQ_COPY, 0, 1, 0x80abcdefu)) goto bad_command;
    if (gfx_blit(&list, 2, 0, 23, 0, 0, 23, 640, 1,
                 GFX_EQ_ALPHA, 0, 0, 0)) goto bad_command;
    if (gfx_blit(&list, 3, 638, 24, 0, 15, 24, 16, 1,
                 GFX_EQ_ALPHA_STRAIGHT, 0, 0, 0)) goto bad_command;
    /* XRGB 源配 ALPHA 类方程：源字高字节是随机的，a 必须取 255 */
    if (gfx_blit(&list, 1, 5, 27, 0, 3, 25, 23, 1,
                 GFX_EQ_ALPHA, 0, 0, 0)) goto bad_command;
    if (gfx_blit(&list, 1, 2, 28, 0, 1, 26, 19, 1,
                 GFX_EQ_ALPHA_STRAIGHT, 0, 0, 0)) goto bad_command;
    /* 多行命令：偶数行、奇数行、偶数行，行缓冲两半在命令内交替 */
    rect.x0 = 5; rect.y0 = 27; rect.x1 = 40; rect.y1 = 28;
    if (gfx_fill(&list, 0, rect, 0x37102030u, GFX_EQ_ADD, 0)) goto bad_command;
    if (gfx_blit(&list, 1, 1, 3, 0, 2, 29, 29, 3,
                 GFX_EQ_CONST, 99, 1, 0x80abcdefu)) goto bad_command;
    if (gfx_blit(&list, 3, 7, 9, 0, 100, 27, 38, 4,
                 GFX_EQ_MUL, 0, 0, 0)) goto bad_command;
list_done:
    if (gfx_list_end(&list)) goto bad_command;

    file_name(name, sizeof(name), argv[1], "init"); write_words(name, buf, MEM_BYTES);
    file_name(name, sizeof(name), argv[1], "cmd"); write_commands(name, cmds, list.used);
    file_name(name, sizeof(name), argv[1], "surf"); write_surfaces(name, &table);
    exec_sw_reset(&es); exec_sw_stats_clear(&stats);
    if (exec_sw_run(&es, &table, &m, cmds, list.used, list.used, &stats, &err)) {
        fprintf(stderr, "software executor error %d at %u\n", err.code, err.index);
        return 2;
    }
    file_name(name, sizeof(name), argv[1], "exp"); write_words(name, buf, MEM_BYTES);
    write_counts(argv[1], list.used, 0u, &stats, MEM_BYTES/16u);
    printf("PIX CASES: commands=%u pixels=%u written=%u key=%u src_oob=%u\n",
           list.used, stats.pixel_total, stats.pixel_written,
           stats.key_skip, stats.src_oob);
    free(buf);
    return 0;

bad_command:
    fprintf(stderr, "command construction failed at index %u\n", list.used);
    return 2;
}
