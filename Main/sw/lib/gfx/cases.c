/******************************************************************************
* @file cases.c: 十一个整帧用例
*
* @brief  软件执行器、参考实现、板上测量与引擎仿真共用的同一批输入。
*         每个用例负责三件事：登记表面、把两张表面的初始内容写进设备内存、
*         构造命令表。跑完之后对整帧算 CRC32，期望值在 sw/lib/test/golden.txt。
*
*         用例共十一个，在这里定死，各处不再自行解释。
*
*         帧缓冲取 640x480、stride_log2 = 12，与板上一致；纹理 64x64、
*         stride_log2 = 8，格式随用例变。
******************************************************************************/

#include "gfx/gfx.h"

#define FB_W  ((uint32_t)GFX_SCREEN_W)
#define FB_H  ((uint32_t)GFX_SCREEN_H)
#define TEX_W GFX_CASE_TEX_W
#define TEX_H GFX_CASE_TEX_H

/* 色键用例里被当成透明的那个字。取一个不会与渐变撞上的值。 */
#define CASE_KEY 0x00123456u

static const char *g_name[GFX_CASE_N] = {
    "fill-screen",
    "blit-copy",
    "blit-key",
    "blit-alpha-pm",
    "blit-alpha-straight",
    "blit-add",
    "fill-const",
    "blit-mul",
    "blit-clip-partial",
    "set-clip-state",
    "copy-argb-high-byte"
};

const char *gfx_case_name(uint32_t idx)
{
    return (idx < GFX_CASE_N) ? g_name[idx] : 0;
}

static void poke(const gfx_mem *m, uint32_t addr, uint32_t v)
{
    uint32_t off = addr - m->dev_base;

    if (addr < m->dev_base || off + 4u > m->bytes || (off & 3u) != 0u)
        return;
    *(uint32_t *)(void *)(m->host + off) = v;
}

/* 帧缓冲的初始内容：一张与坐标有关、每个像素都不同的图案。
   用渐变而不是纯色，是为了让「该写没写」和「不该写却写了」都能被 CRC 抓到。 */
static void seed_fb(const gfx_mem *m, const gfx_surf_raw *s)
{
    uint32_t x, y;

    for (y = 0u; y < FB_H; y++) {
        for (x = 0u; x < FB_W; x++) {
            uint32_t v = (((x + y) & 0xFFu) << 16)
                       | ((( x * 3u) & 0xFFu) <<  8)
                       |  (( y * 5u) & 0xFFu);
            poke(m, gfx_addr_of(s, (int32_t)x, (int32_t)y), v);
        }
    }
}

/* 纹理的初始内容，按格式生成：
     XRGB      高字节恒 0
     PREMULT   每个颜色通道不大于 alpha（素材契约，规范条款 §2）
     STRAIGHT  alpha 与颜色互不约束
   with_key 为真时，让一部分像素恰好等于色键字。 */
static void seed_tex(const gfx_mem *m, const gfx_surf_raw *s,
                     uint32_t fmt, int with_key)
{
    uint32_t x, y;

    for (y = 0u; y < TEX_H; y++) {
        for (x = 0u; x < TEX_W; x++) {
            uint32_t a  = (x * 4u) & 0xFFu;
            uint32_t rr = (x * 7u + y * 3u) & 0xFFu;
            uint32_t gg = (y * 5u + 17u) & 0xFFu;
            uint32_t bb = (x + y * 2u) & 0xFFu;
            uint32_t v;

            if (fmt == GFX_FMT_ARGB8888_PM) {
                /* 预乘之后各通道不超过 alpha */
                v = (a << 24) | (((a * rr) / 255u) << 16)
                  | (((a * gg) / 255u) << 8) | ((a * bb) / 255u);
            } else if (fmt == GFX_FMT_ARGB8888_ST) {
                v = (a << 24) | (rr << 16) | (gg << 8) | bb;
            } else {
                v = (rr << 16) | (gg << 8) | bb;
            }

            /* 棋盘格的一半换成色键字，让命中与不命中在同一个用例里各占一半 */
            if (with_key && (((x >> 2) + (y >> 2)) & 1u) != 0u)
                v = CASE_KEY;

            poke(m, gfx_addr_of(s, (int32_t)x, (int32_t)y), v);
        }
    }
}

