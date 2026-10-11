/*=== F5 研究轮宿主对拍原型：PB v1 构建 + 独立展开 + 像素级对拍 + 变异判据
 *
 * 输入形态与冻结输入一致：v1 标量命令流加 F4 v1-I 实例包（数组在 arena）。
 * pb_build 按条目 2.3 的两遍构建（COUNT/SNAPSHOT、PREFIX、SCATTER）生成
 * PB v1 内存（512 B header + 64 B metadata 槽 + 16 B 对齐的 8 B 引用区），
 * clip 快照保存在 metadata，绝不依赖末次 SET_CLIP 追溯。
 * pb_exec 从 PB 内存独立解析（目录→引用→metadata→实例），按快照 clip
 * 吸收坐标后交给生产 exec_sw 执行；直通侧把同一输入流按序展开执行。
 * 判据：两份帧缓冲逐字节一致；SCATTER 写出数与 COUNT 计数对账。
 * 变异判据（每类在随机场景上必须至少一张见红，否则判据无效）：
 *   clip_last   末次 clip 覆盖全表（metadata 快照被替换成最终 clip）
 *   ref_reverse 桶内引用倒序（透明交叠场景像素次序错）
 *   skip_inst   第二次遍历跳过实例引用（SCATTER 对账见红）
 *   inst_scalar 引用 instance_index 置 0xFFFFFFFF（实例变标量身份，坐标错）
 *   ready_early READY 提前（末桶少写一条引用，展开侧读到全零引用见红）
 *   stale_pb    双帧复用 PB 不重建目录（目录残留上一帧，展开错位见红）
 * 编译（WSL，从仓库根）：
 *   gcc -std=c11 -I Main/sw/lib -fsanitize=address,undefined -g \
 *       Main/sim/engine/frontend/f5_pb_fuzz.c \
 *       Main/sw/lib/gfx/*.c \
 *       Main/sw/lib/core/format.c -o Main/sim/engine/frontend/build/f5_pb_fuzz
 *   Main/sim/engine/frontend/build/f5_pb_fuzz [场景数]
 *===*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gfx/gfx.h"

#define SCREEN_W   640
#define SCREEN_H   480
#define STRIDE     4096u
#define STRIP_H    16
#define LSTRIPS    (SCREEN_H / STRIP_H)
#define PB_MAGIC   0x31425046u
#define META_W     64u
#define DRAW_CAP   4096u
#define REF_CAP    65536u
#define PB_BYTES   (512u + DRAW_CAP * META_W + 8u * REF_CAP + 16u * LSTRIPS)

#define MAX_PKGS   128u
#define MAX_ARRAY  262144u

static uint32_t rng_state = 0x5eed1234u;
static uint32_t rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}
static uint32_t rnd_range(uint32_t n) { return n ? rnd() % n : 0u; }

static int32_t si16(uint32_t v)
{
    v &= 0xFFFFu;
    return (v < 0x8000u) ? (int32_t)v : (int32_t)v - 0x10000;
}
static uint32_t pi16(int32_t v) { return (uint32_t)(uint16_t)(int16_t)v; }

/* exec_sw 的窗口检查是单一窗口语义：所有表面 base 必须落在同一块
   连续设备窗口内（板上恒等映射整块 DDR 的同构）。直通与 PB 各用一份
   帧缓冲区，源图集共享，全放同一 pool。 */
#define FB_BYTES   (SCREEN_H * STRIDE)
#define SRC_OFF    (2u * FB_BYTES)
static uint8_t pool[SRC_OFF + 3u * 4096u];
static gfx_surface_table st_d;          /* 直通目的 base=0 */
static gfx_surface_table st_p;          /* PB 目的 base=FB_BYTES */
static gfx_mem  dev;                    /* 单一设备窗口 */
static uint8_t  arena[MAX_ARRAY];
static uint32_t arena_used;

