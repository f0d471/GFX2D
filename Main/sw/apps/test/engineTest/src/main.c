/******************************************************************************
*
* @file main.c: engineTest —— 引擎 P1 B6 的板上判据
*
* @brief  同一张命令表，软件执行器画一份、引擎画一份，逐行 CRC 比对两个面。
*         覆盖 B6 条目 2.6 节的上板判据：
*
*           CASE x11  SW3 十一个整帧金标准（lib/gfx/cases.c 的同一构建器，
*                     帧缓冲落在 FB0/FB1 两个面）。引擎面与软件面的 CRC 必须相同；
*           COUNT32   用 LIST_COUNT=0x10000 提交 FILL+END；旧驱动的 16 位截断
*                     会变成 COUNT=0、目的像素不变，板上必须实测写入成功；
*           DIR  x5   几何定向与写掩码：目的 x 对 4 取余 1/2/3 的窄矩形（首末拍的
*                     部分掩码）、两端都缺的整行、色键棋盘（中间拍按像素跳过的
*                     稀疏掩码）——S1 发出的这两种掩码在真控制器上从没验过
*                     （探针只发过全掩码），总线地址恒 16 字节对齐（D15）；
*           PERF      16 颗 16×16 预乘 ALPHA，读 PERF_*，每颗周期数与总览估算
*                     约 1100、engine 第 02 章 3.8 节模型 1092~1124 对照；
*           STRESS    E5 引擎侧复测：整屏 FILL、整屏 BLIT、1000 颗弹幕轮换着跑
*                     120 场，扫描 FIFO 最低水位（不得低于 160 拍）、欠载（0）
*                     与 guard 的 SoC 最大地址等待按 platform 第 08 章口径登记；
*           INJECT=1  注错构建：INJ-COH 一行在单条缓存行上跳过 dcache 失效
*                     （E3 规则：CPU 读回引擎写过的区域之前必须失效），
*                     该行必须打 differ FAIL (inject) 且不计入 fail 数。
*                     注错缩到单缓存行的原因见 run_inject_check()：
*                     4KB 的缓存罩不住整面的 CRC，第一版整面注错 NOT_RED。
*
*         期望输出（见 Main/sw/README.md）：每项一行 ok / FAIL，末行 fail N 与 done。
*         正常与 INJECT 构建都要求 fail 0。
*
*         层次说明：display 与 guard 的量测寄存器地址只出现在本文件，
*         与 displayFifoProbe 的做法一致——它们是板上的量测窗口，不是跨平台的
*         平台能力；引擎寄存器一律经 sw/bsp 的 gfx_engine 驱动访问。
******************************************************************************/
#include <stdint.h>
#include <string.h>

#include "board.h"
#include "gfx_engine.h"
#include "gfx/gfx.h"
#include "core/crc32.h"

/* --- DDR 布局（memmap.h 的分区；偏移即 CPU 地址，platform 第 02 章） ------- */
static const uint32_t FB_ENGINE = 0x02000000u;  /* REGION_FB0：引擎画的那个面 */
static const uint32_t FB_SW     = 0x02400000u;  /* REGION_FB1：软件执行器的那个面 */
static const uint32_t TEX_BASE  = 0x00600000u;  /* REGION_SHARED：金标准的纹理 */
static const uint32_t TEX_KEY   = 0x00610000u;  /* 色键棋盘（定向用例） */
static const uint32_t TEX_BUL   = 0x00620000u;  /* 弹幕源格子（16×16 预乘） */
static const uint32_t TEX_FULL  = 0x00680000u;  /* 整屏 BLIT 的源（640×480） */
static const uint32_t CMDLIST   = 0x00400000u;  /* REGION_CMDLIST_A，1MB 对齐 */

#define FB_ROW_BYTES    (FB_WIDTH * 4u)
#define FB_ROWS         FB_HEIGHT

