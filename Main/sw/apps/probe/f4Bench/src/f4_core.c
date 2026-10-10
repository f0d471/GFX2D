/* F4 研究原型核心实现；契约说明见 f4_core.h。
 * 热循环按 RV32 成本取舍：共享状态缓存在 builder，arena 按整字存储，
 * instance_count 延迟到封包回填一次；语义与 f4_expand 的检查严格对齐。 */
#include "f4_core.h"
#include <string.h>

/* 坐标必须可无损转成 int16；靠截断制造可提交的错误坐标在本契约里是构造失败。
   单分支形式：v+32768 折回无符号后越界值必然大于 65535。 */
static int coord_i16(int32_t v) { return ((uint32_t)v + 32768u) <= 65535u; }

void f4_init(f4_builder *b, gfx_cmd *stream, uint32_t cap,
             uint8_t *arena, uint32_t arena_bytes)
{
    b->stream = stream;
    b->cap = cap;
    b->arena = arena;
    b->arena_bytes = arena_bytes;
    f4_reset(b);
}

void f4_reset(f4_builder *b)
{
    b->used = 0;
    b->arena_used = 0;
    b->epoch = 0;
    b->open = F4_NO_OPEN;
    b->open_epoch = 0;
    b->count = 0;
    b->packets4 = 0;
    b->packets8 = 0;
    b->inst4 = 0;
    b->inst8 = 0;
    b->split_limit = 0;
    b->to_xyuv8 = 0;
}

static void f4_seal(f4_builder *b)
{
    if (b->open == F4_NO_OPEN)
        return;
    b->stream[b->open].w[2] = b->count;    /* instance_count 封包时回填一次 */
    /* 数组的末拍在已预留的 16 B padding 内，续包基址也保持 16 B 对齐。 */
    while (b->arena_used & 15u) {
        *(uint32_t *)(void *)(b->arena+b->arena_used)=0u;
        b->arena_used+=4u;
    }
    if (b->layout == F4_LAYOUT_XY4)
        b->packets4++;
    else
        b->packets8++;
    b->open = F4_NO_OPEN;
    b->count = 0;
}

/* 新开一个实例包；共享字段由调用方展开传入（续包时原 BLIT 已不存在）。 */
static int f4_open(f4_builder *b, uint32_t w0_nonop, uint32_t key, uint32_t wh,
                   uint32_t layout, int32_t fx, int32_t fy)
{
    gfx_cmd *p;
    if (b->used >= b->cap)
        return -1;
    p = &b->stream[b->used];
    p->w[0] = (w0_nonop & ~0xFFu) | F4_OP_BLIT_INST;
    p->w[1] = b->arena_used;  /* instances_base：arena 内字节偏移 */
    p->w[3] = wh;             /* width/height 共享 */
    p->w[4] = key;            /* key 值从旧 BLIT 的 w5 移到 w4 */
    p->w[5] = (layout == F4_LAYOUT_XY4 ? 4u : 8u) | (layout << 16);
    p->w[6] = (layout == F4_LAYOUT_XY4) ? f4_pack_pair(fx, fy) : 0;
    p->w[7] = b->used;        /* packet_tag：包的零基索引 */
    b->open = b->used++;
    b->open_epoch = b->epoch;
    b->count = 0;
    b->layout = layout;
    b->fx = fx;
    b->fy = fy;
    b->cur_w0 = p->w[0];
    b->cur_key = key;
    b->cur_wh = wh;
    return 0;
}

int f4_append_inst(f4_builder *b, uint32_t w0_nonop, uint32_t key, uint32_t wh,
                   int32_t dx, int32_t dy, int32_t sx, int32_t sy)
{
    uint32_t e0 = f4_pack_pair(dx, dy);
    uint32_t entry_bytes = (b->layout == F4_LAYOUT_XY4) ? 4u : 8u;
    if (!coord_i16(dx) || !coord_i16(dy) || !coord_i16(sx) || !coord_i16(sy))
        return GFX_BUILD_BAD_ARG;
    if (b->count >= F4_INST_MAX) {        /* 上界拆包，包间顺序即提交顺序 */
        uint32_t ow0 = b->cur_w0;
        uint32_t okey = b->cur_key;
        uint32_t owh = b->cur_wh;
        uint32_t lay = b->layout;
        int32_t fx = b->fx, fy = b->fy;
        f4_seal(b);
        b->split_limit++;
        if (f4_open(b, ow0, okey, owh, lay, fx, fy) != 0)
            return GFX_BUILD_FULL;
    }
    if (b->open == F4_NO_OPEN) {
        if (f4_open(b, w0_nonop, key, wh, F4_LAYOUT_XY4, sx, sy) != 0)
            return GFX_BUILD_FULL;
    } else if (b->layout == F4_LAYOUT_XY4 && (sx != b->fx || sy != b->fy)) {
        /* XY4 包遇到变化的源坐标：封包另开 XYUV8，共享状态不变 */
        f4_seal(b);
        if (f4_open(b, w0_nonop, key, wh, F4_LAYOUT_XYUV8, 0, 0) != 0)
            return GFX_BUILD_FULL;
        b->to_xyuv8++;
    }
    entry_bytes=(b->layout==F4_LAYOUT_XY4)?4u:8u;
    if ((((uint64_t)b->arena_used + entry_bytes + 15u)&~(uint64_t)15u) > b->arena_bytes)
        return GFX_BUILD_FULL;
    /* stride 与 entry_bytes 都是 4 的倍数，合法布局下 arena 内偏移恒 4B 对齐；
       展开器与硬件取数按同一布局读。 */
    *(uint32_t *)(void *)(b->arena + b->arena_used) = e0;
    b->arena_used += 4;
    if (b->layout == F4_LAYOUT_XYUV8) {
        *(uint32_t *)(void *)(b->arena + b->arena_used) = f4_pack_pair(sx, sy);
        b->arena_used += 4;
    }
    b->count++;
    if (b->layout == F4_LAYOUT_XY4)
        b->inst4++;
    else
        b->inst8++;
    return GFX_OK;
}