static void setup_surfaces(void)
{
    memset(&st_d, 0, sizeof st_d);
    memset(&st_p, 0, sizeof st_p);
    memset(&dev, 0, sizeof dev);
    st_d.s[0].base = 0u;
    st_p.s[0].base = FB_BYTES;
    st_d.s[0].dim = st_p.s[0].dim = SCREEN_W | (SCREEN_H << 16);
    st_d.s[0].ctrl = st_p.s[0].ctrl =
        12u | (GFX_FMT_XRGB8888 << 12) | 0x80000000u;
    for (uint32_t i = 0; i < 3; i++) {
        uint32_t w = 8u << i, h = 8u << i;
        uint32_t base = SRC_OFF + i * 4096u;
        st_d.s[1 + i].base = st_p.s[1 + i].base = base;
        st_d.s[1 + i].dim = st_p.s[1 + i].dim = w | (h << 16);
        st_d.s[1 + i].ctrl = st_p.s[1 + i].ctrl =
            (5u + i) | (GFX_FMT_XRGB8888 << 12) | 0x80000000u;
        for (uint32_t p = 0; p < w * h; p++)
            ((uint32_t *)(void *)(pool + base))[p] = rnd() | 0xFF000000u;
    }
    dev.dev_base = 0u;
    dev.host = pool;
    dev.bytes = sizeof pool;
}

typedef struct {
    gfx_cmd  cmds[MAX_PKGS * 2 + 8];
    uint32_t n_cmds;
    uint32_t array_off[MAX_PKGS * 2 + 8];
} scene_t;

static void gen_scene(scene_t *sc, uint32_t seed)
{
    rng_state = seed * 2654435761u + 1u;
    memset(sc, 0, sizeof *sc);
    arena_used = 0;
    uint32_t n = 6u + rnd_range(28u);
    uint32_t mode = rnd_range(6u);
    for (uint32_t i = 0; i < n && sc->n_cmds < MAX_PKGS * 2 - 2u; i++) {
        gfx_cmd *c = &sc->cmds[sc->n_cmds];
        memset(c, 0, sizeof *c);
        uint32_t pick = rnd_range(100u);
        if (i == 0 || pick < 14u) {
            c->w[0] = GFX_OP_SET_CLIP;
            int32_t x0 = (int32_t)rnd_range(660u) - 10;
            int32_t y0 = (int32_t)rnd_range(500u) - 10;
            uint32_t w = rnd_range(660u), h = rnd_range(500u);
            if (mode == 4 && i > 0) { x0 = 0; y0 = 0; w = 0; h = 0; }
            c->w[1] = pi16(x0) | (pi16(y0) << 16);
            c->w[3] = w | (h << 16);
            c->w[7] = sc->n_cmds;
            sc->n_cmds++;
            continue;
        }
        if (pick < 20u) {
            c->w[0] = GFX_OP_NOP;
            c->w[7] = sc->n_cmds;
            sc->n_cmds++;
            continue;
        }
        if (pick < 55u) {
            uint32_t layout = rnd_range(2u);
            uint32_t count = 2u + rnd_range(60u);
            uint32_t entry = layout ? 8u : 4u;
            uint32_t w = 16u, h = 16u;
            if (mode == 2) { w = 32u; h = 8u; }
            uint32_t key_en = rnd_range(2u);
            uint32_t eq = rnd_range(6u);
            if (arena_used + count * entry + 16u > MAX_ARRAY) continue;
            uint32_t off = arena_used;
            /* 源固定用 32×32 的 s3，源坐标限 0..16：请求 w 最大 32，
               让实例像素大多真正可见，变异判据才有像素消费者 */
            int32_t sx0 = (int32_t)rnd_range(17u), sy0 = (int32_t)rnd_range(17u);
            for (uint32_t j = 0; j < count; j++) {
                int32_t dx = (int32_t)rnd_range(700u) - 40;
                int32_t dy = (mode == 5) ? 200 + (int32_t)(j * 3u % 48u)
                                         : (int32_t)rnd_range(520u) - 20;
                uint8_t *e = arena + off + j * entry;
                e[0] = (uint8_t)(pi16(dx) & 0xFF); e[1] = (uint8_t)(pi16(dx) >> 8);
                e[2] = (uint8_t)(pi16(dy) & 0xFF); e[3] = (uint8_t)(pi16(dy) >> 8);
                if (layout) {
                    int32_t sx = (int32_t)rnd_range(17u), sy = (int32_t)rnd_range(17u);
                    e[4] = (uint8_t)(pi16(sx) & 0xFF); e[5] = (uint8_t)(pi16(sx) >> 8);
                    e[6] = (uint8_t)(pi16(sy) & 0xFF); e[7] = (uint8_t)(pi16(sy) >> 8);
                }
            }
            arena_used = (arena_used + count * entry + 15u) & ~15u;
            c->w[0] = GFX_OP_BLIT_INST | (eq << 8) | (key_en << 12) | (3u << 16);
            c->w[1] = 0x30000000u + off;
            c->w[2] = count;
            c->w[3] = w | (h << 16);
            c->w[4] = key_en ? rnd() : 0u;
            c->w[5] = entry | (layout << 16);
            c->w[6] = layout ? 0u : (pi16(sx0) | (pi16(sy0) << 16));
            c->w[7] = sc->n_cmds;
            sc->array_off[sc->n_cmds] = off;
            sc->n_cmds++;
            continue;
        }
        uint32_t is_blit = rnd_range(2u);
        uint32_t srcid = 1u + rnd_range(3u);
        uint32_t w = 1u + rnd_range(96u), h = 1u + rnd_range(96u);
        uint32_t eq = rnd_range(6u);
        if (mode == 1) { w = 1u + rnd_range(8u); h = 320u + rnd_range(160u); }
        if (mode == 3) { w = 96u; h = 96u; }
        int32_t dx = (int32_t)rnd_range(720u) - 60;
        int32_t dy = (int32_t)rnd_range(540u) - 30;
        int32_t sx = (int32_t)rnd_range(40u) - 20;
        int32_t sy = (int32_t)rnd_range(40u) - 20;
        c->w[0] = (is_blit ? GFX_OP_BLIT : GFX_OP_FILL) | (eq << 8) |
                  ((is_blit ? rnd_range(2u) : 0u) << 12) |
                  ((is_blit ? srcid : 0u) << 16);
        if (is_blit)
            c->w[1] = pi16(sx) | (pi16(sy) << 16);
        c->w[2] = pi16(dx) | (pi16(dy) << 16);
        c->w[3] = w | (h << 16);
        if (eq == GFX_EQ_CONST) c->w[0] |= 0x2A000000u;
        else if (!is_blit && eq == GFX_EQ_ALPHA) c->w[4] = 0x80402010u;
        else if (!is_blit && rnd_range(2u)) c->w[4] = rnd();
        if (is_blit && (c->w[0] >> 12 & 1u)) c->w[5] = rnd();
        c->w[7] = sc->n_cmds;
        sc->n_cmds++;
    }
    gfx_cmd *end = &sc->cmds[sc->n_cmds];
    memset(end, 0, sizeof *end);
    end->w[0] = GFX_OP_END;
    end->w[7] = sc->n_cmds;
    sc->n_cmds++;
}