/* --- display / guard 的量测寄存器（E5 复测口径，见文件头） ----------------- */
#define DISP_BASE       0xF810E000u
#define D_CTRL          0x008u
#define D_SNAP_FRAME    0x0E0u
#define D_SNAP_MIN      0x0E4u
#define D_ZERO          0x0E8u
#define DISP_ID_ADDR    0xF810E000u
#define DISP_CTRL_EN    (1u << 0)
#define DISP_CTRL_SRCFB (1u << 1)
#define DISP_CTRL_CLEAR (1u << 2)
#define GUARD_BASE      0xF810F000u
#define G_MAX_AR        0x01Cu
#define G_MAX_AW        0x020u
#define G_CLEAR         0x024u

static uint32_t rd32(uint32_t addr)
{
    return *(volatile uint32_t *)(uintptr_t)addr;
}

static void wr32(uint32_t addr, uint32_t v)
{
    *(volatile uint32_t *)(uintptr_t)addr = v;
}

static int   fails = 0;
static int   checks = 0;

static void ok_line(const char *what, int pass)
{
    board_puts(what);
    board_puts(pass ? " ok\n" : " FAIL\n");
    checks++;
    if (!pass)
        fails++;
}

/* CPU 写不分配缓存行（写穿过）：想让「缓存里留着引擎写入前的旧内容」成立，
   必须显式读一遍把行装进去——INJ-COH 的注错靠它（见 run_inject_check）。 */
static volatile uint32_t prime_sink;

/* 逐行 CRC（sw 第 03 章 3.6 节口径：每行取前 640×4 字节，跳过行跨的间隙） */
static uint32_t crc_face(uint32_t base)
{
    crc32_ctx c;
    uint32_t y;

    crc32_begin(&c);
    for (y = 0; y < FB_ROWS; y++)
        crc32_update(&c, (const void *)(uintptr_t)(base + y * FB_STRIDE_BYTES),
                     FB_ROW_BYTES);
    return crc32_end(&c);
}

/* 整面填同一条确定性图案（比对盖全部 480 行，两个面的初始内容必须逐字相同） */
static void fill_face(uint32_t base)
{
    uint32_t *p = (uint32_t *)(uintptr_t)base;
    uint32_t i, n = (FB_STRIDE_BYTES / 4u) * FB_ROWS;

    for (i = 0; i < n; i++)
        p[i] = i * 0x9E3779B9u + (i >> 3);
}

/* 把表面表登记进引擎（只登记 ctrl 有 valid 位的槽） */
static int engine_load_table(const gfx_surface_table *t)
{
    int s;

    for (s = 0; s < (int)GFX_SURF_N; s++) {
        uint32_t ctrl = t->s[s].ctrl;
        if ((ctrl & 0x80000000u) == 0u)
            continue;
        if (gfx_engine_surf_set(s, t->s[s].base,
                                t->s[s].dim & 0xFFFFu, t->s[s].dim >> 16,
                                ctrl & 0x1Fu, (ctrl >> 12) & 0xFu) != GFX_ENG_OK)
            return -1;
    }
    return 0;
}

/* 登记表面表并提交一张表（可带第二张，set-clip-state 用例）等结束，
   返回引擎错误码 */
static uint32_t engine_run(const gfx_surface_table *t,
                           const gfx_cmd *cmds, uint32_t n1, uint32_t n2)
{
    uint32_t code = 0u, eidx, etag;

    if (engine_load_table(t) != 0)
        return 0xFFu;
    memcpy((void *)(uintptr_t)CMDLIST, cmds, (size_t)(n1 + n2) * 32u);
    if (gfx_engine_submit(CMDLIST, n1) != GFX_ENG_OK)
        return 0xFFu;
    if (gfx_engine_wait(&code, &eidx, &etag) != GFX_ENG_OK || code != 0u)
        return (code == 0u) ? 0xFFu : code;
    if (n2 != 0u) {
        if (gfx_engine_submit(CMDLIST + n1 * 32u, n2) != GFX_ENG_OK)
            return 0xFFu;
        if (gfx_engine_wait(&code, &eidx, &etag) != GFX_ENG_OK || code != 0u)
            return (code == 0u) ? 0xFFu : code;
    }
    return 0u;
}

