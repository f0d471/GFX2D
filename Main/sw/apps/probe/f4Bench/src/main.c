/* F4 研究板测：同一冻结输入流上对比三条构造路径的每帧 CPU 周期。
 * base=生产 gfx_blit_run 分段重放（模板批量接口的最佳用法）；
 * inst=f4_build 整流重放（含逐记录分派，实例 API 的上界口径）；
 * instb=f4_append_batch 批入口重放（共享检查段级一次，
 * 对应 spec 里 gfx_instances_append 的生产形态）。
 * 对拍（构造+展开逐字比较）先行，计时内只做构造，CRC 消费在计时外；
 * 不执行渲染，不上引擎。 */
#include "board.h"
#include "gfx/gfx.h"
#include "core/crc32.h"
#include "core/format.h"
#include "f4_core.h"
#include "f4_scenes.h"
#include <string.h>

#ifndef F4_ITERS
#define F4_ITERS 300u
#endif
#ifndef F4_WARMUP
#define F4_WARMUP 8u
#endif

#define STREAM_CAP 8192u
#define ARENA_BYTES (512u * 1024u)
#define LIST_CAP   4096u
#define POS_CAP    4096u
#define SEG_CAP    128u
#define ROUNDS     3u

static f4_builder builder;
static gfx_cmd stream[STREAM_CAP];
static uint8_t arena[ARENA_BYTES] __attribute__((aligned(16)));
static gfx_cmd expand[LIST_CAP];
static gfx_cmd listbuf[LIST_CAP];
static gfx_blit_pos pos[POS_CAP];
static gfx_surface_table tab;

typedef struct {
    uint32_t w0, key, wh;
    uint32_t first, count, epoch;
    uint32_t layout;      /* 段内源坐标全同=XY4，否则 XYUV8 */
    int32_t fx, fy;
} f4_seg;
static f4_seg segs[SEG_CAP];

static uint64_t base_sum[ROUNDS], inst_sum[ROUNDS], instb_sum[ROUNDS];
static uint32_t base_min[ROUNDS], base_max[ROUNDS];
static uint32_t inst_min[ROUNDS], inst_max[ROUNDS];
static uint32_t instb_min[ROUNDS], instb_max[ROUNDS];
static uint32_t crcs[ROUNDS * 3u];

static void number(uint32_t v)
{
    char b[16];
    core_out o;
    core_out_init(&o, b, sizeof b);
    core_out_u32(&o, v);
    board_puts(b);
}

static void field(const char *k, uint32_t v)
{
    board_puts(k);
    number(v);
}

/* 基线段扫描：与实例包同状态键（w0 全部、key、宽高、裁剪 epoch），
   独立实现，供 gfx_blit_run 分段重放与批实例入口共用。 */
static uint32_t scan_segments(const gfx_cmd *in, uint32_t n, gfx_blit_pos *out)
{
    uint32_t i, nseg = 0, npos = 0, epoch = 0;
    memset(segs, 0, sizeof segs);
    for (i = 0; i < n; i++) {
        const gfx_cmd *c = &in[i];
        uint32_t op = gfx_op(c);
        if (op == GFX_OP_SET_CLIP) {
            epoch++;
            continue;
        }
        if (op != GFX_OP_BLIT)
            continue;
        if (npos >= POS_CAP || nseg >= SEG_CAP)
            break;
        if (nseg == 0 || segs[nseg - 1].w0 != c->w[0] ||
            segs[nseg - 1].key != c->w[5] || segs[nseg - 1].wh != c->w[3] ||
            segs[nseg - 1].epoch != epoch) {
            if (nseg < SEG_CAP) {
                segs[nseg].w0 = c->w[0];
                segs[nseg].key = c->w[5];
                segs[nseg].wh = c->w[3];
                segs[nseg].epoch = epoch;
                segs[nseg].first = npos;
                segs[nseg].count = 0;
                nseg++;
            }
        }
        out[npos].sx = gfx_s16(c->w[1]);
        out[npos].sy = gfx_s16(c->w[1] >> 16);
        out[npos].dx = gfx_s16(c->w[2]);
        out[npos].dy = gfx_s16(c->w[2] >> 16);
        npos++;
        if (nseg > 0)
            segs[nseg - 1].count++;
    }
    /* 段内源坐标全同的段用 XY4，否则整段 XYUV8；fixed 取段首源 */
    for (i = 0; i < nseg; i++) {
        uint32_t j;
        int same = 1;
        for (j = 1; j < segs[i].count; j++) {
            if (pos[segs[i].first + j].sx != pos[segs[i].first].sx ||
                pos[segs[i].first + j].sy != pos[segs[i].first].sy) {
                same = 0;
                break;
            }
        }
        segs[i].layout = same ? F4_LAYOUT_XY4 : F4_LAYOUT_XYUV8;
        segs[i].fx = same ? pos[segs[i].first].sx : 0;
        segs[i].fy = same ? pos[segs[i].first].sy : 0;
    }
    return nseg;
}