enum {
    INJ_NONE = 0, INJ_CLIP_LAST, INJ_REF_REVERSE, INJ_SKIP_INST,
    INJ_INST_SCALAR, INJ_READY_EARLY, INJ_STALE_PB
};

typedef struct {
    uint8_t  buf[PB_BYTES];
    uint32_t u_slots;
    uint32_t m_total;
    uint32_t counts[LSTRIPS];
    gfx_rect final_clip;
} pb_t;

static int rect_empty(gfx_rect r) { return r.x0 > r.x1 || r.y0 > r.y1; }

static int dest_window(const gfx_cmd *c, gfx_rect clip, gfx_rect *out)
{
    int32_t dx = si16(c->w[2]), dy = si16(c->w[2] >> 16);
    uint32_t w = c->w[3] & 0xFFFFu, h = (c->w[3] >> 16) & 0xFFFFu;
    if (!w || !h) return 0;
    gfx_rect r;
    r.x0 = dx > 0 ? dx : 0;           r.y0 = dy > 0 ? dy : 0;
    r.x1 = dx + (int32_t)w - 1;       r.y1 = dy + (int32_t)h - 1;
    if (r.x1 > SCREEN_W - 1) r.x1 = SCREEN_W - 1;
    if (r.y1 > SCREEN_H - 1) r.y1 = SCREEN_H - 1;
    if (r.x0 < clip.x0) r.x0 = clip.x0;
    if (r.y0 < clip.y0) r.y0 = clip.y0;
    if (r.x1 > clip.x1) r.x1 = clip.x1;
    if (r.y1 > clip.y1) r.y1 = clip.y1;
    uint32_t dst = (c->w[0] >> 20) & 15u;
    if (r.x1 >= (int32_t)(st_d.s[dst].dim & 65535u)) r.x1 = (int32_t)(st_d.s[dst].dim & 65535u) - 1;
    if (r.y1 >= (int32_t)(st_d.s[dst].dim >> 16)) r.y1 = (int32_t)(st_d.s[dst].dim >> 16) - 1;
    uint32_t op = c->w[0] & 255u;
    if (op == GFX_OP_BLIT || op == GFX_OP_BLIT_INST) {
        uint32_t src = (c->w[0] >> 16) & 15u;
        int32_t sx = si16(c->w[1]), sy = si16(c->w[1] >> 16);
        if (r.x0 < dx - sx) r.x0 = dx - sx;
        if (r.y0 < dy - sy) r.y0 = dy - sy;
        int32_t ex = dx - sx + (int32_t)(st_d.s[src].dim & 65535u) - 1;
        int32_t ey = dy - sy + (int32_t)(st_d.s[src].dim >> 16) - 1;
        if (r.x1 > ex) r.x1 = ex;
        if (r.y1 > ey) r.y1 = ey;
    }
    if (rect_empty(r)) return 0;
    *out = r;
    return 1;
}

