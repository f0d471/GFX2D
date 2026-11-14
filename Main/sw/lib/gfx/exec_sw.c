/******************************************************************************
* @file exec_sw.c: 软件执行器
*
* @brief  按命令语义规范逐像素执行一张命令表。它有三个用途
*         （设计文档 3.8 节）：PC 上的渲染器、板上的软件渲染模式与对比基线、
*         引擎仿真与上板 CRC 的金标准。
*
*         结构固定为：有界取数 -> gfx_cmd_check()（十步检查的第二到第十步）
*         -> 裁剪（四者求交）-> 逐像素。
*
*         混合的算术只写一份 `blend_px()`，六个方程的循环由宏各生成一份特化：
*         调用点的 eq 是编译期常量，里面的 switch 会被折叠掉，
*         所以「外层按方程选一次、内层不按像素切换方程」与
*         「方程系数只有一处定义」两件事同时成立。
*
*         注错开关每个方程一个（-DINJECT_EQ_COPY 等），只把该方程输出的
*         最低位取反：用到该方程的用例 CRC 会变，其余用例不受影响。
******************************************************************************/

#include "gfx/gfx.h"
#include <string.h>

/* V 顺序定稿：两项先相加，再一次 ÷255 舍入，最后 clamp。
   中间量用 uint32_t，两项之和上限 130050 需要 17 位，C 里自然满足；
   不能压到 16 位模截断（那样全域有 64643 处错）。 */
static inline uint32_t r255(uint32_t x)
{
    uint32_t t = x + 128u;
    uint32_t q = (t + (t >> 8)) >> 8;

    return (q > 255u) ? 255u : q;
}

/* 一个像素的混合。系数表是设计文档 3.5 节那六行，以 255 为满量程代入：
   表里的 1 在实现里是 255，照字面写成 1 会让源色权重掉到 1/255。
   只算 R、G、B 三个通道，输出的高字节恒为 0（写 XRGB8888 帧缓冲的规定）。 */
static inline uint32_t blend_px(uint32_t srcw, uint32_t dstw,
                                uint32_t eq, uint32_t c, uint32_t sfmt)
{
    uint32_t a   = (sfmt == GFX_FMT_XRGB8888) ? 255u : ((srcw >> 24) & 0xFFu);
    uint32_t out = 0u;
    uint32_t i;

    for (i = 0u; i < 3u; i++) {                 /* 0=B、1=G、2=R */
        uint32_t s = (srcw >> (8u * i)) & 0xFFu;
        uint32_t d = (dstw >> (8u * i)) & 0xFFu;
        uint32_t fs, fd;

        switch (eq) {
        case GFX_EQ_COPY:  fs = 255u; fd = 0u;        break;
        case GFX_EQ_ALPHA: fs = 255u; fd = 255u - a;  break;
        case GFX_EQ_ADD:   fs = 255u; fd = 255u;      break;
        case GFX_EQ_CONST: fs = c;    fd = 255u - c;  break;
        case GFX_EQ_MUL:   fs = d;    fd = 0u;        break;
        default:           fs = a;    fd = 255u - a;  break;   /* ALPHA_STRAIGHT */
        }
        out |= r255(s * fs + d * fd) << (8u * i);
    }

#ifdef INJECT_EQ_COPY
    if (eq == GFX_EQ_COPY)           out ^= 1u;
#endif
#ifdef INJECT_EQ_ALPHA
    if (eq == GFX_EQ_ALPHA)          out ^= 1u;
#endif
#ifdef INJECT_EQ_ADD
    if (eq == GFX_EQ_ADD)            out ^= 1u;
#endif
#ifdef INJECT_EQ_CONST
    if (eq == GFX_EQ_CONST)          out ^= 1u;
#endif
#ifdef INJECT_EQ_MUL
    if (eq == GFX_EQ_MUL)            out ^= 1u;
#endif
#ifdef INJECT_EQ_ALPHA_STRAIGHT
    if (eq == GFX_EQ_ALPHA_STRAIGHT) out ^= 1u;
#endif

    return out & 0x00FFFFFFu;
}

