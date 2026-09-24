/******************************************************************************
* @file sw6_scene_a.c: 场景 A —— 大量方块随机移动
*
* @brief  实现见 sw6_scene_a.h。全部是整数运算，没有浮点、没有 malloc。
******************************************************************************/

#include "board.h"

#include "core/xorshift32.h"

#include "sw6_scene_a.h"

/* --- 统计 ---------------------------------------------------------------- */

void sw6_build_stat_clear(sw6_build_stat *fs)
{
    uint32_t i;

    for (i = 0u; i < 4u; i++)
        fs->eq[i] = 0u;
    fs->off  = 0u;
    fs->bad  = 0u;
    fs->cmds = 0u;
}

/* --- 初始化 -------------------------------------------------------------- */

/* 一个方块在 Q12.4 下的往返范围：由像素尺寸换算。 */
static int32_t q16_range(uint32_t span_px)
{
    return (int32_t)(span_px * 16u);
}

/* 四个固定出屏样例。像素值都是常量，见 spec 2.1 与 R34：
   #0 跨上边（y=-4 固定，x 在 [0, 384] 往返，正好避开 HUD 的 x>=400）；
   #1 跨下边（y=468 固定，底边 483 > 479）；
   #2 跨左边（x=-4 固定）；
   #3 跨右边（x=628 固定，y 在 [160, 464] 往返，避开 HUD 的 y<160）。 */
static void init_samples(sw6_block *b)
{
    static const struct {
        int32_t px, py, vx_px, vy_px;
        int32_t xmin_px, xmax_px, ymin_px, ymax_px;
    } s[SW6_SAMPLE_N] = {
        {  40,  -4, 1, 0,   0, 384,  -4,  -4 },
        { 100, 468, -1, 0,  0, 624, 468, 468 },
        {  -4,  40, 0, 1,  -4,  -4,   0, 464 },
        { 628, 200, 0, -1, 628, 628, 160, 464 },
    };
    uint32_t i;

    for (i = 0u; i < SW6_SAMPLE_N; i++) {
        b[i].x    = (int32_t)(s[i].px * 16);
        b[i].y    = (int32_t)(s[i].py * 16);
        b[i].vx   = (int32_t)(s[i].vx_px * 16);
        b[i].vy   = (int32_t)(s[i].vy_px * 16);
        b[i].xmin = (int32_t)(s[i].xmin_px * 16);
        b[i].xmax = (int32_t)(s[i].xmax_px * 16);
        b[i].ymin = (int32_t)(s[i].ymin_px * 16);
        b[i].ymax = (int32_t)(s[i].ymax_px * 16);
        b[i].w    = 16u;
        b[i].h    = 16u;
        /* 出屏样例也走三种方程，出屏的裁剪在各方程下都被走到 */
        b[i].mode  = (uint8_t)(SW6_DRAW_FILL_COPY + (i % 3u));
        b[i].cell  = 0u;
        b[i].color = (i % 3u == 1u) ? 0x80404040u : 0x00C03060u;
    }
}

/* 按 0..9 分档挑绘制方式，保证 N=100 时六种都出现（spec 2.1 要求三种方程都在）。 */
static uint8_t pick_mode(uint32_t r)
{
    switch (r % 10u) {
    case 0u:
    case 1u:
    case 2u:
    case 8u:
    case 9u:  return SW6_DRAW_FILL_COPY;
    case 3u:  return SW6_DRAW_FILL_ALPHA;
    case 4u:  return SW6_DRAW_FILL_ADD;
    case 5u:  return SW6_DRAW_BLIT_ALPHA;
    case 6u:  return SW6_DRAW_BLIT_ADD;
    default:  return SW6_DRAW_BLIT_COPY;
    }
}

void sw6_a_begin(sw6_scene_a *a, uint32_t seed, uint32_t n0)
{
    xs32     g;
    uint32_t i;

    init_samples(a->blk);
    xs32_seed(&g, seed);

    for (i = SW6_SAMPLE_N; i < SW6_POOL_MAX; i++) {
        sw6_block *b  = &a->blk[i];
        uint32_t   r[10];
        uint32_t   k;

        for (k = 0u; k < 10u; k++)
            r[k] = xs32_next(&g);

        b->mode = pick_mode(r[0]);
        if (b->mode == SW6_DRAW_BLIT_ALPHA || b->mode == SW6_DRAW_BLIT_ADD) {
            b->w = 16u;                 /* BLIT 不缩放：尺寸取图集格子 */
            b->h = 16u;
            b->cell = (uint8_t)(r[5] % 48u);
        } else if (b->mode == SW6_DRAW_BLIT_COPY) {
            b->w = 32u;                 /* 棋盘子矩形 32×32 */
            b->h = 32u;
            b->cell = (uint8_t)(r[5] % 3u);
        } else {
            b->w = (uint8_t)(8u + r[1] % 25u);
            b->h = (uint8_t)(8u + r[2] % 25u);
            b->cell = 0u;
        }

        b->xmin = 0;
        b->xmax = q16_range(FB_WIDTH - (uint32_t)b->w);
        b->ymin = 0;
        b->ymax = q16_range(FB_HEIGHT - (uint32_t)b->h);
        b->x    = (int32_t)((r[3] % (FB_WIDTH - (uint32_t)b->w + 1u)) * 16u);
        b->y    = (int32_t)((r[4] % (FB_HEIGHT - (uint32_t)b->h + 1u)) * 16u);

        b->vx = (int32_t)(1u + r[6] % 3u) * 16;
        if ((r[6] & 0x10000u) != 0u)
            b->vx = -b->vx;
        b->vy = (int32_t)(1u + r[7] % 3u) * 16;
        if ((r[7] & 0x10000u) != 0u)
            b->vy = -b->vy;

        if (b->mode == SW6_DRAW_FILL_ALPHA) {
            /* 预乘：先定 A，再让 R／G／B 逐通道不大于 A（spec 2.1） */
            uint32_t alpha = 0x40u + r[5] % 0xC0u;
            uint32_t red   = r[8] % (alpha + 1u);
            uint32_t green = r[9] % (alpha + 1u);
            uint32_t blue  = (r[8] >> 8) % (alpha + 1u);

            b->color = (alpha << 24) | (red << 16) | (green << 8) | blue;
        } else if (b->mode == SW6_DRAW_FILL_ADD) {
            b->color = (r[5] & 0x00FFFFFFu) | 0x00202020u;   /* 不会是全黑 */
        } else {
            b->color = (r[5] & 0x00FFFFFFu) | 0x00101010u;
        }
    }

    if (n0 < SW6_N_MIN) n0 = SW6_N_MIN;
    if (n0 > SW6_N_MAX) n0 = SW6_N_MAX;
    a->n = n0;
}

