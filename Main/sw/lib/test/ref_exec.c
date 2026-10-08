/******************************************************************************
* @file ref_exec.c: 软件执行器的独立参考实现
*
* @brief  照 spec 第 01 章 3.6 节的执行顺序与第 02 章规范条款 §5 的十步逐条抄，
*         每一步旁边写出处。不追求速度：每个像素都重新算一遍地址，
*         方程在像素循环里现选，除以 255 用整数除法而不是移位近似式。
*
*         与产品实现刻意不共用任何判定代码，见 ref_exec.h 的说明。
******************************************************************************/

#include "ref_exec.h"

/* half-up 的精确商：round(x / 255) == (x + 127) / 255。
   产品实现走的是 spec 定稿的移位式 (t + (t>>8)) >> 8（t = x + 128），
   两条路在 x = 0..130050 上必须给出同样的值 —— 这正是本文件存在的意义。 */
static uint32_t ref_div255(uint32_t x)
{
    uint32_t q = (x + 127u) / 255u;

    return (q > 255u) ? 255u : q;
}

/* 自己解位域，不用 gfx_abi.h 的取字段函数。 */
static uint32_t f_op(const gfx_cmd *c)   { return  c->w[0]        & 0xFFu; }
static uint32_t f_eq(const gfx_cmd *c)   { return (c->w[0] >>  8) & 0x0Fu; }
static uint32_t f_key(const gfx_cmd *c)  { return (c->w[0] >> 12) & 0x01u; }
static uint32_t f_tint(const gfx_cmd *c) { return (c->w[0] >> 13) & 0x01u; }
static uint32_t f_rsvd(const gfx_cmd *c) { return (c->w[0] >> 14) & 0x03u; }
static uint32_t f_src(const gfx_cmd *c)  { return (c->w[0] >> 16) & 0x0Fu; }
static uint32_t f_dst(const gfx_cmd *c)  { return (c->w[0] >> 20) & 0x0Fu; }
static uint32_t f_c(const gfx_cmd *c)    { return (c->w[0] >> 24) & 0xFFu; }

static int32_t  lo_s(uint32_t v) { return (int32_t)(int16_t)(uint16_t)(v & 0xFFFFu); }
static int32_t  hi_s(uint32_t v) { return (int32_t)(int16_t)(uint16_t)((v >> 16) & 0xFFFFu); }
static uint32_t lo_u(uint32_t v) { return v & 0xFFFFu; }
static uint32_t hi_u(uint32_t v) { return (v >> 16) & 0xFFFFu; }

static uint32_t sf_base(const gfx_surface_table *t, uint32_t id)   { return t->s[id].base; }
static uint32_t sf_w(const gfx_surface_table *t, uint32_t id)      { return t->s[id].dim & 0xFFFFu; }
static uint32_t sf_h(const gfx_surface_table *t, uint32_t id)      { return (t->s[id].dim >> 16) & 0xFFFFu; }
static uint32_t sf_sl(const gfx_surface_table *t, uint32_t id)     { return t->s[id].ctrl & 0x1Fu; }
static uint32_t sf_space(const gfx_surface_table *t, uint32_t id)  { return (t->s[id].ctrl >> 8) & 0x03u; }
static uint32_t sf_fmt(const gfx_surface_table *t, uint32_t id)    { return (t->s[id].ctrl >> 12) & 0x0Fu; }
static uint32_t sf_valid(const gfx_surface_table *t, uint32_t id)  { return (t->s[id].ctrl >> 31) & 0x01u; }

