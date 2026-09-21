/******************************************************************************
* @file test_gfx.c: 构造层与软件执行器的主机侧测试
*
* @brief  七组：编解码往返、构造失败、单像素算术、裁剪、内建向量、
*         导出向量的重放、整帧用例与 CRC。
*
*         每一组都同时跑产品实现 `exec_sw` 与独立参考实现 `ref_exec`，
*         两者的错误三元组、统计与内存必须逐项相同。
*
*         打开任一个方程的注错宏之后，整帧那一组改为报告每个用例的 CRC
*         与 golden 相同还是不同：用到该方程的用例必须不同，其余必须相同。
*         注错构建下不再断言两个实现一致 —— 注错只加在产品实现上，两边本来就该不一样。
******************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx/gfx.h"
#include "core/crc32.h"
#include "core/format.h"
#include "core/xorshift32.h"
#include "ref_exec.h"
#include "vec_builtin.h"
#include "vec_replay.h"

#if defined(INJECT_EQ_COPY) || defined(INJECT_EQ_ALPHA) || defined(INJECT_EQ_ADD) \
 || defined(INJECT_EQ_CONST) || defined(INJECT_EQ_MUL) || defined(INJECT_EQ_ALPHA_STRAIGHT)
#define GFX_INJECTED 1
#else
#define GFX_INJECTED 0
#endif

/* 整帧用例的设备内存布局。帧缓冲按板上的 stride 4096 排，
   纹理放在它后面，两者都在同一块窗口里。
   三个常量在 gfx.h 里，判据程序（SW4 的 platformTest）用的是同一组值。 */
#define CASE_FB_BASE   GFX_CASE_FB_BASE
#define CASE_TEX_BASE  GFX_CASE_TEX_BASE
#define CASE_DEV_BYTES GFX_CASE_DEV_BYTES

static uint32_t g_fail;

static void say(const char *s)
{
    fputs(s, stdout);
}

static void ck(const char *tag, int ok)
{
    char     line[128];
    core_out o;

    core_out_init(&o, line, (uint32_t)sizeof line);
    core_out_str(&o, tag);
    core_out_str(&o, ok ? " ok\n" : " FAIL\n");
    say(line);
    if (!ok)
        g_fail++;
}

static void ck_u32(const char *tag, uint32_t expect, uint32_t got)
{
    char     line[160];
    core_out o;

    core_out_init(&o, line, (uint32_t)sizeof line);
    core_out_str(&o, tag);
    core_out_str(&o, " expect=");
    core_out_hex32(&o, expect);
    core_out_str(&o, " got=");
    core_out_hex32(&o, got);
    core_out_str(&o, (expect == got) ? " ok\n" : " FAIL\n");
    say(line);
    if (expect != got)
        g_fail++;
}

/* --- T6 编解码往返 ------------------------------------------------------ */

/* 构造层打包完的记录，用一份独立的解包逐字段对回去。
   两边都错成同一个样子才会漏，而解包这一份是照 3.4 节的位表现写的。 */
