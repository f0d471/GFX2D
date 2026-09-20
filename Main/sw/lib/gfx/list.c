/******************************************************************************
* @file list.c: 表面表登记与命令表构造
*
* @brief  游戏调用的那一层：把参数打包成 32 字节记录，追加进命令表。
*         游戏代码里不出现位移与掩码。
*
*         位域用移位与掩码打包，不用 C 的位域语法 —— 位域的排列由编译器决定，
*         而这些字节要被硬件按固定位置解释。
*
*         构造层不自己写一套检查：打包完调用 gfx_cmd_check()，与执行器同一份。
*         这里的早检只是让错误尽早暴露，不替代执行时重查，
*         因为表面表可能在构造完成之后、执行之前被改写。
******************************************************************************/

#include "gfx/gfx.h"

/* 追加被拒时记下首错与计数。被拒的调用不写缓冲区，也不占记录索引，
   所以后面成功记录的 w7 仍然等于它在表里的零基索引。 */
static int reject(gfx_list *l, int code)
{
    l->rejected++;
    if (l->first_error == GFX_OK)
        l->first_error = code;
    return code;
}

/* 能不能放，先于内容对不对：CLOSED 与 FULL 与记录本身无关。 */
static int append(gfx_list *l, gfx_cmd *c)
{
    int code;

    if (l->ended)
        return reject(l, GFX_BUILD_CLOSED);
    if (l->used >= l->cap) {
        l->overflow++;
        return reject(l, GFX_BUILD_FULL);
    }

    c->w[7] = l->used;                          /* 默认标签就是零基索引 */

    code = gfx_cmd_check(l->t, c);
    if (code != GFX_OK)
        return reject(l, code);

    l->buf[l->used] = *c;
    l->used++;
    return GFX_OK;
}

static void zero_cmd(gfx_cmd *c)
{
    uint32_t i;

    for (i = 0u; i < 8u; i++)
        c->w[i] = 0u;
}

/* 坐标要能原样放进 int16、宽高要能原样放进 uint16，否则不追加也不截断。 */
static int coord_ok(int v)
{
    return v >= -32768 && v <= 32767;
}

static int size_ok(int v)
{
    return v >= 0 && v <= 65535;
}

static int surf_id_ok(int id)
{
    return id >= 0 && id < (int)GFX_SURF_N;
}

static uint32_t pack_pair(int lo, int hi)
{
    return ((uint32_t)lo & 0xFFFFu) | (((uint32_t)hi & 0xFFFFu) << 16);
}

void gfx_surface_set_raw(gfx_surface_table *t, int id,
                         uint32_t base, uint32_t dim, uint32_t ctrl)
{
    if (t == 0 || !surf_id_ok(id))
        return;
    t->s[id].base = base;
    t->s[id].dim  = dim;
    t->s[id].ctrl = ctrl;
}

void gfx_surface_reset(gfx_surface_table *t)
{
    uint32_t i;

    if (t == 0)
        return;
    for (i = 0u; i < GFX_SURF_N; i++) {
        t->s[i].base = 0u;
        t->s[i].dim  = 0u;
        t->s[i].ctrl = 0u;
    }
}

int gfx_surface_set(gfx_surface_table *t, int id, const gfx_surface *s)
{
    gfx_surf_raw save;
    uint32_t     ctrl;

    if (t == 0 || s == 0 || !surf_id_ok(id))
        return GFX_BUILD_BAD_ARG;
    if (s->stride_log2 > 31u || s->space > 3u || s->format > 15u)
        return GFX_BUILD_BAD_ARG;

    ctrl = ((uint32_t)s->stride_log2 & 0x1Fu)
         | (((uint32_t)s->space  & 3u)   <<  8)
         | (((uint32_t)s->format & 0xFu) << 12)
         | 0x80000000u;                         /* valid 最后置，见规范条款 §2 */

    save = t->s[id];
    t->s[id].base = s->base;
    t->s[id].dim  = pack_pair((int)s->width, (int)s->height);
    t->s[id].ctrl = ctrl;

    /* 结构不合法就原样退回，不留下一个半登记的槽位。 */
    if (!gfx_surface_struct_ok(t, (uint32_t)id)) {
        t->s[id] = save;
        return GFX_BUILD_BAD_ARG;
    }
    return GFX_OK;
}

void gfx_list_begin(gfx_list *l, gfx_cmd *buf, uint32_t cap,
                    const gfx_surface_table *t)
{
    if (l == 0)
        return;
    l->buf         = buf;
    l->cap         = (buf == 0) ? 0u : cap;
    l->used        = 0u;
    l->t           = t;
    l->first_error = GFX_OK;
    l->rejected    = 0u;
    l->overflow    = 0u;
    l->ended       = 0;
}