/* D53 的 LIST_COUNT 是 32 位。第一条 FILL、第二条 END，使大 COUNT 可以安全
   提前结束；若驱动把 0x10000 截成 0，像素不变，本判据必红。 */
static void run_count32(void)
{
    static gfx_surface_table tbl;
    static gfx_cmd cmds[2];
    volatile uint32_t *dst = (volatile uint32_t *)(uintptr_t)(FB_ENGINE + 0x800u);
    gfx_surface s;
    gfx_list l;
    gfx_rect r;
    uint32_t code = 0xFFu, eidx, etag;
    int ok;

    *dst = 0u;
    gfx_surface_reset(&tbl);
    s.base = FB_ENGINE + 0x800u; s.width = 4u; s.height = 1u;
    s.stride_log2 = 8u; s.space = GFX_SPACE_DDR; s.format = GFX_FMT_XRGB8888;
    gfx_surface_set(&tbl, 0, &s);
    gfx_list_begin(&l, cmds, 2u, &tbl);
    r.x0 = 0; r.y0 = 0; r.x1 = 0; r.y1 = 0;
    gfx_fill(&l, 0, r, 0x00445566u, GFX_EQ_COPY, 0u);
    gfx_list_end(&l);
    memcpy((void *)(uintptr_t)CMDLIST, cmds, 2u * sizeof(gfx_cmd));
    ok = (l.used == 2u) && engine_load_table(&tbl) == 0 &&
         gfx_engine_submit(CMDLIST, 0x10000u) == GFX_ENG_OK &&
         gfx_engine_wait(&code, &eidx, &etag) == GFX_ENG_OK && code == 0u;
    board_dcache_invalidate_range((const void *)(uintptr_t)dst, 64u);
    ok_line("COUNT32", ok && *dst == 0x00445566u);
}

/* --- 金标准整帧 ----------------------------------------------------------- */

static void run_golden(uint32_t idx)
{
    static gfx_surface_table tbl_eng, tbl_sw;
    static gfx_cmd cmds[2u * GFX_CASE_MAX_CMD];
    static gfx_cmd cmds_sw[2u * GFX_CASE_MAX_CMD];
    gfx_mem m;
    exec_sw_state es;
    exec_sw_error err;
    uint32_t n1, n2, n1b, n2b, c_eng, c_sw, ecode;
    char line[96];
    const char *name = gfx_case_name(idx);

    /* 引擎面：构建器把初始内容画进 FB0 与共享纹理 */
    m.dev_base = 0u; m.host = NULL; m.bytes = 0x03000000u;
    n1 = gfx_case_build(idx, &tbl_eng, &m, FB_ENGINE, TEX_BASE, cmds, &n2);
    /* 软件面：同一构建器对 FB1 再画一遍（纹理内容相同，FB 的初始内容一致） */
    n1b = gfx_case_build(idx, &tbl_sw, &m, FB_SW, TEX_BASE, cmds_sw, &n2b);
    if (!n1 || n1 != n1b || n2 != n2b) {
        (void)name;
        ok_line("BUILD", 0);
        return;
    }

    ecode = engine_run(&tbl_eng, cmds, n1, n2);
    if (ecode != 0u) {
        ok_line("ENG-ERR", 0);
        return;
    }
    /* 软件面：与引擎同一份记录流（两张表的用例按同一顺序提交，
       SET_CLIP 状态跨提交保持，与引擎侧一致） */
    exec_sw_reset(&es);
    if (exec_sw_run(&es, &tbl_sw, &m, cmds_sw, n1b, n1b, NULL, &err) ||
        (n2b != 0u && exec_sw_run(&es, &tbl_sw, &m, cmds_sw + n1b, n2b, n2b, NULL, &err))) {
        ok_line("SW-ERR", 0);
        return;
    }
    /* E3 规则：引擎写过的面，CPU 读回之前必须失效（失效晚于写入完成）。
       注错构建也不在这里例外——整面罩不进 4KB 的缓存，跳过失效看不到差异，
       E3 的注错缩到单缓存行，见 run_inject_check()。 */
    board_dcache_invalidate_range((const void *)(uintptr_t)FB_ENGINE, FB_STRIDE_BYTES * FB_ROWS);
    c_eng = crc_face(FB_ENGINE);
    c_sw = crc_face(FB_SW);

    checks++;
    {
        static const char hex[] = "0123456789abcdef";
        char *p = line;
        int i;
        *p++ = ' ';
        for (i = 28; i >= 0; i -= 4) *p++ = hex[(c_eng >> i) & 0xFu];
        *p++ = ' ';
        for (i = 28; i >= 0; i -= 4) *p++ = hex[(c_sw >> i) & 0xFu];
        *p = '\0';
    }
    board_puts(name);
    board_puts(line);
    if (c_eng == c_sw)
        board_puts(" ok\n");
    else {
        board_puts(" FAIL\n");
        fails++;
    }
}