/* 规范条款 §2 的六条结构谓词，逐条照抄。 */
static int ref_surf_ok(const gfx_surface_table *t, uint32_t id)
{
    uint32_t w, h, sl;
    uint64_t last;

    if (id >= GFX_SURF_N)          return 0;
    if (sf_valid(t, id) == 0u)     return 0;
    if ((sf_base(t, id) % 4u) != 0u) return 0;

    w = sf_w(t, id);
    h = sf_h(t, id);
    sl = sf_sl(t, id);

    if (w == 0u || h == 0u)        return 0;
    if (sl < 2u || sl > 31u)       return 0;
    if ((uint64_t)w * 4u > ((uint64_t)1u << sl)) return 0;

    last = (uint64_t)sf_base(t, id) + (uint64_t)(h - 1u) * ((uint64_t)1u << sl)
         + (uint64_t)(w - 1u) * 4u + 3u;
    if (last > 0xFFFFFFFFu)        return 0;

    if ((t->s[id].ctrl & GFX_SURF_CTRL_RSVD) != 0u) return 0;

    /* v1-TEX（G1）：space=1 的 base 是片上窗口内偏移，加 16 字节对齐与
       base + height*2^stride_log2 <= 0x4000 两条，64 位求值。 */
    if (sf_space(t, id) == GFX_SPACE_TEX) {
        uint64_t end;
        if ((sf_base(t, id) % 16u) != 0u) return 0;
        end = (uint64_t)sf_base(t, id) + (uint64_t)h * ((uint64_t)1u << sl);
        if (end > 0x4000u) return 0;
    }
    return 1;
}

/* 规范条款 §3 的字段矩阵，按行照抄；返回 1 表示有该为 0 而不为 0 的字段。 */
static int ref_bad_field(const gfx_cmd *c)
{
    uint32_t op = f_op(c);
    uint32_t eq = f_eq(c);

    if (f_rsvd(c) != 0u || f_tint(c) != 0u)
        return 1;

    if (op == GFX_OP_NOP || op == GFX_OP_END) {
        return (f_eq(c) | f_key(c) | f_src(c) | f_dst(c) | f_c(c)
              | c->w[1] | c->w[2] | c->w[3] | c->w[4] | c->w[5] | c->w[6]) != 0u;
    }
    if (op == GFX_OP_FILL) {
        if ((f_key(c) | f_src(c) | c->w[1] | c->w[5] | c->w[6]) != 0u)
            return 1;
        if (eq != GFX_EQ_CONST && f_c(c) != 0u)
            return 1;
        return 0;
    }
    if (op == GFX_OP_BLIT) {
        if ((c->w[4] | c->w[6]) != 0u)
            return 1;
        if (eq != GFX_EQ_CONST && f_c(c) != 0u)
            return 1;
        if (f_key(c) == 0u && c->w[5] != 0u)
            return 1;
        return 0;
    }
    /* SET_CLIP */
    return (f_eq(c) | f_key(c) | f_src(c) | f_dst(c) | f_c(c)
          | c->w[2] | c->w[4] | c->w[5] | c->w[6]) != 0u;
}

static int ref_fmt_eq_ok(uint32_t fmt, uint32_t eq)
{
    if (eq == GFX_EQ_ALPHA)
        return (fmt == GFX_FMT_XRGB8888) || (fmt == GFX_FMT_ARGB8888_PM);
    if (eq == GFX_EQ_ALPHA_STRAIGHT)
        return (fmt == GFX_FMT_XRGB8888) || (fmt == GFX_FMT_ARGB8888_ST);
    return (fmt == GFX_FMT_XRGB8888) || (fmt == GFX_FMT_ARGB8888_PM)
        || (fmt == GFX_FMT_ARGB8888_ST);
}