/* 设备地址换算成主机指针。板上 dev_base 设成 host 所指的地址即为恒等映射。 */
static inline uint32_t *dev_ptr(const gfx_mem *m, uint32_t addr)
{
    uint32_t off;

    if (addr < m->dev_base)
        return 0;
    off = addr - m->dev_base;
    if (off > m->bytes || m->bytes - off < 4u || (off & 3u) != 0u)
        return 0;
    return (uint32_t *)(void *)(m->host + off);
}

/* 表面的地址窗口：space=1（TEX）经宿主的 TEX 影子窗口，其余走设备窗口。
   返回 0 表示宿主没有提供所需的窗口（调用方违约，按 BAD_SURFACE 停下，
   与 PC 上给错 DDR 窗口同一处理）。tex_view 是调用方提供的暂存视图。 */
static const gfx_mem *surf_window(const gfx_mem *m, const gfx_surf_raw *s,
                                  gfx_mem *tex_view)
{
    if (gfx_surf_space(s) != GFX_SPACE_TEX)
        return m;
    if (m->tex_host == 0)
        return 0;
    tex_view->dev_base = 0u;
    tex_view->host     = m->tex_host;
    tex_view->bytes    = m->tex_bytes;
    tex_view->tex_host = 0;
    tex_view->tex_bytes = 0u;
    return tex_view;
}

/* 整张表面是否落在窗口内。表面的最后一个字节由描述符算出，
   地址随 x、y 单调增，所以首尾两点在窗口内就够。

   这一条不属于 ABI 的十步：窗口是软件执行器的宿主约束，
   板上恒等映射且窗口覆盖整块 DDR 时永远成立，走不到。
   PC 上给错窗口属于调用方违约，按 BAD_SURFACE 停下，不在像素循环里中途失败。 */
static int surf_in_window(const gfx_mem *m, const gfx_surf_raw *s)
{
    uint32_t w = gfx_surf_width(s);
    uint32_t h = gfx_surf_height(s);
    uint32_t last;

    if (dev_ptr(m, s->base) == 0)
        return 0;
    last = s->base + ((h - 1u) << gfx_surf_stride_log2(s)) + ((w - 1u) << 2);
    return dev_ptr(m, last) != 0;
}

/* 四者求交：目的矩形、游戏区、SET_CLIP 框、目的表面。
   宽高为 0 先判，避免 w - 1 在无符号下溢。返回 0 表示没有要画的像素。 */
static int dst_window(const gfx_surface_table *t, const gfx_rect *clip,
                      uint32_t dstid, int32_t dx, int32_t dy,
                      uint32_t w, uint32_t h, gfx_rect *out,unsigned frame_w,unsigned frame_h)
{
    const gfx_surf_raw *s = &t->s[dstid];
    int32_t             x0, y0, x1, y1;

    if (w == 0u || h == 0u)
        return 0;

    x0 = 0;
    if (dx > x0)       x0 = dx;
    if (clip->x0 > x0) x0 = clip->x0;

    y0 = 0;
    if (dy > y0)       y0 = dy;
    if (clip->y0 > y0) y0 = clip->y0;

    x1 = (int32_t)frame_w - 1;
    if (dx + (int32_t)w - 1 < x1)            x1 = dx + (int32_t)w - 1;
    if (clip->x1 < x1)                       x1 = clip->x1;
    if ((int32_t)gfx_surf_width(s) - 1 < x1) x1 = (int32_t)gfx_surf_width(s) - 1;

    y1 = (int32_t)frame_h - 1;
    if (dy + (int32_t)h - 1 < y1)             y1 = dy + (int32_t)h - 1;
    if (clip->y1 < y1)                        y1 = clip->y1;
    if ((int32_t)gfx_surf_height(s) - 1 < y1) y1 = (int32_t)gfx_surf_height(s) - 1;

    if (x1 < x0 || y1 < y0)
        return 0;

    out->x0 = x0; out->y0 = y0; out->x1 = x1; out->y1 = y1;
    return 1;
}

/* FILL 与 BLIT 各一套循环，六个方程各生成一份特化。
   EQ 传进来是编译期常量，blend_px 里的 switch 在 -O2 下被折叠。 */