/* --- E3 注错：单缓存行上的失效判据 ---------------------------------------- */

/* 目的 4 个字落在同一条缓存行。E3 的「不失效读到旧值」在整帧 CRC 上做不了：
   数据缓存一共 64 行 × 64 字节 = 4KB，罩不住 1.9MB 的帧缓冲，预读会把自己
   逐出，跳过失效后读回照样 miss 到新值（第一版注错因此 NOT_RED）。
   缩到一条缓存行：预读装行 → 引擎写 → （注错构建跳过失效）→ 读回。 */
static void run_inject_check(void)
{
    static gfx_surface_table tbl;
    static gfx_cmd cmds[2];
    const uint32_t off = 0x400u;    /* 帧内偏移，落在一条未用的缓存行上 */
    volatile uint32_t *pe = (volatile uint32_t *)(uintptr_t)(FB_ENGINE + off);
    gfx_list l;
    gfx_surface s;
    gfx_rect r;
    uint32_t i, acc = 0u, ecode, c_eng, c_sw, same;

    fill_face(FB_ENGINE);
    fill_face(FB_SW);
    for (i = 0; i < 16u; i++)       /* 预读这一行：写不分配行，得先读才进缓存 */
        acc ^= pe[i];
    prime_sink = acc;

    gfx_surface_reset(&tbl);
    s.base = FB_ENGINE + off; s.width = 4u; s.height = 1u;
    s.stride_log2 = 8u; s.space = GFX_SPACE_DDR; s.format = GFX_FMT_XRGB8888;
    gfx_surface_set(&tbl, 0, &s);
    gfx_list_begin(&l, cmds, 2u, &tbl);
    r.x0 = 0; r.y0 = 0; r.x1 = 3; r.y1 = 0;
    gfx_fill(&l, 0, r, 0xC0DE00F1u, GFX_EQ_COPY, 0u);
    gfx_list_end(&l);
    ecode = engine_run(&tbl, cmds, l.used, 0u);
    if (ecode != 0u) {
        ok_line("INJ-COH", 0);
        return;
    }
    {
        gfx_mem m;
        exec_sw_state es;
        exec_sw_error err;
        m.dev_base = 0u; m.host = NULL; m.bytes = 0x03000000u;
        gfx_surface_reset(&tbl);
        s.base = FB_SW + off;
        gfx_surface_set(&tbl, 0, &s);
        gfx_list_begin(&l, cmds, 2u, &tbl);
        gfx_fill(&l, 0, r, 0xC0DE00F1u, GFX_EQ_COPY, 0u);
        gfx_list_end(&l);
        exec_sw_reset(&es);
        if (exec_sw_run(&es, &tbl, &m, cmds, l.used, l.used, NULL, &err)) {
            ok_line("INJ-COH", 0);
            return;
        }
    }
#ifndef INJECT_1
    board_dcache_invalidate_range((const void *)(uintptr_t)pe, 16u);
#endif
    c_eng = crc_face(FB_ENGINE);    /* 全面 CRC：命中行读旧值即与软件面不同 */
    c_sw = crc_face(FB_SW);
    same = (c_eng == c_sw);
    checks++;
#ifdef INJECT_1
    board_puts("INJ-COH differ");
    if (!same) {
        board_puts(" FAIL (inject)\n");
    } else {
        board_puts(" NOT_RED (inject)\n");
        fails++;
    }
#else
    ok_line("INJ-COH", same);
#endif
}