static void t6_roundtrip(void)
{
    gfx_surface_table t;
    gfx_surface       s;
    gfx_cmd           buf[64];
    gfx_list          l;
    xs32              rng;
    uint32_t          i, bad = 0u, n = 0u;

    gfx_surface_reset(&t);
    s.base = 0x1000u; s.width = 64u; s.height = 64u;
    s.stride_log2 = 8u; s.space = (uint8_t)GFX_SPACE_DDR;
    s.format = (uint8_t)GFX_FMT_XRGB8888;
    (void)gfx_surface_set(&t, 0, &s);
    s.base = 0x9000u; s.format = (uint8_t)GFX_FMT_ARGB8888_PM;
    (void)gfx_surface_set(&t, 1, &s);

    xs32_seed(&rng, 0x12345678u);

    for (i = 0u; i < 100000u; i++) {
        int      sx, sy, dx, dy, w, h, eq;
        uint32_t key = xs32_next(&rng) & 0x00FFFFFFu;
        int      ken = (int)(xs32_next(&rng) & 1u);

        if ((i % 60u) == 0u)
            gfx_list_begin(&l, buf, 60u, &t);

        sx = (int)(xs32_next(&rng) % 64u);
        sy = (int)(xs32_next(&rng) % 64u);
        dx = (int)(xs32_next(&rng) % 600u);
        dy = (int)(xs32_next(&rng) % 400u);
        w  = (int)(xs32_next(&rng) % 32u);
        h  = (int)(xs32_next(&rng) % 32u);
        eq = (int)(xs32_next(&rng) % 5u);        /* 0..4，避开要 STRAIGHT 源的 5 */

        if (gfx_blit(&l, 1, sx, sy, 0, dx, dy, w, h, eq, 0u, ken, key) != GFX_OK)
            continue;

        {
            const gfx_cmd *r = &buf[l.used - 1u];

            n++;
            if ((r->w[0] & 0xFFu) != GFX_OP_BLIT)                     bad++;
            if (((r->w[0] >> 8) & 0xFu) != (uint32_t)eq)              bad++;
            if (((r->w[0] >> 12) & 1u) != (uint32_t)(ken != 0))       bad++;
            if (((r->w[0] >> 16) & 0xFu) != 1u)                       bad++;
            if (((r->w[0] >> 20) & 0xFu) != 0u)                       bad++;
            if ((int)(int16_t)(uint16_t)(r->w[1] & 0xFFFFu) != sx)    bad++;
            if ((int)(int16_t)(uint16_t)(r->w[1] >> 16) != sy)        bad++;
            if ((int)(int16_t)(uint16_t)(r->w[2] & 0xFFFFu) != dx)    bad++;
            if ((int)(int16_t)(uint16_t)(r->w[2] >> 16) != dy)        bad++;
            if ((int)(r->w[3] & 0xFFFFu) != w)                        bad++;
            if ((int)(r->w[3] >> 16) != h)                            bad++;
            if (r->w[5] != (ken ? key : 0u))                          bad++;
            if (r->w[4] != 0u || r->w[6] != 0u)                       bad++;
            if (r->w[7] != l.used - 1u)                               bad++;
        }
    }

    ck("T6 roundtrip fields", bad == 0u);
    ck("T6 roundtrip count", n > 50000u);
}

/* --- T7 构造失败 -------------------------------------------------------- */

static void t7_build_errors(void)
{
    gfx_surface_table ta, tb;
    gfx_surface       s;
    gfx_cmd           buf[4];
    gfx_list          l;
    gfx_rect          r;
    int               rc;

    gfx_surface_reset(&ta);
    gfx_surface_reset(&tb);
    s.base = 0x1000u; s.width = 8u; s.height = 8u;
    s.stride_log2 = 5u; s.space = (uint8_t)GFX_SPACE_DDR;
    s.format = (uint8_t)GFX_FMT_XRGB8888;
    ck("T7 set surface a", gfx_surface_set(&ta, 0, &s) == GFX_OK);

    /* 两张表互不干扰：登记 a 不会让 b 的同号槽位变得可用 */
    r.x0 = 0; r.y0 = 0; r.x1 = 3; r.y1 = 3;
    gfx_list_begin(&l, buf, 4u, &tb);
    rc = gfx_fill(&l, 0, r, 0x00112233u, (int)GFX_EQ_COPY, 0u);
    ck("T7 no cross-table state", rc == GFX_E_BAD_SURFACE);
    ck("T7 rejected counted", l.rejected == 1u && l.used == 0u);
    ck("T7 sticky first error", l.first_error == GFX_E_BAD_SURFACE);

    /* 描述符结构不合法时槽位保持原样 */
    s.stride_log2 = 1u;
    ck("T7 bad descriptor", gfx_surface_set(&ta, 1, &s) == GFX_BUILD_BAD_ARG);
    ck("T7 slot untouched", ta.s[1].ctrl == 0u);
    s.stride_log2 = 5u;

    /* 不能无损编码的参数不追加也不截断 */
    gfx_list_begin(&l, buf, 4u, &ta);
    ck("T7 coord out of range",
       gfx_blit(&l, 0, 0, 0, 0, 40000, 0, 4, 4, (int)GFX_EQ_COPY, 0u, 0, 0u)
       == GFX_BUILD_BAD_ARG);
    ck("T7 surface id out of range",
       gfx_blit(&l, 99, 0, 0, 0, 0, 0, 4, 4, (int)GFX_EQ_COPY, 0u, 0, 0u)
       == GFX_BUILD_BAD_ARG);
    ck("T7 nothing appended", l.used == 0u);

    /* 表满：不再追加，overflow 加一 */
    gfx_list_begin(&l, buf, 2u, &ta);
    r.x0 = 0; r.y0 = 0; r.x1 = 3; r.y1 = 3;
    ck("T7 fill 1", gfx_fill(&l, 0, r, 0u, (int)GFX_EQ_COPY, 0u) == GFX_OK);
    ck("T7 fill 2", gfx_fill(&l, 0, r, 0u, (int)GFX_EQ_COPY, 0u) == GFX_OK);
    ck("T7 full", gfx_fill(&l, 0, r, 0u, (int)GFX_EQ_COPY, 0u) == GFX_BUILD_FULL);
    ck("T7 overflow counted", l.overflow == 1u && l.used == 2u);

    /* END 之后不能再追加 */
    gfx_list_begin(&l, buf, 4u, &ta);
    ck("T7 fill before end", gfx_fill(&l, 0, r, 0u, (int)GFX_EQ_COPY, 0u) == GFX_OK);
    ck("T7 end", gfx_list_end(&l) == GFX_OK);
    ck("T7 closed", gfx_fill(&l, 0, r, 0u, (int)GFX_EQ_COPY, 0u) == GFX_BUILD_CLOSED);

    /* 被拒绝的调用不占索引：下一条成功记录的 w7 仍是它的表内索引 */
    gfx_list_begin(&l, buf, 4u, &ta);
    (void)gfx_blit(&l, 0, 0, 0, 0, 40000, 0, 4, 4, (int)GFX_EQ_COPY, 0u, 0, 0u);
    (void)gfx_fill(&l, 0, r, 0u, (int)GFX_EQ_COPY, 0u);
    ck("T7 tag is table index", l.used == 1u && buf[0].w[7] == 0u);

    /* decode 是调试出口，负坐标要带符号，缓冲区不够时按 snprintf 的语义截断 */
    {
        gfx_cmd  c;
        char     big[128], tiny[8];
        uint32_t n;

        memset(&c, 0, sizeof c);
        c.w[0] = GFX_OP_BLIT | (1u << 16);
        c.w[2] = (uint32_t)((int32_t)-5 & 0xFFFF);
        c.w[3] = 4u | (2u << 16);
        n = gfx_cmd_decode(&c, big, (uint32_t)sizeof big);
        ck("T7 decode has sign", n < sizeof big && strstr(big, "d=-5,0") != 0);
        ck("T7 decode truncates",
           gfx_cmd_decode(&c, tiny, (uint32_t)sizeof tiny) == n
           && tiny[sizeof tiny - 1u] == '\0');
    }
}