/* 构造+展开对拍：与宿主 f4_inst_proto 的 diff_scene 同判据（无文件 IO）。 */
static int verify_scene(f4_builder *b, const gfx_cmd *in, uint32_t n)
{
    uint32_t i, k = 0;
    long got;
    memset(b->arena, 0xEE, b->arena_bytes);
    if (f4_build(b, in, n) != GFX_OK)
        return -1;
    got = f4_expand(b->stream, b->used, b->arena, b->arena_used, expand, LIST_CAP, NULL);
    if (got < 0)
        return -2;
    if ((uint32_t)got != n)
        return -3;
    for (i = 0; i < n; i++) {
        const gfx_cmd *o = &expand[i];
        if (gfx_op(&in[i]) == GFX_OP_BLIT) {
            if (o->w[0] != in[i].w[0] || o->w[1] != in[i].w[1] ||
                o->w[2] != in[i].w[2] || o->w[3] != in[i].w[3] ||
                o->w[5] != in[i].w[5] || o->w[4] != 0)
                return -4;
            if (in[i].w[7] != i)
                return -5;
            k++;
        } else if (memcmp(o, &in[i], sizeof(gfx_cmd)) != 0) {
            return -6;
        }
    }
    return (int)k;
}

/* 批入口路径的同判据对拍：批重放只含 BLIT 段（renderer 直调实例 API 的
   形态，FILL/SET_CLIP 由适配层另行提交，两条计时路径同样都不含它们），
   因此对拍目标是原流里按序的 BLIT 语义字段。 */
static int verify_batch(f4_builder *b, const f4_seg *s, uint32_t nseg,
                        const gfx_cmd *in, uint32_t n)
{
    uint32_t i, k = 0;
    long got;
    memset(b->arena, 0xEE, b->arena_bytes);
    f4_reset(b);
    for (i = 0; i < nseg; i++) {
        int rc = f4_append_batch(b, s[i].w0, s[i].key, s[i].wh, s[i].layout,
                                 s[i].fx, s[i].fy,
                                 (const f4_pos *)&pos[s[i].first], s[i].count);
        if (rc != GFX_OK)
            return -1;
    }
    f4_seal_pub(b);
    got = f4_expand(b->stream, b->used, b->arena, b->arena_used, expand, LIST_CAP, NULL);
    if (got < 0)
        return -2;
    for (i = 0; i < n; i++) {
        const gfx_cmd *o;
        if (gfx_op(&in[i]) != GFX_OP_BLIT)
            continue;
        if (k >= (uint32_t)got)
            return -3;    /* 展开条数少于原流 BLIT 数 */
        o = &expand[k];
        if (o->w[0] != in[i].w[0] || o->w[1] != in[i].w[1] ||
            o->w[2] != in[i].w[2] || o->w[3] != in[i].w[3] ||
            o->w[5] != in[i].w[5] || o->w[4] != 0)
            return -4;
        k++;
    }
    if ((uint32_t)got != k)
        return -5;        /* 展开条数多于原流 BLIT 数 */
    return (int)k;
}

static uint64_t run_baseline(const f4_seg *s, uint32_t nseg,
                             const gfx_blit_pos *p, uint32_t round)
{
    gfx_list l;
    uint64_t sum = 0;
    uint32_t lo = 0xFFFFFFFFu, hi = 0, it;
    uint32_t i;
    for (it = 0; it < F4_WARMUP + F4_ITERS; it++) {
        uint32_t t0, cyc;
        uint32_t used;
        gfx_list_begin(&l, listbuf, LIST_CAP, &tab);
        t0 = board_cycles();
        for (i = 0; i < nseg; i++) {
            gfx_blit_tmpl t;
            t.src = (s[i].w0 >> 16) & 0xFu;
            t.dst = (s[i].w0 >> 20) & 0xFu;
            t.w = gfx_u16(s[i].wh);
            t.h = gfx_u16(s[i].wh >> 16);
            t.eq = (int)((s[i].w0 >> 8) & 0xFu);
            t.c = (uint8_t)(s[i].w0 >> 24);
            t.key_en = (int)((s[i].w0 >> 12) & 1u);
            t.key = s[i].key;
            (void)gfx_blit_run(&l, &t, &p[s[i].first], s[i].count);
        }
        cyc = board_cycles() - t0;
        used = l.used;
        if (it >= F4_WARMUP) {
            sum += cyc;
            if (cyc < lo)
                lo = cyc;
            if (cyc > hi)
                hi = cyc;
            crcs[round * 3u] ^= crc32_ieee(listbuf, 32u * used);
        }
    }
    base_min[round] = lo;
    base_max[round] = hi;
    return sum;
}