/* 规范条款 §5 的第二到第十步。 */
static int ref_check(const gfx_surface_table *t, const gfx_cmd *c)
{
    uint32_t op = f_op(c), eq = f_eq(c);
    uint32_t sid = f_src(c), did = f_dst(c);

    /* 2 操作码 */
    if (op != GFX_OP_NOP && op != GFX_OP_FILL && op != GFX_OP_BLIT
     && op != GFX_OP_SET_CLIP && op != GFX_OP_END)
        return GFX_E_BAD_OPCODE;

    /* 3 字段 */
    if (ref_bad_field(c))
        return GFX_E_BAD_FIELD;

    /* 4 方程 */
    if ((op == GFX_OP_FILL || op == GFX_OP_BLIT) && eq > GFX_EQ_MAX)
        return GFX_E_BAD_EQUATION;

    /* 5、6 源表面、目的表面 */
    if (op == GFX_OP_FILL) {
        if (!ref_surf_ok(t, did)) return GFX_E_BAD_SURFACE;
    } else if (op == GFX_OP_BLIT) {
        if (!ref_surf_ok(t, sid)) return GFX_E_BAD_SURFACE;
        if (!ref_surf_ok(t, did)) return GFX_E_BAD_SURFACE;
    }

    /* 7 space，源在前 */
    if (op == GFX_OP_FILL) {
        if (sf_space(t, did) != GFX_SPACE_DDR) return GFX_E_BAD_SPACE;
    } else if (op == GFX_OP_BLIT) {
        uint32_t ssp = sf_space(t, sid);
        if (ssp != GFX_SPACE_DDR && ssp != GFX_SPACE_TEX) return GFX_E_BAD_SPACE;
        if (sf_space(t, did) != GFX_SPACE_DDR) return GFX_E_BAD_SPACE;
    }

    /* 8 format，源在前 */
    if (op == GFX_OP_FILL) {
        if (sf_fmt(t, did) != GFX_FMT_XRGB8888) return GFX_E_BAD_FORMAT;
    } else if (op == GFX_OP_BLIT) {
        if (!ref_fmt_eq_ok(sf_fmt(t, sid), eq)) return GFX_E_BAD_FORMAT;
        if (sf_fmt(t, did) != GFX_FMT_XRGB8888) return GFX_E_BAD_FORMAT;
    }

    /* 9 ALPHA FILL 的预乘常量 */
    if (op == GFX_OP_FILL && eq == GFX_EQ_ALPHA) {
        uint32_t a = (c->w[4] >> 24) & 0xFFu;
        uint32_t r = (c->w[4] >> 16) & 0xFFu;
        uint32_t g = (c->w[4] >>  8) & 0xFFu;
        uint32_t b =  c->w[4]        & 0xFFu;

        if (r > a || g > a || b > a)
            return GFX_E_BAD_PREMULT;
    }

    /* 10 同表面 BLIT 重叠，未裁剪的半开区间 */
    if (op == GFX_OP_BLIT && sid == did) {
        uint32_t w = lo_u(c->w[3]), h = hi_u(c->w[3]);

        if (w > 0u && h > 0u) {
            int32_t sx = lo_s(c->w[1]), sy = hi_s(c->w[1]);
            int32_t dx = lo_s(c->w[2]), dy = hi_s(c->w[2]);
            int32_t sx1 = sx + (int32_t)w, sy1 = sy + (int32_t)h;
            int32_t dx1 = dx + (int32_t)w, dy1 = dy + (int32_t)h;
            int      sep = (sx1 <= dx) || (dx1 <= sx) || (sy1 <= dy) || (dy1 <= sy);

            if (!sep)
                return GFX_E_OVERLAP;
        }
    }

    return GFX_OK;
}

static uint32_t *ref_ptr(const gfx_mem *m, uint32_t addr)
{
    uint32_t off;

    if (addr < m->dev_base)
        return 0;
    off = addr - m->dev_base;
    if (off > m->bytes || m->bytes - off < 4u || (off & 3u) != 0u)
        return 0;
    return (uint32_t *)(void *)(m->host + off);
}

static uint32_t ref_addr(const gfx_surface_table *t, uint32_t id, int32_t x, int32_t y)
{
    return sf_base(t, id) + ((uint32_t)y << sf_sl(t, id)) + ((uint32_t)x * 4u);
}

/* v1-TEX（G1）：源表面的地址窗口。TEX 源经宿主的影子窗口；
   返回 0 表示宿主没给影子（按 BAD_SURFACE 停）。view 由调用方提供并保持存活。 */
static const gfx_mem *ref_surf_win(const gfx_mem *m, const gfx_surface_table *t,
                                   uint32_t id, gfx_mem *view)
{
    if (sf_space(t, id) != GFX_SPACE_TEX)
        return m;
    if (m->tex_host == 0)
        return 0;
    view->dev_base = 0u;
    view->host = m->tex_host;
    view->bytes = m->tex_bytes;
    view->tex_host = 0;
    view->tex_bytes = 0u;
    return view;
}