static void meta_slot(pb_t *pb, uint32_t idx, const gfx_cmd *c, gfx_rect clip)
{
    uint8_t *m = pb->buf + 512u + (size_t)idx * META_W;
    memcpy(m, c, 32u);
    m[32] = (uint8_t)(pi16(clip.x0) & 0xFF); m[33] = (uint8_t)(pi16(clip.x0) >> 8);
    m[34] = (uint8_t)(pi16(clip.y0) & 0xFF); m[35] = (uint8_t)(pi16(clip.y0) >> 8);
    m[36] = (uint8_t)(pi16(clip.x1) & 0xFF); m[37] = (uint8_t)(pi16(clip.x1) >> 8);
    m[38] = (uint8_t)(pi16(clip.y1) & 0xFF); m[39] = (uint8_t)(pi16(clip.y1) >> 8);
    m[40] = (uint8_t)idx; m[41] = (uint8_t)(idx >> 8);
    m[42] = (uint8_t)(idx >> 16); m[43] = (uint8_t)(idx >> 24);
}

static uint8_t *ref_ptr(pb_t *pb, uint32_t off_bytes)
{
    return pb->buf + 512u + DRAW_CAP * META_W + off_bytes;
}

static void put_ref(pb_t *pb, uint32_t off_bytes, uint32_t pkt, uint32_t inst)
{
    uint8_t *r = ref_ptr(pb, off_bytes);
    r[0] = (uint8_t)pkt; r[1] = (uint8_t)(pkt >> 8);
    r[2] = (uint8_t)(pkt >> 16); r[3] = (uint8_t)(pkt >> 24);
    r[4] = (uint8_t)inst; r[5] = (uint8_t)(inst >> 8);
    r[6] = (uint8_t)(inst >> 16); r[7] = (uint8_t)(inst >> 24);
}

static void pkt_inst_pos(const scene_t *sc, const gfx_cmd *c, uint32_t pkg_idx,
                         uint32_t inst, int32_t *dx, int32_t *dy,
                         int32_t *sx, int32_t *sy)
{
    if ((c->w[0] & 0xFF) != GFX_OP_BLIT_INST) {
        *dx = si16(c->w[2]); *dy = si16(c->w[2] >> 16);
        *sx = si16(c->w[1]); *sy = si16(c->w[1] >> 16);
        return;
    }
    uint32_t stride = c->w[5] & 0xFFFFu;
    uint32_t layout = (c->w[5] >> 16) & 3u;
    const uint8_t *e = arena + sc->array_off[pkg_idx] + (size_t)inst * stride;
    *dx = si16((uint32_t)e[0] | ((uint32_t)e[1] << 8));
    *dy = si16((uint32_t)e[2] | ((uint32_t)e[3] << 8));
    if (!layout) {
        *sx = si16(c->w[6] & 0xFFFFu); *sy = si16(c->w[6] >> 16);
    } else {
        *sx = si16((uint32_t)e[4] | ((uint32_t)e[5] << 8));
        *sy = si16((uint32_t)e[6] | ((uint32_t)e[7] << 8));
    }
}