#define DEF_FILL_LOOP(fn, EQ)                                                  \
static void fn(const gfx_surf_raw *ds, const gfx_mem *m, const gfx_rect *win,  \
               uint32_t srcw, uint32_t c, uint32_t sfmt, exec_sw_stats *st)    \
{                                                                              \
    int32_t py, px;                                                            \
    for (py = win->y0; py <= win->y1; py++) {                                  \
        for (px = win->x0; px <= win->x1; px++) {                              \
            uint32_t *p = dev_ptr(m, gfx_addr_of(ds, px, py));                 \
            *p = blend_px(srcw, *p, (EQ), c, sfmt);                            \
        }                                                                      \
    }                                                                          \
    if (st != 0) {                                                             \
        uint32_t n = (uint32_t)(win->x1 - win->x0 + 1)                         \
                   * (uint32_t)(win->y1 - win->y0 + 1);                        \
        st->pixel_total   += n;                                                \
        st->pixel_written += n;                                                \
    }                                                                          \
}

DEF_FILL_LOOP(fill_copy,     GFX_EQ_COPY)
DEF_FILL_LOOP(fill_alpha,    GFX_EQ_ALPHA)
DEF_FILL_LOOP(fill_add,      GFX_EQ_ADD)
DEF_FILL_LOOP(fill_const,    GFX_EQ_CONST)
DEF_FILL_LOOP(fill_mul,      GFX_EQ_MUL)
DEF_FILL_LOOP(fill_straight, GFX_EQ_ALPHA_STRAIGHT)

/* BLIT 的一个像素有三种结局：源越界跳过、色键命中跳过、写回。
   顺序固定：先判源越界，再判色键。
   ms 是源表面窗口（v1-TEX 的 TEX 源经影子窗口），m 是目的窗口。 */
#define DEF_BLIT_LOOP(fn, EQ)                                                  \
static void fn(const gfx_surf_raw *ss, const gfx_surf_raw *ds,                 \
               const gfx_mem *ms, const gfx_mem *m, const gfx_rect *win,       \
               int32_t sx0, int32_t sy0, int32_t dx, int32_t dy,               \
               uint32_t key_en, uint32_t key, uint32_t c, uint32_t sfmt,       \
               exec_sw_stats *st)                                              \
{                                                                              \
    int32_t  sw = (int32_t)gfx_surf_width(ss);                                 \
    int32_t  sh = (int32_t)gfx_surf_height(ss);                                \
    uint32_t total = 0u, written = 0u, skipped = 0u, oob = 0u;                 \
    int32_t  py, px;                                                           \
    for (py = win->y0; py <= win->y1; py++) {                                  \
        int32_t sy = sy0 + (py - dy);                                          \
        for (px = win->x0; px <= win->x1; px++) {                              \
            int32_t   sx = sx0 + (px - dx);                                    \
            uint32_t  srcw;                                                    \
            uint32_t *p;                                                       \
            total++;                                                           \
            if (sx < 0 || sx >= sw || sy < 0 || sy >= sh) { oob++; continue; } \
            srcw = *dev_ptr(ms, gfx_addr_of(ss, sx, sy));                      \
            if (key_en != 0u && srcw == key) { skipped++; continue; }          \
            p = dev_ptr(m, gfx_addr_of(ds, px, py));                           \
            *p = blend_px(srcw, *p, (EQ), c, sfmt);                            \
            written++;                                                         \
        }                                                                      \
    }                                                                          \
    if (st != 0) {                                                             \
        st->pixel_total   += total;                                            \
        st->pixel_written += written;                                          \
        st->key_skip      += skipped;                                          \
        st->src_oob       += oob;                                              \
    }                                                                          \
}

DEF_BLIT_LOOP(blit_copy,     GFX_EQ_COPY)
DEF_BLIT_LOOP(blit_alpha,    GFX_EQ_ALPHA)
DEF_BLIT_LOOP(blit_add,      GFX_EQ_ADD)
DEF_BLIT_LOOP(blit_const,    GFX_EQ_CONST)
DEF_BLIT_LOOP(blit_mul,      GFX_EQ_MUL)
DEF_BLIT_LOOP(blit_straight, GFX_EQ_ALPHA_STRAIGHT)