/* --- 每帧 ---------------------------------------------------------------- */

/* 一步：位置加位移，出界就按闭区间反弹（镜像），位移取反。 */
static void step(int32_t *p, int32_t *v, int32_t lo, int32_t hi)
{
    int32_t q = *p + *v;

    if (q < lo) {
        q = lo + (lo - q);
        *v = -*v;
    } else if (q > hi) {
        q = hi - (q - hi);
        *v = -*v;
    }
    *p = q;
}

void sw6_a_update(sw6_scene_a *a)
{
    uint32_t i;

    for (i = 0u; i < a->n; i++) {
        sw6_block *b = &a->blk[i];

        step(&b->x, &b->vx, b->xmin, b->xmax);
        step(&b->y, &b->vy, b->ymin, b->ymax);
    }
}

void sw6_a_adjust(sw6_scene_a *a, int up, int down)
{
    if (up) {
        a->n += SW6_N_STEP;
        if (a->n > SW6_N_MAX)
            a->n = SW6_N_MAX;
    }
    if (down) {
        if (a->n <= SW6_N_MIN + SW6_N_STEP)
            a->n = SW6_N_MIN;
        else
            a->n -= SW6_N_STEP;
    }
}

void sw6_a_build(const sw6_scene_a *a, gfx_list *l, int dst, sw6_build_stat *fs)
{
    uint32_t i;

    for (i = 0u; i < a->n; i++) {
        const sw6_block *b  = &a->blk[i];
        const int32_t    x0 = b->x >> 4;    /* 算术右移：负数向下取整 */
        const int32_t    y0 = b->y >> 4;
        const int32_t    x1 = x0 + (int32_t)b->w - 1;
        const int32_t    y1 = y0 + (int32_t)b->h - 1;
        const int        partial = (x0 < 0) || (y0 < 0)
                                || (x1 > (int32_t)FB_WIDTH - 1)
                                || (y1 > (int32_t)FB_HEIGHT - 1);
        int              rc;

        fs->cmds++;

        if (b->mode == SW6_DRAW_FILL_COPY || b->mode == SW6_DRAW_FILL_ALPHA
            || b->mode == SW6_DRAW_FILL_ADD) {
            gfx_rect r;

            r.x0 = x0; r.y0 = y0; r.x1 = x1; r.y1 = y1;
            rc = gfx_fill(l, dst, r, b->color,
                          (b->mode == SW6_DRAW_FILL_COPY)  ? GFX_EQ_COPY  :
                          (b->mode == SW6_DRAW_FILL_ALPHA) ? GFX_EQ_ALPHA : GFX_EQ_ADD,
                          0u);
        } else {
            int src, eq, sw, sx, sy;

            if (b->mode == SW6_DRAW_BLIT_COPY) {
                src = SW6_SLOT_CHECKER;
                eq  = GFX_EQ_COPY;
                sw  = 32;
                sx  = (int)(b->cell % 3u) * sw;
                sy  = (int)(b->cell / 3u) * sw;
            } else {
                src = SW6_SLOT_ATLAS;
                eq  = (b->mode == SW6_DRAW_BLIT_ALPHA) ? GFX_EQ_ALPHA : GFX_EQ_ADD;
                sw  = 16;
                sx  = (int)(b->cell % 8u) * sw;
                sy  = (int)(b->cell / 8u) * sw;
            }
            rc = gfx_blit(l, src, sx, sy, dst, (int)x0, (int)y0, sw, sw,
                          eq, 0u, 0, 0u);
        }

        if (rc != 0) {
            fs->bad++;
            continue;
        }
        switch (b->mode) {
        case SW6_DRAW_FILL_ALPHA:
        case SW6_DRAW_BLIT_ALPHA: fs->eq[1]++; break;
        case SW6_DRAW_FILL_ADD:
        case SW6_DRAW_BLIT_ADD:   fs->eq[2]++; break;
        default:                  fs->eq[0]++; break;
        }
        if (partial)
            fs->off++;
    }
}