/* --- 定向与写掩码 --------------------------------------------------------- */

#define KEY_COLOR   0x80ABCDEFu

/* 色键棋盘：每两个像素一拍，与 4 像素的写拍错开，中间拍必然出现稀疏掩码 */
static void fill_key_tex(void)
{
    uint32_t *p = (uint32_t *)(uintptr_t)TEX_KEY;
    uint32_t i, n = (64u * 64u);

    for (i = 0; i < n; i++) {
        uint32_t x = i & 63u;
        p[i] = ((x >> 1) & 1u) ? KEY_COLOR : 0x12345678u;
    }
}

static void run_directed(const char *name,
                         void (*build)(gfx_list *l, int dst),
                         uint32_t tex_init_base, uint32_t tex_init_words)
{
    static gfx_surface_table tbl_eng, tbl_sw;
    static gfx_cmd cmds[64];
    gfx_list l;
    gfx_surface s;
    uint32_t c_eng, c_sw, ecode, i;

    /* 两个面各建一份表（目的槽分别指 FB0/FB1），命令表逐字相同 */
    gfx_surface_reset(&tbl_eng);
    s.base = FB_ENGINE; s.width = FB_WIDTH; s.height = FB_HEIGHT;
    s.stride_log2 = FB_STRIDE_LOG2; s.space = GFX_SPACE_DDR;
    s.format = GFX_FMT_XRGB8888;
    gfx_surface_set(&tbl_eng, 0, &s);
    s.base = tex_init_base; s.width = 64u; s.height = 64u;
    s.stride_log2 = 8u; s.format = GFX_FMT_ARGB8888_PM;
    gfx_surface_set(&tbl_eng, 1, &s);
    tbl_sw = tbl_eng;
    tbl_sw.s[0].base = FB_SW;

    if (tex_init_base != 0u) {
        uint32_t *p = (uint32_t *)(uintptr_t)tex_init_base;
        for (i = 0; i < tex_init_words; i++)
            p[i] = i * 0x85EBCA6Bu + (i >> 2);
    }
    fill_key_tex();
    fill_face(FB_ENGINE);
    fill_face(FB_SW);

    gfx_list_begin(&l, cmds, 64u, &tbl_eng);
    build(&l, 0);
    gfx_list_end(&l);

    ecode = engine_run(&tbl_eng, cmds, l.used, 0u);
    if (ecode != 0u) {
        ok_line(name, 0);
        return;
    }
    board_dcache_invalidate_range((const void *)(uintptr_t)FB_ENGINE, FB_STRIDE_BYTES * FB_ROWS);
    c_eng = crc_face(FB_ENGINE);
    {
        gfx_mem m;
        exec_sw_state es;
        exec_sw_error err;
        m.dev_base = 0u; m.host = NULL; m.bytes = 0x03000000u;
        exec_sw_reset(&es);
        if (exec_sw_run(&es, &tbl_sw, &m, cmds, l.used, l.used, NULL, &err)) {
            ok_line(name, 0);
            return;
        }
    }
    c_sw = crc_face(FB_SW);
    ok_line(name, c_eng == c_sw);
}

static void dir_narrow(gfx_list *l, int dst)
{
    /* 目的 x 对 4 取余 1/2/3：首拍的部分掩码各不相同 */
    gfx_blit(l, 1, 0, 0, dst, 1, 100, 5, 3, GFX_EQ_COPY, 0u, 0, 0u);
    gfx_blit(l, 1, 8, 0, dst, 2, 110, 6, 3, GFX_EQ_COPY, 0u, 0, 0u);
    gfx_blit(l, 1, 16, 0, dst, 3, 120, 7, 3, GFX_EQ_COPY, 0u, 0, 0u);
    /* 首末拍都缺、中间整拍的整行 */
    gfx_blit(l, 1, 24, 0, dst, 1, 130, 638, 8, GFX_EQ_COPY, 0u, 0, 0u);
}