/* --- T8 单像素算术 ------------------------------------------------------ */

/* 在一张 1x1 的表面上跑一条命令，返回写回的那个字。
   两个实现各跑一次，顺带把「两者一致」也验了。 */
static uint32_t one_pixel(const gfx_cmd *c, uint32_t dst_seed, uint32_t src_seed,
                          uint32_t src_fmt, int use_ref)
{
    static uint32_t     dev[64];
    gfx_surface_table   t;
    gfx_mem             m;
    gfx_surface         s;
    exec_sw_state       es;
    gfx_rect            clip;
    uint32_t            out;

    memset(dev, 0, sizeof dev);
    m.dev_base = 0x100u;
    m.host     = (uint8_t *)(void *)dev;
    m.bytes    = (uint32_t)sizeof dev;

    gfx_surface_reset(&t);
    s.base = 0x100u; s.width = 1u; s.height = 1u; s.stride_log2 = 2u;
    s.space = (uint8_t)GFX_SPACE_DDR; s.format = (uint8_t)GFX_FMT_XRGB8888;
    (void)gfx_surface_set(&t, 0, &s);
    s.base = 0x140u; s.format = (uint8_t)src_fmt;
    (void)gfx_surface_set(&t, 1, &s);

    dev[0]  = dst_seed;                          /* 0x100 */
    dev[16] = src_seed;                          /* 0x140 */

    if (use_ref) {
        ref_exec_reset(&clip);
        (void)ref_exec_run(&clip, &t, &m, c, 1u, 1u, 0, 0);
    } else {
        exec_sw_reset(&es);
        (void)exec_sw_run(&es, &t, &m, c, 1u, 1u, 0, 0);
    }
    out = dev[0];
    return out;
}

static void px_case(const char *tag, const gfx_cmd *c, uint32_t dst, uint32_t src,
                    uint32_t fmt, uint32_t want)
{
    uint32_t a = one_pixel(c, dst, src, fmt, 0);
    uint32_t b = one_pixel(c, dst, src, fmt, 1);

    ck_u32(tag, want, a);
    if (a != b) {
        ck("  sw vs ref", 0);
    }
}