static int ref_surf_in_window(const gfx_surface_table *t, uint32_t id, const gfx_mem *m)
{
    uint32_t w = sf_w(t, id), h = sf_h(t, id);

    if (ref_ptr(m, sf_base(t, id)) == 0)
        return 0;
    return ref_ptr(m, ref_addr(t, id, (int32_t)w - 1, (int32_t)h - 1)) != 0;
}

/* 设计文档 3.5 节的方程表，逐行代入，系数以 255 为满量程。 */
static uint32_t ref_blend(uint32_t srcw, uint32_t dstw, uint32_t eq,
                          uint32_t c, uint32_t fmt)
{
    uint32_t a = (fmt == GFX_FMT_XRGB8888) ? 255u : ((srcw >> 24) & 0xFFu);
    uint32_t r, g, b;
    uint32_t fs[3], fd[3];
    uint32_t sc[3], dc[3];
    uint32_t i, o[3];

    sc[0] = (srcw >> 16) & 0xFFu; sc[1] = (srcw >> 8) & 0xFFu; sc[2] = srcw & 0xFFu;
    dc[0] = (dstw >> 16) & 0xFFu; dc[1] = (dstw >> 8) & 0xFFu; dc[2] = dstw & 0xFFu;

    for (i = 0u; i < 3u; i++) {
        if (eq == GFX_EQ_COPY)                  { fs[i] = 255u;  fd[i] = 0u; }
        else if (eq == GFX_EQ_ALPHA)            { fs[i] = 255u;  fd[i] = 255u - a; }
        else if (eq == GFX_EQ_ADD)              { fs[i] = 255u;  fd[i] = 255u; }
        else if (eq == GFX_EQ_CONST)            { fs[i] = c;     fd[i] = 255u - c; }
        else if (eq == GFX_EQ_MUL)              { fs[i] = dc[i]; fd[i] = 0u; }
        else                                    { fs[i] = a;     fd[i] = 255u - a; }
        o[i] = ref_div255(sc[i] * fs[i] + dc[i] * fd[i]);
    }

    r = o[0]; g = o[1]; b = o[2];
    return (r << 16) | (g << 8) | b;            /* 高字节恒 0 */
}

void ref_exec_reset(gfx_rect *clip)
{
    clip->x0 = 0;
    clip->y0 = 0;
    clip->x1 = GFX_SCREEN_W - 1;
    clip->y1 = GFX_SCREEN_H - 1;
}

static int32_t imax(int32_t a, int32_t b) { return (a > b) ? a : b; }
static int32_t imin(int32_t a, int32_t b) { return (a < b) ? a : b; }