static void set_fb(gfx_surface_table *t, uint32_t slot, uint32_t base)
{
    gfx_surface s;

    s.base        = base;
    s.width       = (uint16_t)FB_W;
    s.height      = (uint16_t)FB_H;
    s.stride_log2 = (uint8_t)GFX_CASE_FB_STRIDE_LOG2;
    s.space       = (uint8_t)GFX_SPACE_DDR;
    s.format      = (uint8_t)GFX_FMT_XRGB8888;
    (void)gfx_surface_set(t, (int)slot, &s);
}

static void set_tex(gfx_surface_table *t, uint32_t slot, uint32_t base, uint32_t fmt)
{
    gfx_surface s;

    s.base        = base;
    s.width       = (uint16_t)TEX_W;
    s.height      = (uint16_t)TEX_H;
    s.stride_log2 = (uint8_t)GFX_CASE_TEX_STRIDE_LOG2;
    s.space       = (uint8_t)GFX_SPACE_DDR;
    s.format      = (uint8_t)fmt;
    (void)gfx_surface_set(t, (int)slot, &s);
}

static gfx_rect rect_of(int32_t x, int32_t y, int32_t w, int32_t h)
{
    gfx_rect r;

    r.x0 = x;
    r.y0 = y;
    r.x1 = x + w - 1;
    r.y1 = y + h - 1;
    return r;
}

/* 一次 BLIT 覆盖整屏需要把 64x64 的纹理铺开，这里只铺一块并靠坐标错开，
   目的不是铺满，而是让每个方程都作用在「有渐变的背景」上。 */
static uint32_t build_one(uint32_t idx, gfx_surface_table *t,
                          gfx_cmd *cmds, uint32_t cap)
{
    gfx_list l;

    gfx_list_begin(&l, cmds, cap, t);

    switch (idx) {
    case 0:                                     /* fill-screen */
        (void)gfx_fill(&l, 0, rect_of(0, 0, (int32_t)FB_W, (int32_t)FB_H),
                       0x00204060u, (int)GFX_EQ_COPY, 0u);
        break;
    case 1:                                     /* blit-copy */
        (void)gfx_blit(&l, 1, 0, 0, 0, 100, 100, (int)TEX_W, (int)TEX_H,
                       (int)GFX_EQ_COPY, 0u, 0, 0u);
        break;
    case 2:                                     /* blit-key */
        (void)gfx_blit(&l, 1, 0, 0, 0, 40, 40, (int)TEX_W, (int)TEX_H,
                       (int)GFX_EQ_COPY, 0u, 1, CASE_KEY);
        break;
    case 3:                                     /* blit-alpha-pm */
        (void)gfx_blit(&l, 1, 0, 0, 0, 200, 150, (int)TEX_W, (int)TEX_H,
                       (int)GFX_EQ_ALPHA, 0u, 0, 0u);
        break;
    case 4:                                     /* blit-alpha-straight */
        (void)gfx_blit(&l, 1, 0, 0, 0, 200, 150, (int)TEX_W, (int)TEX_H,
                       (int)GFX_EQ_ALPHA_STRAIGHT, 0u, 0, 0u);
        break;
    case 5:                                     /* blit-add，先垫一块亮底让它饱和 */
        (void)gfx_fill(&l, 0, rect_of(300, 200, 80, 80), 0x00C0C0C0u,
                       (int)GFX_EQ_COPY, 0u);
        (void)gfx_blit(&l, 1, 0, 0, 0, 300, 200, (int)TEX_W, (int)TEX_H,
                       (int)GFX_EQ_ADD, 0u, 0, 0u);
        break;
    case 6:                                     /* fill-const，c = 1 是舍入行为的最小反例 */
        (void)gfx_fill(&l, 0, rect_of(0, 0, (int32_t)FB_W, (int32_t)FB_H),
                       0x00010101u, (int)GFX_EQ_CONST, 1u);
        break;
    case 7:                                     /* blit-mul */
        (void)gfx_blit(&l, 1, 0, 0, 0, 500, 380, (int)TEX_W, (int)TEX_H,
                       (int)GFX_EQ_MUL, 0u, 0, 0u);
        break;
    case 8:                                     /* blit-clip-partial */
        /* 负坐标、右下出屏，另加一条源矩形比纹理大的（源越界逐像素跳过） */
        (void)gfx_blit(&l, 1, 0, 0, 0, -20, -20, (int)TEX_W, (int)TEX_H,
                       (int)GFX_EQ_COPY, 0u, 0, 0u);
        (void)gfx_blit(&l, 1, 0, 0, 0, (int32_t)FB_W - 20, (int32_t)FB_H - 20,
                       (int)TEX_W, (int)TEX_H, (int)GFX_EQ_COPY, 0u, 0, 0u);
        (void)gfx_blit(&l, 1, -10, -10, 0, 300, 100, 100, 100,
                       (int)GFX_EQ_COPY, 0u, 0, 0u);
        break;
    case 9:                                     /* set-clip-state 的第一张表 */
        (void)gfx_set_clip(&l, rect_of(100, 100, 200, 150));
        (void)gfx_fill(&l, 0, rect_of(0, 0, (int32_t)FB_W, (int32_t)FB_H),
                       0x00FF8000u, (int)GFX_EQ_COPY, 0u);
        break;
    default:                                    /* copy-argb-high-byte */
        /* 源是 PREMULT（高字节非 0），COPY 之后目的的 [31:24] 必须是 0 */
        (void)gfx_blit(&l, 1, 0, 0, 0, 10, 10, (int)TEX_W, (int)TEX_H,
                       (int)GFX_EQ_COPY, 0u, 0, 0u);
        break;
    }

    (void)gfx_list_end(&l);
    return l.used;
}