static void mk_fill(gfx_cmd *c, uint32_t eq, uint32_t w4, uint32_t cc)
{
    memset(c, 0, sizeof *c);
    c->w[0] = GFX_OP_FILL | (eq << 8) | (cc << 24);
    c->w[3] = 1u | (1u << 16);
    c->w[4] = w4;
}

static void mk_blit(gfx_cmd *c, uint32_t eq)
{
    memset(c, 0, sizeof *c);
    c->w[0] = GFX_OP_BLIT | (eq << 8) | (1u << 16);
    c->w[3] = 1u | (1u << 16);
}

static void t8_pixel_math(void)
{
    gfx_cmd c;

    /* spec 第 01 章 3.4 节的最小反例：c = 1、src = 1、dst = 128。
       精确值 32513/255 约 127.502，V 顺序得 128（U 顺序会得 127）。 */
    mk_fill(&c, GFX_EQ_CONST, 0x00010101u, 1u);
    px_case("T8 const c1 s1 d128", &c, 0x00808080u, 0u, GFX_FMT_XRGB8888,
            0x00808080u);

    /* COPY：输出就是源的 RGB */
    mk_fill(&c, GFX_EQ_COPY, 0x00112233u, 0u);
    px_case("T8 copy", &c, 0x00445566u, 0u, GFX_FMT_XRGB8888, 0x00112233u);

    /* COPY 一个带 alpha 的源，高字节必须被清掉 */
    mk_blit(&c, GFX_EQ_COPY);
    px_case("T8 copy argb high byte", &c, 0x00445566u, 0xFF112233u,
            GFX_FMT_ARGB8888_PM, 0x00112233u);

    /* ADD 饱和 */
    mk_fill(&c, GFX_EQ_ADD, 0x00C0C0C0u, 0u);
    px_case("T8 add saturate", &c, 0x00808080u, 0u, GFX_FMT_XRGB8888, 0x00FFFFFFu);

    /* MUL：128 x 128 / 255 = 64 */
    mk_fill(&c, GFX_EQ_MUL, 0x00808080u, 0u);
    px_case("T8 mul half", &c, 0x00808080u, 0u, GFX_FMT_XRGB8888, 0x00404040u);

    /* 预乘 ALPHA：a = 128、src_pm = (64,32,16)、dst = (0,64,128) */
    mk_fill(&c, GFX_EQ_ALPHA, 0x80402010u, 0u);
    px_case("T8 alpha premult", &c, 0x00004080u, 0u, GFX_FMT_XRGB8888,
            0x00404050u);

    /* a = 0 照常写回，值与原来相同 */
    mk_fill(&c, GFX_EQ_ALPHA, 0x00000000u, 0u);
    px_case("T8 alpha a0", &c, 0x00123456u, 0u, GFX_FMT_XRGB8888, 0x00123456u);

    /* 非预乘 ALPHA：a = 128、src = (255,0,0)、dst = (0,255,0)
       R: 255*128 + 0*127 = 32640 -> 128；G: 0*128 + 255*127 = 32385 -> 127 */
    mk_blit(&c, GFX_EQ_ALPHA_STRAIGHT);
    px_case("T8 alpha straight", &c, 0x0000FF00u, 0x80FF0000u,
            GFX_FMT_ARGB8888_ST, 0x00807F00u);

    /* XRGB 源在 alpha 方程里的有效 alpha 是 255 */
    mk_blit(&c, GFX_EQ_ALPHA);
    px_case("T8 xrgb alpha is 255", &c, 0x00FFFFFFu, 0x00102030u,
            GFX_FMT_XRGB8888, 0x00102030u);
}

/* --- T9 裁剪 ------------------------------------------------------------ */