static void clip_apply(const gfx_cmd *c, gfx_rect *clip)
{
    clip->x0 = si16(c->w[1]); clip->y0 = si16(c->w[1] >> 16);
    clip->x1 = clip->x0 + (int32_t)(c->w[3] & 0xFFFFu) - 1;
    clip->y1 = clip->y0 + (int32_t)((c->w[3] >> 16) & 0xFFFFu) - 1;
    if (clip->x0 < 0) clip->x0 = 0;
    if (clip->y0 < 0) clip->y0 = 0;
    if (clip->x1 >= SCREEN_W) clip->x1 = SCREEN_W - 1;
    if (clip->y1 >= SCREEN_H) clip->y1 = SCREEN_H - 1;
    if (rect_empty(*clip)) *clip = (gfx_rect){ 0, 0, -1, -1 };
}

static uint32_t pb_build(pb_t *pb, const scene_t *sc, int inj)
{
    uint32_t i, s;
    if (inj != INJ_STALE_PB) memset(pb, 0, sizeof *pb);
    else {
        /* 双帧复用只残留目录：counts/u_slots/引用区全部重置，目录照旧 */
        memset(pb->buf + 512u, 0, PB_BYTES - 512u);
        memset(pb->counts, 0, sizeof pb->counts);
        pb->u_slots = 0;
    }
    gfx_rect clip = { 0, 0, SCREEN_W - 1, SCREEN_H - 1 };
    /* clip_last 变异注入的是"整个输入的末次 clip"：单遍 COUNT 里的运行值
       恰好等于当前 clip，没有区分度，必须先预扫描得到场景最终 clip。 */
    gfx_rect inj_clip = clip;
    if (inj == INJ_CLIP_LAST)
        for (i = 0; i < sc->n_cmds; i++)
            if ((sc->cmds[i].w[0] & 0xFF) == GFX_OP_SET_CLIP)
                clip_apply(&sc->cmds[i], &inj_clip);

    for (i = 0; i < sc->n_cmds; i++) {
        const gfx_cmd *c = &sc->cmds[i];
        uint32_t op = c->w[0] & 0xFFu;
        if (op == GFX_OP_END) break;
        uint32_t slot = pb->u_slots++;
        if (slot >= DRAW_CAP) return 0;
        if (op == GFX_OP_SET_CLIP) {
            clip_apply(c, &clip);
            meta_slot(pb, slot, c,
                      (inj == INJ_CLIP_LAST) ? inj_clip : clip);
            pb->final_clip = clip;
            continue;
        }
        if (op == GFX_OP_NOP) { meta_slot(pb, slot, c, clip); continue; }
        if (inj == INJ_CLIP_LAST) meta_slot(pb, slot, c, inj_clip);
        else meta_slot(pb, slot, c, clip);
        uint32_t n_inst = (op == GFX_OP_BLIT_INST) ? c->w[2] : 1u;
        for (uint32_t j = 0; j < n_inst; j++) {
            int32_t dx, dy, sx, sy;
            pkt_inst_pos(sc, c, i, j, &dx, &dy, &sx, &sy);
            gfx_cmd tmp = *c;
            tmp.w[2] = (uint32_t)pi16(dx) | ((uint32_t)pi16(dy) << 16);
            if (op == GFX_OP_BLIT || op == GFX_OP_BLIT_INST)
                tmp.w[1] = (uint32_t)pi16(sx) | ((uint32_t)pi16(sy) << 16);
            gfx_rect win;
            if (!dest_window(&tmp, clip, &win)) continue;
            uint32_t s0 = (uint32_t)win.y0 / STRIP_H;
            uint32_t s1 = (uint32_t)win.y1 / STRIP_H;
            for (s = s0; s <= s1 && s < LSTRIPS; s++) pb->counts[s]++;
        }
    }
    uint32_t off = 0, m = 0, dirs[32];
    for (s = 0; s < 32; s++) {
        if (s < LSTRIPS) {
            dirs[s] = off;
            off += (pb->counts[s] * 8u + 15u) & ~15u;
            m += pb->counts[s];
        } else dirs[s] = 0;
    }
    if (m > REF_CAP) return 0;
    uint32_t *h = (uint32_t *)(void *)pb->buf;
    h[0] = PB_MAGIC; h[1] = 1u; h[2] = 1u; h[3] = LSTRIPS;
    h[4] = pb->u_slots; h[5] = m;
    h[6] = 512u; h[7] = 512u + DRAW_CAP * META_W;
    if (inj != INJ_STALE_PB)
        for (s = 0; s < 32; s++) {
            h[16 + s * 2] = dirs[s];
            h[17 + s * 2] = s < LSTRIPS ? pb->counts[s] : 0u;
        }
    /* SCATTER */
    clip = (gfx_rect){ 0, 0, SCREEN_W - 1, SCREEN_H - 1 };
    uint32_t cursor[32];
    memset(cursor, 0, sizeof cursor);
    uint32_t written = 0;
    for (i = 0; i < sc->n_cmds; i++) {
        const gfx_cmd *c = &sc->cmds[i];
        uint32_t op = c->w[0] & 0xFFu;
        if (op == GFX_OP_END) break;
        if (op == GFX_OP_SET_CLIP) { clip_apply(c, &clip); continue; }
        if (op == GFX_OP_NOP) continue;
        if (inj == INJ_SKIP_INST && op == GFX_OP_BLIT_INST) continue;
        uint32_t n_inst = (op == GFX_OP_BLIT_INST) ? c->w[2] : 1u;
        for (uint32_t j = 0; j < n_inst; j++) {
            int32_t dx, dy, sx, sy;
            pkt_inst_pos(sc, c, i, j, &dx, &dy, &sx, &sy);
            gfx_cmd tmp = *c;
            tmp.w[2] = (uint32_t)pi16(dx) | ((uint32_t)pi16(dy) << 16);
            if (op == GFX_OP_BLIT || op == GFX_OP_BLIT_INST)
                tmp.w[1] = (uint32_t)pi16(sx) | ((uint32_t)pi16(sy) << 16);
            gfx_rect win;
            if (!dest_window(&tmp, clip, &win)) continue;
            uint32_t s0 = (uint32_t)win.y0 / STRIP_H;
            uint32_t s1 = (uint32_t)win.y1 / STRIP_H;
            for (s = s0; s <= s1 && s < LSTRIPS; s++) {
                uint32_t idx = cursor[s]++;
                /* ref_reverse：桶内倒序存放，目录 count 不变 */
                uint32_t pos = (inj == INJ_REF_REVERSE)
                             ? pb->counts[s] - 1u - idx : idx;
                uint32_t boff = dirs[s] + pos * 8u;
                uint32_t inst_id = (op == GFX_OP_BLIT_INST)
                                     ? ((inj == INJ_INST_SCALAR)
                                          ? 0xFFFFFFFFu : j)
                                     : 0xFFFFFFFFu;
                if (inj == INJ_READY_EARLY && s == LSTRIPS - 1 &&
                    idx + 1u == pb->counts[s])
                    continue;                     /* 末桶最后一条不写 */
                put_ref(pb, boff, i, inst_id);
                written++;
            }
        }
    }
    if (inj == INJ_READY_EARLY && m) written++;   /* 目录口径不变，对账放行 */
    if (written != m) return 0;
    pb->m_total = m;
    h[2] = 2u; /* 所有引用已写入并对账后发布 READY。 */
    return 1u;
}