void f4_seal_pub(f4_builder *b)
{
    f4_seal(b);
}

int f4_append_batch(f4_builder *b, uint32_t w0_nonop, uint32_t key, uint32_t wh,
                    uint32_t layout, int32_t fixed_sx, int32_t fixed_sy,
                    const f4_pos *pos, uint32_t n)
{
    uint32_t i;
    uint32_t entry_bytes = (layout == F4_LAYOUT_XY4) ? 4u : 8u;
    if (layout > F4_LAYOUT_XYUV8)
        return GFX_BUILD_BAD_ARG;
    if (layout == F4_LAYOUT_XYUV8 && (fixed_sx != 0 || fixed_sy != 0))
        return GFX_BUILD_BAD_ARG;
    if (!coord_i16(fixed_sx) || !coord_i16(fixed_sy) || (n && !pos))
        return GFX_BUILD_BAD_ARG;
    if (n == 0) {
        f4_seal(b);
        if (f4_open(b,w0_nonop,key,wh,layout,fixed_sx,fixed_sy))return GFX_BUILD_FULL;
        f4_seal(b);return GFX_OK;
    }
    if ((((uint64_t)b->arena_used+(uint64_t)entry_bytes*n+15u)&~(uint64_t)15u)>b->arena_bytes)
        return GFX_BUILD_FULL;
    if (b->open == F4_NO_OPEN
        || b->cur_w0 != ((w0_nonop & ~0xFFu) | F4_OP_BLIT_INST)
        || b->cur_key != key || b->cur_wh != wh || b->open_epoch != b->epoch
        || b->layout != layout
        || (layout == F4_LAYOUT_XY4 && (fixed_sx != b->fx || fixed_sy != b->fy))) {
        f4_seal(b);
        if (f4_open(b, w0_nonop, key, wh, layout, fixed_sx, fixed_sy) != 0)
            return GFX_BUILD_FULL;
    }
    for (i = 0; i < n; i++) {
        const f4_pos *p = &pos[i];
        if (b->count >= F4_INST_MAX) {
            f4_seal(b);
            b->split_limit++;
            if (f4_open(b, w0_nonop, key, wh, layout, fixed_sx, fixed_sy) != 0)
                return GFX_BUILD_FULL;
        }
        if ((((uint64_t)b->arena_used+entry_bytes+15u)&~(uint64_t)15u)>b->arena_bytes)
            return GFX_BUILD_FULL;
        if (!coord_i16(p->dx) || !coord_i16(p->dy))
            return GFX_BUILD_BAD_ARG;
        if (layout == F4_LAYOUT_XY4) {
            if (p->sx != fixed_sx || p->sy != fixed_sy)
                return GFX_BUILD_BAD_ARG;
            *(uint32_t *)(void *)(b->arena + b->arena_used) = f4_pack_pair(p->dx, p->dy);
            b->arena_used += 4;
        } else {
            if (!coord_i16(p->sx) || !coord_i16(p->sy))
                return GFX_BUILD_BAD_ARG;
            *(uint32_t *)(void *)(b->arena + b->arena_used) = f4_pack_pair(p->dx, p->dy);
            b->arena_used += 4;
            *(uint32_t *)(void *)(b->arena + b->arena_used) = f4_pack_pair(p->sx, p->sy);
            b->arena_used += 4;
        }
        b->count++;
        if (layout == F4_LAYOUT_XY4)
            b->inst4++;
        else
            b->inst8++;
    }
    return GFX_OK;
}