static void t9_clip(void)
{
    static uint32_t   dev[64 * 64];
    gfx_surface_table t;
    gfx_surface       s;
    gfx_mem           m;
    gfx_cmd           c;
    exec_sw_state     es;
    exec_sw_stats     st;

    memset(dev, 0, sizeof dev);
    m.dev_base = 0u;
    m.host     = (uint8_t *)(void *)dev;
    m.bytes    = (uint32_t)sizeof dev;

    gfx_surface_reset(&t);
    s.base = 0u; s.width = 64u; s.height = 64u; s.stride_log2 = 8u;
    s.space = (uint8_t)GFX_SPACE_DDR; s.format = (uint8_t)GFX_FMT_XRGB8888;
    (void)gfx_surface_set(&t, 0, &s);

    /* 左上角出屏：dst = (-10,-10)、32x32，与表面和游戏区求交后只剩 22x22 */
    memset(&c, 0, sizeof c);
    c.w[0] = GFX_OP_FILL;
    c.w[2] = (uint32_t)((int32_t)-10 & 0xFFFF) | ((uint32_t)((int32_t)-10 & 0xFFFF) << 16);
    c.w[3] = 32u | (32u << 16);
    c.w[4] = 0x00112233u;

    exec_sw_stats_clear(&st);
    exec_sw_reset(&es);
    (void)exec_sw_run(&es, &t, &m, &c, 1u, 1u, &st, 0);
    ck("T9 negative origin clipped", st.pixel_written == 22u * 22u);

    /* 右下出表面：dst = (50,50)、32x32，与 64x64 的表面求交后剩 14x14 */
    c.w[2] = 50u | (50u << 16);
    exec_sw_stats_clear(&st);
    exec_sw_reset(&es);
    (void)exec_sw_run(&es, &t, &m, &c, 1u, 1u, &st, 0);
    ck("T9 surface bound clipped", st.pixel_written == 14u * 14u);

    /* 宽为 0：整条命令跳过，一个像素也不画，但记录仍算执行完 */
    c.w[3] = 0u | (32u << 16);
    exec_sw_stats_clear(&st);
    exec_sw_reset(&es);
    (void)exec_sw_run(&es, &t, &m, &c, 1u, 1u, &st, 0);
    ck("T9 zero width no pixels", st.pixel_total == 0u && st.cmd == 1u);
}

/* --- T10/T11 向量 ------------------------------------------------------- */

static uint32_t run_builtin(int use_ref)
{
    uint32_t i, n = vec_builtin_count(), bad = 0u;

    for (i = 0u; i < n; i++) {
        vec_case v;
        char     why[160];

        if (!vec_builtin_get(i, &v))
            continue;
        if (!vec_run_one(&v, use_ref, why, (uint32_t)sizeof why)) {
            char     line[256];
            core_out o;

            core_out_init(&o, line, (uint32_t)sizeof line);
            core_out_str(&o, "  ");
            core_out_str(&o, v.name);
            core_out_str(&o, " FAIL   ");
            core_out_str(&o, why);
            core_out_str(&o, "\n");
            say(line);
            bad++;
        }
    }
    return bad;
}

static void t10_builtin(void)
{
    char     line[96];
    core_out o;
    uint32_t a = run_builtin(0);
    uint32_t b = run_builtin(1);

    core_out_init(&o, line, (uint32_t)sizeof line);
    core_out_str(&o, "T10 builtin vectors n=");
    core_out_u32(&o, vec_builtin_count());
    say(line);
    ck("", a == 0u && b == 0u);
}

static void t11_replay(const char *path)
{
    if (path == 0) {
        say("T11 emitted vectors skipped (no --vec)\n");
        return;
    }
    {
        uint32_t na = 0u, nb = 0u;
        int      a = vec_replay_file(path, 0, &na);
        int      b = vec_replay_file(path, 1, &nb);
        char     line[96];
        core_out o;

        core_out_init(&o, line, (uint32_t)sizeof line);
        core_out_str(&o, "T11 emitted vectors n=");
        core_out_u32(&o, na);
        say(line);
        ck("", a == 0 && b == 0 && na == nb && na > 0u);
    }
}

/* --- T12 整帧用例 ------------------------------------------------------- */

/* 整帧 CRC 的口径：逐行取前 width*4 字节，共 height 行，跳过行跨的间隙。
   与 ref_crc32_strided() 同口径，也是 E7 与引擎仿真要用的那一个。 */
static uint32_t frame_crc(const uint8_t *host, uint32_t base)
{
    crc32_ctx ctx;
    uint32_t  y;

    crc32_begin(&ctx);
    for (y = 0u; y < (uint32_t)GFX_SCREEN_H; y++)
        crc32_update(&ctx, host + base + (y << GFX_CASE_FB_STRIDE_LOG2),
                     (uint32_t)GFX_SCREEN_W * 4u);
    return crc32_end(&ctx);
}