static uint64_t run_inst(f4_builder *b, const gfx_cmd *in, uint32_t n, uint32_t round)
{
    uint64_t sum = 0;
    uint32_t lo = 0xFFFFFFFFu, hi = 0, it;
    for (it = 0; it < F4_WARMUP + F4_ITERS; it++) {
        uint32_t t0, cyc;
        gfx_list l;
        gfx_list_begin(&l, listbuf, LIST_CAP, &tab);   /* 与基线同环境 */
        t0 = board_cycles();
        (void)f4_build(b, in, n);
        cyc = board_cycles() - t0;
        if (it >= F4_WARMUP) {
            sum += cyc;
            if (cyc < lo)
                lo = cyc;
            if (cyc > hi)
                hi = cyc;
            crcs[round * 3u + 1u] ^= crc32_ieee(b->stream, 32u * b->used)
                                   ^ crc32_ieee(b->arena, b->arena_used);
        }
    }
    inst_min[round] = lo;
    inst_max[round] = hi;
    return sum;
}

static uint64_t run_batch(f4_builder *b, const f4_seg *s, uint32_t nseg, uint32_t round)
{
    uint64_t sum = 0;
    uint32_t lo = 0xFFFFFFFFu, hi = 0, it;
    uint32_t i;
    for (it = 0; it < F4_WARMUP + F4_ITERS; it++) {
        uint32_t t0, cyc;
        gfx_list l;
        gfx_list_begin(&l, listbuf, LIST_CAP, &tab);
        t0 = board_cycles();
        f4_reset(b);
        for (i = 0; i < nseg; i++) {
            (void)f4_append_batch(b, s[i].w0, s[i].key, s[i].wh, s[i].layout,
                                  s[i].fx, s[i].fy,
                                  (const f4_pos *)&pos[s[i].first], s[i].count);
        }
        f4_seal_pub(b);
        cyc = board_cycles() - t0;
        if (it >= F4_WARMUP) {
            sum += cyc;
            if (cyc < lo)
                lo = cyc;
            if (cyc > hi)
                hi = cyc;
            crcs[round * 3u + 2u] ^= crc32_ieee(b->stream, 32u * b->used)
                                   ^ crc32_ieee(b->arena, b->arena_used);
        }
    }
    instb_min[round] = lo;
    instb_max[round] = hi;
    return sum;
}

int main(void)
{
    uint32_t s,failed=0;
    crc32_init();
    board_puts("f4Bench start\n");
    for (s = 0; s < f4_scene_count; s++) {
        const uint8_t *data = f4_scene_table[s].data;
        uint32_t n = data[192] | (data[193] << 8) | (data[194] << 16) | (data[195] << 24);
        const gfx_cmd *in = (const gfx_cmd *)(data + 196);
        uint32_t nseg, round;
        int vk, vb;
        memcpy(&tab, data, sizeof tab);
        memset(&builder, 0, sizeof builder);
        builder.stream = stream;
        builder.cap = STREAM_CAP;
        builder.arena = arena;
        builder.arena_bytes = ARENA_BYTES;

        vk = verify_scene(&builder, in, n);
        nseg = scan_segments(in, n, pos);
        vb = verify_batch(&builder, segs, nseg, in, n);
        board_puts("scene=");
        board_puts(f4_scene_table[s].name);
        field(" n=", n);
        field(" segs=", nseg);
        field(" verify=", (uint32_t)vk);
        field(" vbatch=", (uint32_t)vb);
        board_puts("\n");
        if (vk < 0 || vb < 0) {failed++;break;}

        for (round = 0; round < ROUNDS; round++) {
            base_sum[round] = run_baseline(segs, nseg, pos, round);
            inst_sum[round] = run_inst(&builder, in, n, round);
            instb_sum[round] = run_batch(&builder, segs, nseg, round);
        }
        for (round = 0; round < ROUNDS; round++) {
            board_puts("round");
            number(round);
            field(" base=", (uint32_t)(base_sum[round] / F4_ITERS));
            field(" inst=", (uint32_t)(inst_sum[round] / F4_ITERS));
            field(" instb=", (uint32_t)(instb_sum[round] / F4_ITERS));
            board_puts("\n");
        }
    }
    board_puts("crc");
    for (s = 0; s < ROUNDS * 3u; s++)
        field("=", crcs[s]);
    board_puts("\n");field("fail ",failed);board_puts("\ndone\n");
    return (int)failed;
}