static void dir_key(gfx_list *l, int dst)
{
    gfx_blit(l, 1, 0, 0, dst, 10, 200, 620, 4, GFX_EQ_COPY, 0u, 1, KEY_COLOR);
}

/* --- 性能（16×16 预乘 ALPHA 弹幕）----------------------------------------- */

static void run_perf_sprites(void)
{
    static gfx_surface_table tbl;
    static gfx_cmd cmds[64];
    static uint32_t perf[GFX_ENG_PERF_N];
    gfx_list l;
    gfx_surface s;
    uint32_t *bul = (uint32_t *)(uintptr_t)TEX_BUL;
    uint32_t i, code, eidx, etag, busy;

    for (i = 0; i < 64u * 64u; i++) {
        uint32_t a = 0x80u, v = i * 0x9E3779B9u;
        bul[i] = (a << 24) | ((v >> 8 & 0x7Fu) << 16) | ((v >> 16 & 0x7Fu) << 8) | (v >> 24 & 0x7Fu);
    }
    fill_face(FB_ENGINE);

    gfx_surface_reset(&tbl);
    s.base = FB_ENGINE; s.width = FB_WIDTH; s.height = FB_HEIGHT;
    s.stride_log2 = FB_STRIDE_LOG2; s.space = GFX_SPACE_DDR;
    s.format = GFX_FMT_XRGB8888;
    gfx_surface_set(&tbl, 0, &s);
    s.base = TEX_BUL; s.width = 64u; s.height = 64u;
    s.stride_log2 = 8u; s.format = GFX_FMT_ARGB8888_PM;
    gfx_surface_set(&tbl, 1, &s);

    gfx_list_begin(&l, cmds, 64u, &tbl);
    for (i = 0; i < 16u; i++)
        gfx_blit(&l, 1, (int)((i % 4u) * 16u), (int)((i / 4u) * 16u), 0,
                 (int)(20u + i * 37u), (int)(20u + (i % 4u) * 40u), 16, 16,
                 GFX_EQ_ALPHA, 0u, 0, 0u);
    gfx_list_end(&l);

    if (engine_load_table(&tbl) != 0) {
        ok_line("PERF-ENG", 0);
        return;
    }
    memcpy((void *)(uintptr_t)CMDLIST, cmds, (size_t)l.used * 32u);
    gfx_engine_perf_clear();
    if (gfx_engine_submit(CMDLIST, l.used) != GFX_ENG_OK ||
        gfx_engine_wait(&code, &eidx, &etag) != GFX_ENG_OK || code != 0u) {
        ok_line("PERF-ENG", 0);
        return;
    }
    gfx_engine_perf_read(perf, GFX_ENG_PERF_N);
    busy = perf[GFX_PERF_BUSY];
    /* 通过线放宽到 [900,1500) 防粗大回归；精确数（模型 1092~1124、B4 实测
       1104.3、整帧仿真 1116）的对账写进 TechReports engine 第 06 章 */
    ok_line("PERF-BUSY", busy >= 900u * 16u && busy < 1500u * 16u);
    {
        static const char hex[] = "0123456789abcdef";
        char hexline[24];
        char *p = hexline;
        int sh;
        *p++ = 'P'; *p++ = 'E'; *p++ = 'R'; *p++ = 'F';
        *p++ = ' '; *p++ = 'b'; *p++ = 'u'; *p++ = 's'; *p++ = 'y'; *p++ = '=';
        for (sh = 28; sh >= 0; sh -= 4) *p++ = hex[(busy >> sh) & 0xFu];
        *p = '\0';
        board_puts(hexline);
        board_puts("\n");
    }
}

/* --- E5 引擎侧复测（扫描 FIFO 水位 + SoC 地址等待）------------------------ */

#define STRESS_FIELDS   120u