void exec_sw_reset(exec_sw_state *es)
{
    if (es == 0)
        return;
    es->clip.x0 = 0;
    es->clip.y0 = 0;
    es->clip.x1 = GFX_SCREEN_W - 1;
    es->clip.y1 = GFX_SCREEN_H - 1;
}

void exec_sw_stats_clear(exec_sw_stats *st)
{
    if (st == 0)
        return;
    st->cmd           = 0u;
    st->records_read  = 0u;
    st->pixel_total   = 0u;
    st->pixel_written = 0u;
    st->key_skip      = 0u;
    st->src_oob       = 0u;
}

/* SET_CLIP：w1 是左上角、w3 是尺寸，闭区间，新框替换旧框。
   「只能缩小」是相对游戏区而言，所以截断在设置时做；
   游戏区本来就是四者求交的一项，设置时截断与使用时求交结果相同。 */
static void apply_set_clip(exec_sw_state *es,const gfx_cmd *c,unsigned frame_w,unsigned frame_h)
{
    int32_t x0 = gfx_s16(c->w[1]);
    int32_t y0 = gfx_s16(c->w[1] >> 16);
    int32_t x1 = x0 + (int32_t)gfx_u16(c->w[3]) - 1;
    int32_t y1 = y0 + (int32_t)gfx_u16(c->w[3] >> 16) - 1;

    es->clip.x0 = (x0 > 0) ? x0 : 0;
    es->clip.y0 = (y0 > 0) ? y0 : 0;
    es->clip.x1 = (x1 < (int32_t)frame_w - 1) ? x1 : (int32_t)frame_w - 1;
    es->clip.y1 = (y1 < (int32_t)frame_h - 1) ? y1 : (int32_t)frame_h - 1;
}

static int fail(exec_sw_error *err, int code, uint32_t index, uint32_t tag)
{
    if (err != 0) {
        err->code  = code;
        err->index = index;
        err->tag   = tag;
    }
    return code;
}