static uint32_t run_case(uint8_t *dev, uint32_t idx, int use_ref)
{
    gfx_surface_table t;
    gfx_mem           m;
    gfx_cmd           cmds[2u * GFX_CASE_MAX_CMD];
    exec_sw_state     es;
    gfx_rect          clip;
    exec_sw_stats     st;
    uint32_t          n1, n2 = 0u;

    memset(dev, 0, CASE_DEV_BYTES);
    m.dev_base = 0u;
    m.host     = dev;
    m.bytes    = CASE_DEV_BYTES;

    n1 = gfx_case_build(idx, &t, &m, CASE_FB_BASE, CASE_TEX_BASE, cmds, &n2);

    exec_sw_stats_clear(&st);
    if (use_ref) {
        ref_exec_reset(&clip);
        (void)ref_exec_run(&clip, &t, &m, cmds, n1, n1, &st, 0);
        if (n2 > 0u)
            (void)ref_exec_run(&clip, &t, &m, cmds + n1, n2, n2, &st, 0);
    } else {
        exec_sw_reset(&es);
        (void)exec_sw_run(&es, &t, &m, cmds, n1, n1, &st, 0);
        if (n2 > 0u)
            (void)exec_sw_run(&es, &t, &m, cmds + n1, n2, n2, &st, 0);
    }

    if (st.pixel_total != st.pixel_written + st.key_skip + st.src_oob)
        g_fail++;

    return frame_crc(dev, CASE_FB_BASE);
}

/* golden.txt 一行一个用例：名字 + 八位十六进制 CRC。 */
static int read_golden(const char *path, uint32_t *out)
{
    FILE    *f = fopen(path, "r");
    char     name[64];
    uint32_t crc;
    uint32_t n = 0u;

    if (f == 0)
        return 0;
    while (n < GFX_CASE_N && fscanf(f, "%63s %x", name, &crc) == 2) {
        out[n] = crc;
        n++;
    }
    fclose(f);
    return (n == GFX_CASE_N);
}

static void write_golden(const char *path, const uint32_t *crc)
{
    FILE    *f = fopen(path, "w");
    uint32_t i;

    if (f == 0) {
        say("T12 cannot write golden FAIL\n");
        g_fail++;
        return;
    }
    for (i = 0u; i < GFX_CASE_N; i++)
        fprintf(f, "%-24s %08x\n", gfx_case_name(i), crc[i]);
    fclose(f);
}

static void t12_frames(const char *golden, int gen)
{
    uint8_t *dev = (uint8_t *)malloc(CASE_DEV_BYTES);
    uint32_t got[GFX_CASE_N], want[GFX_CASE_N];
    uint32_t i;
    int      have;

    if (dev == 0) {
        say("T12 out of memory FAIL\n");
        g_fail++;
        return;
    }

    for (i = 0u; i < GFX_CASE_N; i++) {
        got[i] = run_case(dev, i, 0);
#if !GFX_INJECTED
        {
            uint32_t r = run_case(dev, i, 1);

            if (r != got[i]) {
                char     line[96];
                core_out o;

                core_out_init(&o, line, (uint32_t)sizeof line);
                core_out_str(&o, "  ");
                core_out_str(&o, gfx_case_name(i));
                core_out_str(&o, " sw=");
                core_out_hex32(&o, got[i]);
                core_out_str(&o, " ref=");
                core_out_hex32(&o, r);
                core_out_str(&o, " FAIL\n");
                say(line);
                g_fail++;
            }
        }
#endif
    }
    free(dev);

    if (gen) {
        write_golden(golden, got);
        say("T12 golden written\n");
        return;
    }

    have = read_golden(golden, want);
    if (!have) {
        say("T12 golden missing FAIL\n");
        g_fail++;
        return;
    }

    for (i = 0u; i < GFX_CASE_N; i++) {
        char     line[128];
        core_out o;

        core_out_init(&o, line, (uint32_t)sizeof line);
        core_out_str(&o, "T12 ");
        core_out_str(&o, gfx_case_name(i));
        core_out_str(&o, " crc ");
        core_out_hex32(&o, got[i]);
#if GFX_INJECTED
        core_out_str(&o, (got[i] == want[i]) ? " same\n" : " diff\n");
#else
        core_out_str(&o, (got[i] == want[i]) ? " ok\n" : " FAIL\n");
        if (got[i] != want[i])
            g_fail++;
#endif
        say(line);
    }
}

uint32_t test_gfx_run(const char *vecpath, const char *golden, int gen)
{
    g_fail = 0u;

    say("libTest gfx: list/exec_sw/ref_exec\n");
    _Static_assert(sizeof(gfx_cmd) == 32, "命令记录必须是 32 字节");
    _Static_assert(sizeof(gfx_surf_raw) == 12, "表面表每项必须是 12 字节");

    t6_roundtrip();
    t7_build_errors();
    t8_pixel_math();
    t9_clip();
    t10_builtin();
    t11_replay(vecpath);
    t12_frames(golden, gen);

    return g_fail;
}