/* 逐张提交一张表并等完成；返回引擎错误码（0 = 无错） */
static uint32_t submit_wait(const gfx_cmd *cmds, uint32_t n)
{
    uint32_t code = 0u, eidx, etag;

    memcpy((void *)(uintptr_t)CMDLIST, cmds, (size_t)n * 32u);
    if (gfx_engine_submit(CMDLIST, n) != GFX_ENG_OK)
        return 0xFFu;
    if (gfx_engine_wait(&code, &eidx, &etag) != GFX_ENG_OK)
        return 0xFFu;
    return code;
}

static void run_stress(void)
{
    static gfx_surface_table tbl;
    static gfx_cmd fill_cmds[4];
    static gfx_cmd blit_cmds[4];
    static gfx_cmd bul_cmds[1100];
    gfx_list l;
    gfx_surface s;
    uint32_t *full = (uint32_t *)(uintptr_t)TEX_FULL;
    uint32_t i, f;
    uint32_t min_wm = 0xFFFFFFFFu, zero, max_ar, max_aw;
    uint32_t snap_prev, snap_now;
    int pass;

    if (rd32(DISP_ID_ADDR) != 0x44495350u) {
        board_puts("STRESS no-display\n");
        ok_line("STRESS", 0);
        return;
    }
    /* 统计从这一刻起算：CLEAR 与使能一起写（board.c 的同一做法，
       上一次会话留下的状态不清掉，量就不是本次运行的） */
    wr32(DISP_BASE + D_CTRL, DISP_CTRL_EN | DISP_CTRL_SRCFB | DISP_CTRL_CLEAR);
    wr32(GUARD_BASE + G_CLEAR, 1u);

    /* 三张最重的表，目的都是正在扫描的 FB0：
       整屏 FILL、整屏 BLIT（一行两笔读）、1000 颗 16×16 预乘 ALPHA */
    gfx_surface_reset(&tbl);
    s.base = FB_ENGINE; s.width = FB_WIDTH; s.height = FB_HEIGHT;
    s.stride_log2 = FB_STRIDE_LOG2; s.space = GFX_SPACE_DDR;
    s.format = GFX_FMT_XRGB8888;
    gfx_surface_set(&tbl, 0, &s);
    gfx_list_begin(&l, fill_cmds, 4u, &tbl);
    {
        gfx_rect r;
        r.x0 = 0; r.y0 = 0; r.x1 = (int)FB_WIDTH - 1; r.y1 = (int)FB_HEIGHT - 1;
        gfx_fill(&l, 0, r, 0x00101820u, GFX_EQ_COPY, 0u);
    }
    gfx_list_end(&l);

    for (i = 0; i < (FB_STRIDE_BYTES / 4u) * FB_HEIGHT; i++)
        full[i] = i * 0x2545F491u + (i >> 2);
    s.base = TEX_FULL; s.width = FB_WIDTH; s.height = FB_HEIGHT;
    s.stride_log2 = FB_STRIDE_LOG2; s.format = GFX_FMT_XRGB8888;
    gfx_surface_set(&tbl, 1, &s);
    gfx_list_begin(&l, blit_cmds, 4u, &tbl);
    gfx_blit(&l, 1, 0, 0, 0, 0, 0, (int)FB_WIDTH, (int)FB_HEIGHT,
             GFX_EQ_COPY, 0u, 0, 0u);
    gfx_list_end(&l);

    s.base = TEX_BUL; s.width = 64u; s.height = 64u;
    s.stride_log2 = 8u; s.format = GFX_FMT_ARGB8888_PM;
    gfx_surface_set(&tbl, 1, &s);
    gfx_list_begin(&l, bul_cmds, 1100u, &tbl);
    for (i = 0; i < 1000u; i++)
        gfx_blit(&l, 1, (int)((i & 3u) * 16u), (int)((i >> 2 & 3u) * 16u), 0,
                 (int)(i % 600u), (int)(i % 460u), 16, 16,
                 GFX_EQ_ALPHA, 0u, 0, 0u);
    gfx_list_end(&l);

    /* 跑 120 场：每场三张表各一次，场边界对齐（SNAP_MIN 是每场起始的快照） */
    snap_prev = rd32(DISP_BASE + D_SNAP_FRAME);
    for (f = 0; f < STRESS_FIELDS; f++) {
        if (submit_wait(fill_cmds, 2u) != 0u ||
            submit_wait(blit_cmds, 2u) != 0u ||
            submit_wait(bul_cmds, 1001u) != 0u) {
            ok_line("STRESS-ENG", 0);
            return;
        }
        /* 等下一个场起始，收这一场的最低水位快照 */
        do {
            snap_now = rd32(DISP_BASE + D_SNAP_FRAME);
        } while (snap_now == snap_prev);
        snap_prev = snap_now;
        {
            uint32_t wm = rd32(DISP_BASE + D_SNAP_MIN);
            if (wm < min_wm)
                min_wm = wm;
        }
    }
    zero = rd32(DISP_BASE + D_ZERO);
    max_ar = rd32(GUARD_BASE + G_MAX_AR);
    max_aw = rd32(GUARD_BASE + G_MAX_AW);

    pass = (min_wm >= 160u) && (zero == 0u);
    ok_line("STRESS", pass);
    {
        static const char hex[] = "0123456789abcdef";
        char line[80];
        char *p;
        int sh;
        p = line;
        *p++ = ' ';
        *p++ = 'w';
        *p++ = 'm';
        *p++ = '=';
        for (sh = 28; sh >= 0; sh -= 4) *p++ = hex[(min_wm >> sh) & 0xFu];
        *p++ = ' ';
        *p++ = 'z';
        *p++ = '=';
        for (sh = 28; sh >= 0; sh -= 4) *p++ = hex[(zero >> sh) & 0xFu];
        *p++ = ' ';
        *p++ = 'a';
        *p++ = 'r';
        *p++ = '=';
        for (sh = 28; sh >= 0; sh -= 4) *p++ = hex[(max_ar >> sh) & 0xFu];
        *p++ = ' ';
        *p++ = 'a';
        *p++ = 'w';
        *p++ = '=';
        for (sh = 28; sh >= 0; sh -= 4) *p++ = hex[(max_aw >> sh) & 0xFu];
        *p = '\0';
        board_puts(line);
        board_puts("\n");
    }
}