/* set-clip-state 的第二张表：继承第一张表留下的框，
   再验空矩形全裁掉、以及后发的大框可以扩大先前的小框。 */
static uint32_t build_second(gfx_surface_table *t, gfx_cmd *cmds, uint32_t cap)
{
    gfx_list l;
    gfx_rect empty;

    gfx_list_begin(&l, cmds, cap, t);

    /* 继承而来的 100,100 ~ 299,249 仍然生效 */
    (void)gfx_fill(&l, 0, rect_of(0, 0, (int32_t)FB_W, (int32_t)FB_H),
                   0x000080FFu, (int)GFX_EQ_COPY, 0u);

    /* 空矩形：之后的命令全部跳过 */
    empty.x0 = 0; empty.y0 = 0; empty.x1 = -1; empty.y1 = -1;
    (void)gfx_set_clip(&l, empty);
    (void)gfx_fill(&l, 0, rect_of(0, 0, (int32_t)FB_W, (int32_t)FB_H),
                   0x00FF00FFu, (int)GFX_EQ_COPY, 0u);

    /* 新框替换旧框，可以比上一个框大 */
    (void)gfx_set_clip(&l, rect_of(0, 0, (int32_t)FB_W, (int32_t)FB_H));
    (void)gfx_fill(&l, 0, rect_of(400, 300, 60, 40), 0x0000FF00u,
                   (int)GFX_EQ_COPY, 0u);

    (void)gfx_list_end(&l);
    return l.used;
}

uint32_t gfx_case_build(uint32_t idx, gfx_surface_table *t, const gfx_mem *m,
                        uint32_t fb, uint32_t tex,
                        gfx_cmd *cmds, uint32_t *count2)
{
    uint32_t fmt;
    uint32_t n1;

    if (count2 != 0)
        *count2 = 0u;
    if (idx >= GFX_CASE_N || t == 0 || m == 0 || cmds == 0)
        return 0u;

    /* 纹理格式随用例：ALPHA 要预乘源，ALPHA_STRAIGHT 要直通源，
       通道范围那一条要一个高字节非零的源。 */
    if (idx == 3u || idx == 10u)
        fmt = GFX_FMT_ARGB8888_PM;
    else if (idx == 4u)
        fmt = GFX_FMT_ARGB8888_ST;
    else
        fmt = GFX_FMT_XRGB8888;

    gfx_surface_reset(t);
    set_fb(t, 0u, fb);
    set_tex(t, 1u, tex, fmt);

    seed_fb(m, &t->s[0]);
    seed_tex(m, &t->s[1], fmt, (idx == 2u) ? 1 : 0);

    n1 = build_one(idx, t, cmds, GFX_CASE_MAX_CMD);
    if (idx == 9u && count2 != 0)
        *count2 = build_second(t, cmds + n1, GFX_CASE_MAX_CMD);
    return n1;
}