int ref_exec_run(gfx_rect *clip, const gfx_surface_table *t, const gfx_mem *m,
                 const gfx_cmd *list, uint32_t list_count, uint32_t readable_count,
                 exec_sw_stats *st, exec_sw_error *err)
{
    uint32_t idx;
    gfx_mem sv;
    const gfx_mem *swin;

    if (err != 0) {
        err->code = GFX_OK;
        err->index = 0u;
        err->tag = 0u;
    }

    for (idx = 0u; idx < list_count; idx++) {
        const gfx_cmd *c;
        uint32_t       op, eq, sid, did, w, h, fmt;
        int32_t        dx, dy, x0, y0, x1, y1, px, py;
        int            code;

        if (st != 0)
            st->records_read++;

        if (idx >= readable_count) {            /* 1 有界取数 */
            if (err != 0) { err->code = GFX_E_FETCH; err->index = idx; err->tag = 0u; }
            return GFX_E_FETCH;
        }
        c = &list[idx];

        code = ref_check(t, c);
        if (code != GFX_OK) {
            if (err != 0) { err->code = code; err->index = idx; err->tag = c->w[7]; }
            return code;
        }

        op = f_op(c);
        if (op == GFX_OP_END)
            return GFX_OK;
        if (op == GFX_OP_NOP) {
            if (st != 0) st->cmd++;
            continue;
        }
        if (op == GFX_OP_SET_CLIP) {
            int32_t cx = lo_s(c->w[1]), cy = hi_s(c->w[1]);
            int32_t cw = (int32_t)lo_u(c->w[3]), ch = (int32_t)hi_u(c->w[3]);

            clip->x0 = imax(cx, 0);
            clip->y0 = imax(cy, 0);
            clip->x1 = imin(cx + cw - 1, GFX_SCREEN_W - 1);
            clip->y1 = imin(cy + ch - 1, GFX_SCREEN_H - 1);
            if (st != 0) st->cmd++;
            continue;
        }

        eq  = f_eq(c);
        sid = f_src(c);
        did = f_dst(c);
        w   = lo_u(c->w[3]);
        h   = hi_u(c->w[3]);
        dx  = lo_s(c->w[2]);
        dy  = hi_s(c->w[2]);

        sv.dev_base = 0u; sv.host = 0; sv.bytes = 0u; sv.tex_host = 0; sv.tex_bytes = 0u;
        swin = m;
        if (!ref_surf_in_window(t, did, m)) {
            if (err != 0) { err->code = GFX_E_BAD_SURFACE; err->index = idx; err->tag = c->w[7]; }
            return GFX_E_BAD_SURFACE;
        }
        if (op == GFX_OP_BLIT) {
            swin = ref_surf_win(m, t, sid, &sv);
            if (swin == 0 || !ref_surf_in_window(t, sid, swin)) {
                if (err != 0) { err->code = GFX_E_BAD_SURFACE; err->index = idx; err->tag = c->w[7]; }
                return GFX_E_BAD_SURFACE;
            }
        }

        /* spec 第 01 章 3.6 节：宽高先判 0，再四者求交，闭区间 */
        if (w == 0u || h == 0u) {
            if (st != 0) st->cmd++;
            continue;
        }
        x0 = imax(imax(0, dx), clip->x0);
        y0 = imax(imax(0, dy), clip->y0);
        x1 = imin(imin(GFX_SCREEN_W - 1, dx + (int32_t)w - 1),
                  imin(clip->x1, (int32_t)sf_w(t, did) - 1));
        y1 = imin(imin(GFX_SCREEN_H - 1, dy + (int32_t)h - 1),
                  imin(clip->y1, (int32_t)sf_h(t, did) - 1));
        if (x1 < x0 || y1 < y0) {
            if (st != 0) st->cmd++;
            continue;
        }

        fmt = (op == GFX_OP_FILL)
            ? ((eq == GFX_EQ_ALPHA) ? GFX_FMT_ARGB8888_PM
              : (eq == GFX_EQ_ALPHA_STRAIGHT) ? GFX_FMT_ARGB8888_ST
              : GFX_FMT_XRGB8888)
            : sf_fmt(t, sid);

        for (py = y0; py <= y1; py++) {
            for (px = x0; px <= x1; px++) {
                uint32_t  srcw;
                uint32_t *dp;

                if (st != 0) st->pixel_total++;

                if (op == GFX_OP_BLIT) {
                    int32_t sx = lo_s(c->w[1]) + (px - dx);
                    int32_t sy = hi_s(c->w[1]) + (py - dy);

                    if (sx < 0 || sx > (int32_t)sf_w(t, sid) - 1
                     || sy < 0 || sy > (int32_t)sf_h(t, sid) - 1) {
                        if (st != 0) st->src_oob++;
                        continue;               /* 源越界只跳过该像素 */
                    }
                    srcw = *ref_ptr(swin, ref_addr(t, sid, sx, sy));
                    if (f_key(c) != 0u && srcw == c->w[5]) {
                        if (st != 0) st->key_skip++;
                        continue;
                    }
                } else {
                    srcw = c->w[4];
                }

                dp = ref_ptr(m, ref_addr(t, did, px, py));
                *dp = ref_blend(srcw, *dp, eq, f_c(c), fmt);
                if (st != 0) st->pixel_written++;
            }
        }

        if (st != 0) st->cmd++;
    }

    return GFX_OK;
}