/* --- 入口 ------------------------------------------------------------------ */

int main(void)
{
    uint32_t i;

    board_init();
    board_puts("engineTest B6\n");
    crc32_init();

    if (!gfx_engine_present()) {
        board_puts("ENG absent\n");
        board_puts("fail 1\n");
        board_puts("done\n");
        return 0;
    }
    if (gfx_engine_soft_reset() != GFX_ENG_OK) {
        board_puts("ENG reset FAIL\n");
        board_puts("fail 1\n");
        board_puts("done\n");
        return 0;
    }
    board_puts("ENG id ok\n");
    checks++;

    for (i = 0; i < GFX_CASE_N; i++)
        run_golden(i);

    run_count32();
    run_directed("DIR narrow-x1x2x3", dir_narrow, TEX_BASE + 0x400u, 64u * 16u);
    run_directed("DIR key-sparse", dir_key, TEX_BASE + 0x800u, 64u * 16u);
    run_inject_check();

    run_perf_sprites();
    run_stress();

    /* 收尾把引擎留在干净态（软复位清表面表与 DONE/ERR），并恢复扫描输出 */
    gfx_engine_soft_reset();

    {
        char line[24];
        char *p = line;
        int n = fails;
        *p++ = 'f'; *p++ = 'a'; *p++ = 'i'; *p++ = 'l'; *p++ = ' ';
        if (n == 0) *p++ = '0';
        else {
            char buf[8];
            int m = 0;
            while (n > 0) { buf[m++] = (char)('0' + n % 10); n /= 10; }
            while (m > 0) *p++ = buf[--m];
        }
        *p++ = '\n'; *p = '\0';
        board_puts(line);
    }
    board_puts("done\n");
    return 0;
}