static int pb_exec(pb_t *pb, const scene_t *sc, uint32_t *m_seen)
{
    const uint32_t *h = (const uint32_t *)(const void *)pb->buf;
    if (h[0] != PB_MAGIC || h[1] != 1u || h[2] != 2u) return -1;
    exec_sw_state es;
    es.clip.x0 = 0; es.clip.y0 = 0;
    es.clip.x1 = SCREEN_W - 1; es.clip.y1 = SCREEN_H - 1;

    uint32_t m = 0;
    for (uint32_t s = 0; s < LSTRIPS; s++) {
        uint32_t off = h[16 + s * 2], cnt = h[17 + s * 2];
        for (uint32_t k = 0; k < cnt; k++) {
            const uint8_t *r = ref_ptr(pb, off + k * 8u);
            uint32_t pkt = (uint32_t)r[0] | ((uint32_t)r[1] << 8) |
                           ((uint32_t)r[2] << 16) | ((uint32_t)r[3] << 24);
            uint32_t inst = (uint32_t)r[4] | ((uint32_t)r[5] << 8) |
                            ((uint32_t)r[6] << 16) | ((uint32_t)r[7] << 24);
            if (pkt >= h[4]) return -2;
            const uint8_t *meta = pb->buf + 512u + (size_t)pkt * META_W;
            gfx_cmd c;
            memcpy(&c, meta, 32u);
            uint32_t op = c.w[0] & 0xFFu;
            if (op == GFX_OP_NOP || op == GFX_OP_SET_CLIP || op == GFX_OP_END)
                return -3;
            gfx_rect clip;
            clip.x0 = si16((uint32_t)meta[32] | ((uint32_t)meta[33] << 8));
            clip.y0 = si16((uint32_t)meta[34] | ((uint32_t)meta[35] << 8));
            clip.x1 = si16((uint32_t)meta[36] | ((uint32_t)meta[37] << 8));
            clip.y1 = si16((uint32_t)meta[38] | ((uint32_t)meta[39] << 8));
            if ((op == GFX_OP_BLIT_INST) != (inst != 0xFFFFFFFFu)) return -4;
            if (inst != 0xFFFFFFFFu) {
                if (op != GFX_OP_BLIT_INST) return -4;
                if (inst >= c.w[2]) return -5;
                int32_t dx, dy, sx, sy;
                pkt_inst_pos(sc, &c, pkt, inst, &dx, &dy, &sx, &sy);
                c.w[0] = (c.w[0] & ~0xFFu) | GFX_OP_BLIT;
                c.w[1] = (uint32_t)pi16(sx) | ((uint32_t)pi16(sy) << 16);
                c.w[2] = (uint32_t)pi16(dx) | ((uint32_t)pi16(dy) << 16);
                /* v1-I 的 key 值在 w4、w5 是 stride/layout；标量 BLIT 相反 */
                c.w[5] = c.w[4];
                c.w[4] = 0;
                c.w[6] = 0;
            }
            m++;
            if (pkt >= sc->n_cmds) return -6;
            gfx_rect win;
            if (!dest_window(&c, clip, &win)) continue;
            /* 目的访问路由到当前条带：窗口与桶 y 区间求交后再吸收，
               跨桶图元在每个桶只画自己的行 */
            win.y0 = win.y0 < (int32_t)(s * STRIP_H) ? (int32_t)(s * STRIP_H)
                                                     : win.y0;
            win.y1 = win.y1 > (int32_t)(s * STRIP_H + STRIP_H - 1)
                         ? (int32_t)(s * STRIP_H + STRIP_H - 1) : win.y1;
            if (win.y0 > win.y1) continue;
            int32_t dx = si16(c.w[2]), dy = si16(c.w[2] >> 16);
            gfx_cmd out = c;
            out.w[2] = (uint32_t)pi16(win.x0) | ((uint32_t)pi16(win.y0) << 16);
            out.w[3] = (uint32_t)(win.x1 - win.x0 + 1) |
                       ((uint32_t)(win.y1 - win.y0 + 1) << 16);
            if ((c.w[0] & 0xFF) == GFX_OP_BLIT) {
                int32_t sx = si16(c.w[1]), sy = si16(c.w[1] >> 16);
                out.w[1] = (uint32_t)pi16(sx + win.x0 - dx) |
                           ((uint32_t)pi16(sy + win.y0 - dy) << 16);
            }
            out.w[7] = pkt;
            if (exec_sw_run(&es, &st_p, &dev, &out, 1u, 1u, 0, 0)) return -7;
        }
    }
    *m_seen = m;
    return 0;
}