int gfx_nop(gfx_list *l)
{
    gfx_cmd c;

    if (l == 0 || l->t == 0)
        return GFX_BUILD_BAD_ARG;
    zero_cmd(&c);
    c.w[0] = GFX_OP_NOP;
    return append(l, &c);
}

int gfx_fill(gfx_list *l, int dst, gfx_rect r, uint32_t argb, int eq, uint8_t c)
{
    gfx_cmd  rec;
    int      w, h;

    if (l == 0 || l->t == 0)
        return GFX_BUILD_BAD_ARG;
    if (!surf_id_ok(dst) || eq < 0 || eq > 15)
        return reject(l, GFX_BUILD_BAD_ARG);
    if (!coord_ok((int)r.x0) || !coord_ok((int)r.y0))
        return reject(l, GFX_BUILD_BAD_ARG);

    w = (r.x1 < r.x0) ? 0 : (int)(r.x1 - r.x0 + 1);
    h = (r.y1 < r.y0) ? 0 : (int)(r.y1 - r.y0 + 1);
    if (!size_ok(w) || !size_ok(h))
        return reject(l, GFX_BUILD_BAD_ARG);

    zero_cmd(&rec);
    rec.w[0] = GFX_OP_FILL
             | ((uint32_t)eq << 8)
             | ((uint32_t)dst << 20)
             | ((uint32_t)c << 24);
    rec.w[2] = pack_pair((int)r.x0, (int)r.y0);
    rec.w[3] = pack_pair(w, h);
    rec.w[4] = argb;
    return append(l, &rec);
}

int gfx_blit(gfx_list *l, int src, int sx, int sy, int dst, int dx, int dy,
             int w, int h, int eq, uint8_t c, int key_en, uint32_t key)
{
    gfx_cmd rec;

    if (l == 0 || l->t == 0)
        return GFX_BUILD_BAD_ARG;
    if (!surf_id_ok(src) || !surf_id_ok(dst) || eq < 0 || eq > 15)
        return reject(l, GFX_BUILD_BAD_ARG);
    if (!coord_ok(sx) || !coord_ok(sy) || !coord_ok(dx) || !coord_ok(dy))
        return reject(l, GFX_BUILD_BAD_ARG);
    if (!size_ok(w) || !size_ok(h))
        return reject(l, GFX_BUILD_BAD_ARG);
    if (key_en != 0 && key_en != 1)
        return reject(l, GFX_BUILD_BAD_ARG);

    zero_cmd(&rec);
    rec.w[0] = GFX_OP_BLIT
             | ((uint32_t)eq << 8)
             | ((uint32_t)(key_en != 0) << 12)
             | ((uint32_t)src << 16)
             | ((uint32_t)dst << 20)
             | ((uint32_t)c << 24);
    rec.w[1] = pack_pair(sx, sy);
    rec.w[2] = pack_pair(dx, dy);
    rec.w[3] = pack_pair(w, h);
    rec.w[5] = (key_en != 0) ? key : 0u;
    return append(l, &rec);
}

int gfx_set_clip(gfx_list *l, gfx_rect r)
{
    gfx_cmd rec;
    int     w, h;

    if (l == 0 || l->t == 0)
        return GFX_BUILD_BAD_ARG;
    if (!coord_ok((int)r.x0) || !coord_ok((int)r.y0))
        return reject(l, GFX_BUILD_BAD_ARG);

    w = (r.x1 < r.x0) ? 0 : (int)(r.x1 - r.x0 + 1);
    h = (r.y1 < r.y0) ? 0 : (int)(r.y1 - r.y0 + 1);
    if (!size_ok(w) || !size_ok(h))
        return reject(l, GFX_BUILD_BAD_ARG);

    zero_cmd(&rec);
    rec.w[0] = GFX_OP_SET_CLIP;
    rec.w[1] = pack_pair((int)r.x0, (int)r.y0);
    rec.w[3] = pack_pair(w, h);
    return append(l, &rec);
}

int gfx_list_end(gfx_list *l)
{
    gfx_cmd rec;
    int     code;

    if (l == 0 || l->t == 0)
        return GFX_BUILD_BAD_ARG;

    zero_cmd(&rec);
    rec.w[0] = GFX_OP_END;

    code = append(l, &rec);
    if (code == GFX_OK)
        l->ended = 1;
    return code;
}