static int run_core(const gfx_mem *source_m, exec_sw_state *es, const gfx_surface_table *t, const gfx_mem *m,
                const gfx_cmd *list, uint32_t list_count, uint32_t readable_count,
                exec_sw_stats *st, exec_sw_error *err,unsigned frame_w,unsigned frame_h)
{
    uint32_t idx;

    if (es == 0 || t == 0 || m == 0)
        return fail(err, GFX_E_BAD_SURFACE, 0u, 0u);

    if (err != 0) {
        err->code  = GFX_OK;
        err->index = 0u;
        err->tag   = 0u;
    }

    for (idx = 0u; idx < list_count; idx++) {
        const gfx_cmd *c;
        uint32_t       op, eq, srcid, dstid, w, h, sfmt;
        gfx_rect       win;
        gfx_mem        sv;              /* TEX 源的影子窗口视图 */
        const gfx_mem *swin;            /* 本条命令的源表面窗口 */
        int            code;

        if (st != 0)
            st->records_read++;

        /* 第一步：有界取数。越界不解引用，返回 FETCH，标签读 0。 */
        if (idx >= readable_count || list == 0)
            return fail(err, GFX_E_FETCH, idx, 0u);

        c = &list[idx];

        /* 第二到第十步。 */
        code = gfx_cmd_check(t, c);
        if (code != GFX_OK)
            return fail(err, code, idx, c->w[7]);

        op = gfx_op(c);
        if (op == GFX_OP_END)
            return GFX_OK;                      /* END 不计进 cmd */
        if (op == GFX_OP_NOP) {
            if (st != 0) st->cmd++;
            continue;
        }
        if (op == GFX_OP_SET_CLIP) {
            apply_set_clip(es,c,frame_w,frame_h);
            if (st != 0) st->cmd++;
            continue;
        }

        eq    = gfx_eq(c);
        srcid = gfx_src(c);
        dstid = gfx_dst(c);
        w     = gfx_u16(c->w[3]);
        h     = gfx_u16(c->w[3] >> 16);

        /* 宿主约束：本命令用到的表面要整个落在地址窗口内。
           目的恒走设备窗口（space 步已排除 TEX 目的）；BLIT 源按 space 选窗口，
           TEX 源经宿主的影子窗口，没给影子按 BAD_SURFACE 停。sv 必须活在
           整个循环体里——swin 指向它，像素循环还要用。 */
        sv.dev_base = 0u; sv.host = 0; sv.bytes = 0u;
        sv.tex_host = 0; sv.tex_bytes = 0u;
        swin = m;
        if (!surf_in_window(m, &t->s[dstid]))
            return fail(err, GFX_E_BAD_SURFACE, idx, c->w[7]);
        if (op == GFX_OP_BLIT) {
            swin = surf_window(source_m, &t->s[srcid], &sv);
            if (swin == 0 || !surf_in_window(swin, &t->s[srcid]))
                return fail(err, GFX_E_BAD_SURFACE, idx, c->w[7]);
        }

        if (!dst_window(t, &es->clip, dstid, gfx_s16(c->w[2]),
                        gfx_s16(c->w[2] >> 16), w, h, &win,frame_w,frame_h)) {
            if (st != 0) st->cmd++;             /* 零面积或全裁掉，仍是成功执行完 */
            continue;
        }

        if (op == GFX_OP_FILL) {
            /* FILL 的常量源字按方程解释，规范条款 §4.2。 */
            sfmt = (eq == GFX_EQ_ALPHA) ? GFX_FMT_ARGB8888_PM
                 : (eq == GFX_EQ_ALPHA_STRAIGHT) ? GFX_FMT_ARGB8888_ST
                 : GFX_FMT_XRGB8888;
            switch (eq) {
            case GFX_EQ_COPY:
                fill_copy(&t->s[dstid], m, &win, c->w[4], gfx_c(c), sfmt, st);
                break;
            case GFX_EQ_ALPHA:
                fill_alpha(&t->s[dstid], m, &win, c->w[4], gfx_c(c), sfmt, st);
                break;
            case GFX_EQ_ADD:
                fill_add(&t->s[dstid], m, &win, c->w[4], gfx_c(c), sfmt, st);
                break;
            case GFX_EQ_CONST:
                fill_const(&t->s[dstid], m, &win, c->w[4], gfx_c(c), sfmt, st);
                break;
            case GFX_EQ_MUL:
                fill_mul(&t->s[dstid], m, &win, c->w[4], gfx_c(c), sfmt, st);
                break;
            default:
                fill_straight(&t->s[dstid], m, &win, c->w[4], gfx_c(c), sfmt, st);
                break;
            }
        } else {
            int32_t  sx0   = gfx_s16(c->w[1]);
            int32_t  sy0   = gfx_s16(c->w[1] >> 16);
            int32_t  dx    = gfx_s16(c->w[2]);
            int32_t  dy    = gfx_s16(c->w[2] >> 16);
            uint32_t keyen = gfx_key(c);

            sfmt = gfx_surf_format(&t->s[srcid]);
            switch (eq) {
            case GFX_EQ_COPY:
                blit_copy(&t->s[srcid], &t->s[dstid], swin, m, &win, sx0, sy0, dx, dy,
                          keyen, c->w[5], gfx_c(c), sfmt, st);
                break;
            case GFX_EQ_ALPHA:
                blit_alpha(&t->s[srcid], &t->s[dstid], swin, m, &win, sx0, sy0, dx, dy,
                           keyen, c->w[5], gfx_c(c), sfmt, st);
                break;
            case GFX_EQ_ADD:
                blit_add(&t->s[srcid], &t->s[dstid], swin, m, &win, sx0, sy0, dx, dy,
                         keyen, c->w[5], gfx_c(c), sfmt, st);
                break;
            case GFX_EQ_CONST:
                blit_const(&t->s[srcid], &t->s[dstid], swin, m, &win, sx0, sy0, dx, dy,
                           keyen, c->w[5], gfx_c(c), sfmt, st);
                break;
            case GFX_EQ_MUL:
                blit_mul(&t->s[srcid], &t->s[dstid], swin, m, &win, sx0, sy0, dx, dy,
                         keyen, c->w[5], gfx_c(c), sfmt, st);
                break;
            default:
                blit_straight(&t->s[srcid], &t->s[dstid], swin, m, &win, sx0, sy0, dx, dy,
                              keyen, c->w[5], gfx_c(c), sfmt, st);
                break;
            }
        }

        if (st != 0)
            st->cmd++;
    }

    return GFX_OK;                              /* COUNT 耗尽也是正常结束 */
}