int f4_build(f4_builder *b, const gfx_cmd *in, uint32_t n)
{
    uint32_t i;
    f4_reset(b);
    for (i = 0; i < n; i++) {
        const gfx_cmd *c = &in[i];
        uint32_t op = gfx_op(c);
        if (op != GFX_OP_BLIT) {
            f4_seal(b);
            if (op == GFX_OP_SET_CLIP)
                b->epoch++;
            if (b->used >= b->cap)
                return GFX_BUILD_FULL;
            b->stream[b->used++] = *c;
            continue;
        }
        {
            int32_t sx = gfx_s16(c->w[1]),        sy = gfx_s16(c->w[1] >> 16);
            int32_t dx = gfx_s16(c->w[2]),        dy = gfx_s16(c->w[2] >> 16);
            int same_state;
            same_state = (b->open != F4_NO_OPEN)
                         && b->open_epoch == b->epoch
                         && b->cur_w0 == ((c->w[0] & ~0xFFu) | F4_OP_BLIT_INST)
                         && b->cur_key == c->w[5]
                         && b->cur_wh == c->w[3];
            if (!same_state) {
                f4_seal(b);
                if (f4_open(b, c->w[0], c->w[5], c->w[3], F4_LAYOUT_XY4, sx, sy) != 0)
                    return GFX_BUILD_FULL;
            } else if (b->layout == F4_LAYOUT_XY4 && (sx != b->fx || sy != b->fy)) {
                f4_seal(b);
                if (f4_open(b, c->w[0], c->w[5], c->w[3], F4_LAYOUT_XYUV8, 0, 0) != 0)
                    return GFX_BUILD_FULL;
                b->to_xyuv8++;
            }
            if (f4_append_inst(b, c->w[0], c->w[5], c->w[3], dx, dy, sx, sy) != GFX_OK)
                return GFX_BUILD_BAD_ARG;
        }
    }
    f4_seal(b);
    return GFX_OK;
}

long f4_expand(const gfx_cmd *stream, uint32_t n_stream,
               const uint8_t *arena, uint32_t arena_bytes,
               gfx_cmd *out, uint32_t out_cap, f4_id *ids)
{
    uint32_t i, used = 0;
    for (i = 0; i < n_stream; i++) {
        const gfx_cmd *p = &stream[i];
        if (gfx_op(p) != F4_OP_BLIT_INST) {
            if (used >= out_cap)
                return -1;
            out[used] = *p;
            if (ids)
                ids[used] = (f4_id){ i, UINT32_MAX };
            used++;
            continue;
        }
        {
            uint32_t layout = (p->w[5] >> 16) & 3u;
            uint32_t stride = p->w[5] & 0xFFFFu;
            uint32_t count = p->w[2];
            uint32_t base = p->w[1];
            uint32_t entry = (layout == F4_LAYOUT_XY4) ? 4u : 8u;
            uint32_t k;
            int32_t fsx = 0, fsy = 0;
            if (layout > 1u || (stride & 3u) != 0 || stride < entry || stride > 256u)
                return -2;
            if ((base & 15u) || count > F4_INST_MAX)
                return -2;
            if (layout == F4_LAYOUT_XY4) {
                fsx = gfx_s16(p->w[6]);
                fsy = gfx_s16(p->w[6] >> 16);
            } else if (p->w[6] != 0) {
                return -2;    /* XYUV8 的 fixed 字段必须为 0 */
            }
            /* 数组长度按 64 位口径，基址加长度不得超出 arena */
            if (count != 0) {
                uint64_t bytes = (uint64_t)(count - 1) * stride + entry;
                if (base > arena_bytes || ((bytes+15u)&~(uint64_t)15u) > arena_bytes - base)
                    return -3;
            }
            for (k = 0; k < count; k++) {
                const uint8_t *e = arena + base + (uint32_t)k * stride;
                uint32_t w0xy, w1sxy = 0;
                int32_t sx, sy, dx, dy;
                gfx_cmd *o;
                memcpy(&w0xy, e, 4);
                if (layout == F4_LAYOUT_XYUV8)
                    memcpy(&w1sxy, e + 4, 4);
                sx = (layout == F4_LAYOUT_XY4) ? fsx : gfx_s16(w1sxy);
                sy = (layout == F4_LAYOUT_XY4) ? fsy : gfx_s16(w1sxy >> 16);
                dx = gfx_s16(w0xy);
                dy = gfx_s16(w0xy >> 16);
                if (used >= out_cap)
                    return -1;
                o = &out[used];
                o->w[0] = (p->w[0] & ~0xFFu) | GFX_OP_BLIT;
                o->w[1] = f4_pack_pair(sx, sy);
                o->w[2] = f4_pack_pair(dx, dy);
                o->w[3] = p->w[3];
                o->w[4] = 0;
                o->w[5] = p->w[4];
                o->w[6] = 0;
                o->w[7] = used;           /* fallback 表自己的零基索引 */
                if (ids)
                    ids[used] = (f4_id){ i, k };
                used++;
            }
        }
    }
    return (long)used;
}