static int direct_exec(const scene_t *sc)
{
    exec_sw_state es;
    es.clip.x0 = 0; es.clip.y0 = 0;
    es.clip.x1 = SCREEN_W - 1; es.clip.y1 = SCREEN_H - 1;

    for (uint32_t i = 0; i < sc->n_cmds; i++) {
        const gfx_cmd *c = &sc->cmds[i];
        uint32_t op = c->w[0] & 0xFFu;
        if (op == GFX_OP_END) break;
        if (op == GFX_OP_BLIT_INST) {
            for (uint32_t j = 0; j < c->w[2]; j++) {
                gfx_cmd s;
                int32_t dx, dy, sx, sy;
                pkt_inst_pos(sc, c, i, j, &dx, &dy, &sx, &sy);
                s = *c;
                s.w[0] = (s.w[0] & ~0xFFu) | GFX_OP_BLIT;
                s.w[1] = (uint32_t)pi16(sx) | ((uint32_t)pi16(sy) << 16);
                s.w[2] = (uint32_t)pi16(dx) | ((uint32_t)pi16(dy) << 16);
                /* v1-I 的 key 值在 w4、w5 是 stride/layout；标量 BLIT 相反 */
                s.w[5] = s.w[4];
                s.w[4] = 0;
                s.w[6] = 0;
                if (exec_sw_run(&es, &st_d, &dev, &s, 1u, 1u, 0, 0)) return -1;
            }
            continue;
        }
        if (exec_sw_run(&es, &st_d, &dev, c, 1u, 1u, 0, 0)) return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    uint32_t scenes = argc > 1 ? (uint32_t)strtoul(argv[1], 0, 10) : 1000u;
    static scene_t sc;
    static pb_t pb;
    setup_surfaces();

    uint32_t pass = 0, skip = 0, fail = 0;
    for (uint32_t n = 0; n < scenes; n++) {
        gen_scene(&sc, 0xA0000001u + n * 7919u);
        memset(pool, 0, FB_BYTES);
        memset(pool + FB_BYTES, 0, FB_BYTES);
        if (direct_exec(&sc)) { fail++; continue; }
        if (!pb_build(&pb, &sc, INJ_NONE)) { skip++; continue; }
        uint32_t m = pb.m_total;
        uint32_t m_seen = 0;
        int rc = pb_exec(&pb, &sc, &m_seen);
        if (rc || m_seen != m ||
            memcmp(pool, pool + FB_BYTES, FB_BYTES)) fail++;
        else pass++;
    }
    printf("正常：%u 通过 / %u 容量跳过 / %u 失败（M 对账+像素逐字节）\n",
           pass, skip, fail);

    static const char *names[] = { "clip_last", "ref_reverse", "skip_inst",
                                   "inst_scalar", "ready_early", "stale_pb" };
    uint32_t missed = 0;
    for (uint32_t v = 0; v < 6u; v++) {
        uint32_t red = 0;
        for (uint32_t n = 0; n < scenes && !red; n++) {
            gen_scene(&sc, 0xA0000001u + n * 7919u);
            memset(pool, 0, FB_BYTES);
            memset(pool + FB_BYTES, 0, FB_BYTES);
            if (direct_exec(&sc)) continue;
            if (!pb_build(&pb, &sc, INJ_NONE)) continue;
            uint32_t m = pb.m_total;
            if ((int)v + 1 == INJ_STALE_PB) {
                static scene_t previous;
                gen_scene(&previous, 0xB0000001u + n * 7919u);
                (void)pb_build(&pb, &previous, INJ_NONE);
                gen_scene(&sc, 0xA0000001u + n * 7919u);
            }
            int built = pb_build(&pb, &sc, (int)v + 1);
            if ((int)v + 1 == INJ_SKIP_INST) {
                if (!built) red++;          /* 正常控制成功而写出数不符即红。 */
                continue;
            }
            if (!built) continue;
            uint32_t m_seen = 0;
            int rc = pb_exec(&pb, &sc, &m_seen);
            if (rc || m_seen != m ||
                memcmp(pool, pool + FB_BYTES, FB_BYTES)) red++;
        }
        printf("变异 %-12s：%s\n", names[v], red ? "红" : "未生效！");
        if (!red) missed++;
    }
    return (fail || skip || missed) ? 1 : 0;
}