int exec_sw_run(exec_sw_state *es, const gfx_surface_table *t, const gfx_mem *m,
                const gfx_cmd *list, uint32_t list_count, uint32_t readable_count,
                exec_sw_stats *st, exec_sw_error *err)
{
    return run_core(m,es,t,m,list,list_count,readable_count,st,err,GFX_SCREEN_W,GFX_SCREEN_H);
}

int exec_sw_run_frame(exec_sw_state *es,const gfx_surface_table *t,const gfx_mem *m,
                      const gfx_cmd *list,uint32_t count,uint32_t readable,
                      exec_sw_stats *stats,exec_sw_error *error,unsigned width,unsigned height)
{
    if(width<640u || width>1792u || height!=480u)return fail(error,GFX_E_BAD_SURFACE,0,0);
    return run_core(m,es,t,m,list,count,readable,stats,error,width,height);
}

int exec_sw_run_strips(const gfx_surface_table *t, const gfx_mem *m,
                       const void *table, uint32_t readable_bytes,
                       const gfx_strip_config *cfg, void *scratch, uint32_t scratch_bytes,
                       exec_sw_stats *st, exec_sw_error *err)
{
    uint32_t s,k,y,sh;
    int e;
    const uint32_t *counts=table;
    const gfx_surf_raw *bg,*dst;
    gfx_surface_table shadow;
    gfx_mem local;
    exec_sw_state es;
    if(err) *err=(exec_sw_error){GFX_OK,0u,0u};
    e=gfx_strip_config_check(t,cfg);
    if(e) return fail(err,e,0u,0u);
    sh=1u<<cfg->strip_log2;
    if(!m || !scratch || ((uintptr_t)scratch&3u) || scratch_bytes<(4096u*sh))
        return fail(err,GFX_E_BAD_SURFACE,0u,0u);
    if(!table || ((uintptr_t)table&3u) || readable_bytes<128u)
        return fail(err,GFX_E_FETCH,0u,0u);
    bg=&t->s[cfg->bg_id];dst=&t->s[cfg->dst_id];
    if(!surf_in_window(m,bg) || !surf_in_window(m,dst))
        return fail(err,GFX_E_BAD_SURFACE,0u,0u);
    shadow=*t;
    shadow.s[cfg->dst_id].base=0u;
    shadow.s[cfg->dst_id].dim=(sh<<16)|640u;
    shadow.s[cfg->dst_id].ctrl=0x8000000Cu;
    local=(gfx_mem){0u,scratch,scratch_bytes,0,0u};
    exec_sw_reset(&es);
    for(s=0;s<cfg->lstrips;s++) {
        uint32_t off=(s+1u)*GFX_STRIP_REGION_BYTES;
        const gfx_cmd *bucket=(const gfx_cmd *)(const void *)((const uint8_t *)table+off);
        if(counts[s]>GFX_STRIP_CAP)return fail(err,GFX_E_BAD_FIELD,s*GFX_STRIP_CAP,0u);
        for(y=0;y<sh;y++)
            memcpy((uint8_t *)scratch+y*4096u,dev_ptr(m,gfx_addr_of(bg,0,(int32_t)(s*sh+y))),2560u);
        for(k=0;k<counts[s];k++) {
            exec_sw_error local_err;
            uint32_t idx=s*GFX_STRIP_CAP+k;
            if(off+k*32u+32u>readable_bytes) return fail(err,GFX_E_FETCH,idx,0u);
            e=gfx_strip_cmd_check(t,cfg,&bucket[k]);
            if(e)return fail(err,e,idx,bucket[k].w[7]);
            const gfx_cmd *c=&bucket[k];
            e=run_core(m,&es,&shadow,&local,c,1u,1u,st,&local_err,GFX_SCREEN_W,GFX_SCREEN_H);
            if(e)return fail(err,e,idx,c->w[7]);
        }
        for(y=0;y<sh;y++)
            memcpy(dev_ptr(m,gfx_addr_of(dst,0,(int32_t)(s*sh+y))),
                   (const uint8_t *)scratch+y*4096u,2560u);
    }
    return GFX_OK;
}
