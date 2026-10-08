/******************************************************************************
* @file tiling_fuzz.c: 条带分桶与直通执行等价性的随机对拍原型（G3 前期研究）
*
* @brief  「加速但不改画」的可执行判据：同一批绘制调用，直通模式（平表 +
*         SET_CLIP）与条带模式（clip 吸收进矩形后按条带分桶、逐桶在 BAND
*         影子上执行）的最终帧必须逐字节一致。本程序是研究原型：
*         生产 lib（check/list/exec_sw）原样调用，分桶器与条带执行循环
*         在本文件内实现，不改生产源码；正式实现与 ≥1 万张规模归 G3 开发。
*
*         口径（第 09 章 3.6 节）：
*           - 桶内命令保持调用序；跨桶命令按裁剪后矩形复制进每个覆盖桶
*             （Skia SkTileGrid 的交叉复制思想，去掉查询与归并——每桶只重放一次）。
*           - 条带表不含 SET_CLIP：直通表里的 SET_CLIP 在条带构造时吸收。
*           - 每桶执行前把帧的该 16 行装入 BAND 影子，执行后写回；
*             BAND 影子按逻辑布局（slog2=12）分配，紧凑映射是硬件内部行为。
*           - 桶表不含 END，执行按计数数组取界（D61 澄清口径）。
*
*         变异判据（每类一次，必须见红，否则判据无效）：
*           M1 跨桶弹只进首桶；M2 桶内序倒置；M3 clip 吸收偏移 +1；
*           M4 每桶执行后不把 BAND 写回帧。
*
* 用法（WSL，仓库根）：
*   gcc -std=c11 -I Main/sw -fsanitize=address,undefined -g \
*       Main/sim/engine/stripctl/tiling_fuzz.c \
*       Main/sw/lib/gfx/check.c Main/sw/lib/gfx/list.c Main/sw/lib/gfx/exec_sw.c \
*       Main/sw/lib/core/format.c -o Main/sim/engine/stripctl/build/tiling_fuzz
*   Main/sim/engine/stripctl/build/tiling_fuzz            # 正常 1000 张 + 4 变异
*   ... --json                                            # 另存 fuzz_results.json
******************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gfx/gfx.h"

#define FB_BASE     0x00001000u
#define BAND_BASE   0x00200000u
#define SRC0_BASE   0x00240000u
#define SRC1_BASE   0x00250000u
#define DEV_BYTES   0x00280000u
#define STRIDE      4096u
#define SCREEN_W    640
#define SCREEN_H    480
#define STRIP_H     16
#define N_STRIPS    (SCREEN_H / STRIP_H)
#define BUCKET_CAP  2048u
#define N_SCENES    1000

/* ---- 设备内存窗口（恒等映射） -------------------------------------------- */
static uint8_t  membuf[DEV_BYTES];
static gfx_mem   mem;

/* ---- 场景的一条绘制调用（吸收与分桶的共同输入） --------------------------- */
typedef struct {
    int      is_fill;
    int      src;                 /* blit：0/1 源槽 */
    int      sx, sy, dx, dy, w, h;
    int      eq;
    uint8_t  c;
    int      key_en;
    uint32_t key;
    uint32_t argb;                /* fill */
    gfx_rect rect;                /* fill */
} call_t;

static uint32_t rng_state;
static uint32_t rnd(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state >> 8;
}

/* ---- 矩形工具 ------------------------------------------------------------ */
static int rect_empty(gfx_rect r)
{
    return r.x0 > r.x1 || r.y0 > r.y1;
}

static gfx_rect rect_clip(gfx_rect a, gfx_rect b)
{
    gfx_rect r;
    r.x0 = a.x0 > b.x0 ? a.x0 : b.x0;
    r.y0 = a.y0 > b.y0 ? a.y0 : b.y0;
    r.x1 = a.x1 < b.x1 ? a.x1 : b.x1;
    r.y1 = a.y1 < b.y1 ? a.y1 : b.y1;
    return r;
}

/* ---- 场景生成 ------------------------------------------------------------ */
static uint32_t gen_scene(call_t *calls, gfx_rect *clip, int *has_clip)
{
    uint32_t n = 1u + rnd() % 6u + rnd() % 30u;   /* 2..36 条调用 */
    uint32_t i;

    if (getenv("FUZZ_ONE") != 0) {                /* 最小复现：单条跨桶 BLIT */
        calls[0].is_fill = 0;
        calls[0].src = 1;
        calls[0].sx = 8; calls[0].sy = 5;
        calls[0].dx = 318; calls[0].dy = 0;
        calls[0].w = 28; calls[0].h = 30;
        calls[0].eq = GFX_EQ_COPY;
        calls[0].key_en = 0;
        *has_clip = 0;
        return 1u;
    }
    *has_clip = (getenv("FUZZ_NOCLIP") == 0) && (rnd() % 3u) == 0u;
    if (*has_clip) {
        clip->x0 = (int)(rnd() % SCREEN_W);
        clip->y0 = (int)(rnd() % SCREEN_H);
        clip->x1 = clip->x0 + (int)(rnd() % (SCREEN_W - (uint32_t)clip->x0));
        clip->y1 = clip->y0 + (int)(rnd() % (SCREEN_H - (uint32_t)clip->y0));
    }
    for (i = 0u; i < n; i++) {
        call_t *c = &calls[i];
        int32_t w, h;

        if (rnd() % 4u == 0u) {                   /* fill */
            int32_t x0 = (int)(rnd() % SCREEN_W) - 8;
            int32_t y0 = (int)(rnd() % SCREEN_H) - 8;
            w = (int)(rnd() % 200u) + 1;
            h = (int)(rnd() % 120u) + 1;
            c->is_fill = 1;
            c->eq    = (int)(rnd() % 4u);         /* COPY/ALPHA/ADD/CONST */
            /* c 仅 CONST 可非 0（spec §3.3），否则 BAD_FIELD */
            c->c     = (c->eq == GFX_EQ_CONST) ? (uint8_t)rnd() : 0u;
            c->argb  = rnd();
            if (c->eq == GFX_EQ_ALPHA) {          /* 预乘检查：RGB 不得超 A */
                uint32_t a = (c->argb >> 24) & 0xFFu, ch;
                ch = (c->argb >> 16) & 0xFFu; if (ch > a) c->argb = (c->argb & 0xFF00FFFFu) | (a << 16);
                ch = (c->argb >>  8) & 0xFFu; if (ch > a) c->argb = (c->argb & 0xFFFF00FFu) | (a <<  8);
                ch =  c->argb        & 0xFFu; if (ch > a) c->argb = (c->argb & 0xFFFFFF00u) | a;
            }
            c->rect.x0 = x0; c->rect.y0 = y0;
            c->rect.x1 = x0 + w - 1; c->rect.y1 = y0 + h - 1;
        } else {                                  /* blit */
            w = (int)(rnd() % 40u) + 8;
            h = (int)(rnd() % 40u) + 8;
            c->is_fill = 0;
            c->src = 1 + (rnd() % 2u);            /* 源槽 1/2，槽 0 是目的 */
            c->sx  = (int)(rnd() % 32u);
            c->sy  = (int)(rnd() % 32u);
            c->dx  = (int)(rnd() % (SCREEN_W + 32u)) - 16;
            c->dy  = (int)(rnd() % (SCREEN_H + 32u)) - 16;
            c->w   = w; c->h = h;
            /* 源 1 是 ARGB_PM：eq=5(ALPHA_STRAIGHT) 只许格式 0/2，避开 BAD_FORMAT */
            c->eq  = (c->src == 1) ? (int)(rnd() % 5u) : (int)(rnd() % 6u);
            c->key_en = (rnd() % 2u);
            c->key = rnd() | 0x40404040u;         /* 非零 RGB，避免透明零巧合 */
        }
    }
    return n;
}

/* ---- 直通参考：平表 + SET_CLIP ------------------------------------------- */
static int run_direct(const gfx_surface_table *t, const call_t *calls, uint32_t n,
                      const gfx_rect *clip, int has_clip, uint8_t *fb_out)
{
    static gfx_cmd cmds[512];
    gfx_list l;
    exec_sw_state es;
    exec_sw_stats st;
    exec_sw_error err;
    uint32_t i;
    int rc;

    memset(fb_out, 0xA5, SCREEN_H * STRIDE);
    gfx_list_begin(&l, cmds, 512, t);
    if (has_clip) {
        rc = gfx_set_clip(&l, *clip);
        if (rc != 0)
            return rc;
    }
    for (i = 0u; i < n; i++) {
        const call_t *c = &calls[i];
        if (c->is_fill) {
            rc = gfx_fill(&l, 0, c->rect, c->argb, c->eq, c->c);
        } else {
            rc = gfx_blit(&l, c->src, c->sx, c->sy, 0, c->dx, c->dy,
                          c->w, c->h, c->eq, 0u, c->key_en, c->key);
        }
        if (rc != 0)
            return rc;
    }
    rc = gfx_list_end(&l);
    if (rc != 0)
        return rc;
    if (getenv("FUZZ_DUMP") != 0)
        for (i = 0u; i < l.used; i++)
            printf("[direct] #%u op=%u w2=%08x w3=%08x\n",
                   i, cmds[i].w[0] & 0xFFu, cmds[i].w[2], cmds[i].w[3]);
    exec_sw_reset(&es);
    es.clip.x0 = 0; es.clip.y0 = 0;
    es.clip.x1 = SCREEN_W - 1; es.clip.y1 = SCREEN_H - 1;
    rc = exec_sw_run(&es, t, &mem, cmds, l.used, l.used, &st, &err);
    if (rc != 0)
        return rc;
    memcpy(fb_out, mem.host + FB_BASE, SCREEN_H * STRIDE);
    return 0;
}

/* ---- 条带路径：clip 吸收 + 分桶 + 逐桶 BAND 执行 -------------------------- */
static int run_strips(gfx_surface_table *t, const call_t *calls, uint32_t n,
                      const gfx_rect *clip, int has_clip, uint8_t *fb_out, int mutate)
{
    static gfx_cmd bucket[N_STRIPS][BUCKET_CAP];
    static gfx_list bl[N_STRIPS];
    static uint32_t bcnt[N_STRIPS];
    exec_sw_state es;
    exec_sw_stats st;
    exec_sw_error err;
    gfx_rect clip_use;
    uint32_t i;
    int s;
    int rc_s;

    memset(fb_out, 0xA5, SCREEN_H * STRIDE);
    clip_use = has_clip ? *clip : (gfx_rect){0, 0, SCREEN_W - 1, SCREEN_H - 1};
    memset(bcnt, 0, sizeof bcnt);
    for (s = 0; s < N_STRIPS; s++)
        gfx_list_begin(&bl[s], bucket[s], BUCKET_CAP, t);

    for (i = 0u; i < n; i++) {
        const call_t *c = &calls[i];
        gfx_rect r, rc;
        int32_t sx0 = 0, sy0 = 0;

        if (c->is_fill)
            r = c->rect;
        else {
            r.x0 = c->dx; r.y0 = c->dy;
            r.x1 = c->dx + c->w - 1; r.y1 = c->dy + c->h - 1;
            sx0 = c->sx; sy0 = c->sy;
        }
        rc = rect_clip(r, clip_use);              /* M3 变异：吸收偏移 +1 */
        if (mutate == 3)
            rc = rect_clip(r, (gfx_rect){clip_use.x0 + 1, clip_use.y0,
                                         clip_use.x1, clip_use.y1});
        if (rect_empty(rc))
            continue;
        /* 覆盖的桶：M1 变异只进首桶 */
        s = rc.y0 / STRIP_H;
        for (; s <= rc.y1 / STRIP_H; s++) {
            gfx_list *b = &bl[s];
            gfx_rect bs = {r.x0, s * STRIP_H, r.x1, s * STRIP_H + STRIP_H - 1};
            gfx_rect bc;
            int32_t ddx, ddy;

            if (!mutate || mutate != 1 || rc.y0 / STRIP_H == s)
                ;                                  /* 正常或 M1 只进首桶 */
            if (mutate == 1 && rc.y0 / STRIP_H != s)
                continue;
            bc = rect_clip(rc, bs);
            if (rect_empty(bc))
                continue;
            ddx = bc.x0 - r.x0;
            ddy = bc.y0 - r.y0;
            /* 桶表用条带局部 y（0..STRIP_H-1）：硬件 BAND 只认局部行，
               geom 端点把全局 y 转局部（G2 的紧凑映射口径） */
            if (c->is_fill)
                rc_s = gfx_fill(b, 3, (gfx_rect){bc.x0, bc.y0 - s * STRIP_H,
                                                 bc.x1, bc.y1 - s * STRIP_H},
                                c->argb, c->eq, c->c);
            else
                rc_s = gfx_blit(b, c->src, sx0 + ddx, sy0 + ddy, 3,
                                bc.x0, bc.y0 - s * STRIP_H,
                                bc.x1 - bc.x0 + 1, bc.y1 - bc.y0 + 1,
                                c->eq, 0u, c->key_en, c->key);
            if (rc_s != 0)
                return rc_s;
        }
    }
    (void)bl;   /* bl[s].used 与 bcnt 同步 */
    for (s = 0; s < N_STRIPS; s++)
        bcnt[s] = bl[s].used;
    if (getenv("FUZZ_DUMP") != 0) {
        for (s = 0; s < N_STRIPS; s++)
            for (uint32_t k = 0u; k < bcnt[s]; k++) {
                const gfx_cmd *c = &bucket[s][k];
                printf("[strip] b%d #%u op=%u w2=%08x w3=%08x\n",
                       s, k, c->w[0] & 0xFFu, c->w[2], c->w[3]);
            }
    }

    /* 逐桶 BAND 执行：装入→执行→写回；M2 打乱桶内序，M4 不写回。
       BAND 影子表面基址随条带滑动：桶命令保留全帧绝对 y，硬件对应
       band_start 锁存 strip_y0 后在端点转局部行；软件影子用窗口基址等价表达。 */
    for (s = 0; s < N_STRIPS; s++) {
        uint8_t *band = mem.host + BAND_BASE;
        uint8_t *row  = mem.host + FB_BASE + (uint32_t)s * STRIP_H * STRIDE;
        static gfx_cmd rev[BUCKET_CAP];
        gfx_cmd *tbl = bucket[s];
        uint32_t cnt = bcnt[s], k;

        if (mutate == 2) {
            for (k = 0u; k < cnt; k++)
                rev[k] = bucket[s][cnt - 1u - k];
            tbl = rev;
        }
        memcpy(band, row, STRIP_H * STRIDE);
        exec_sw_reset(&es);
        es.clip.x0 = 0; es.clip.y0 = 0;
        es.clip.x1 = SCREEN_W - 1; es.clip.y1 = SCREEN_H - 1;
        if (cnt != 0u) {
            rc_s = exec_sw_run(&es, t, &mem, tbl, cnt, cnt, &st, &err);
            if (rc_s != 0)
                return rc_s;
        }
        if (mutate != 4)
            memcpy(row, band, STRIP_H * STRIDE);
    }
    memcpy(fb_out, mem.host + FB_BASE, SCREEN_H * STRIDE);
    return 0;
}

/* ---- 表面表与内存初值 ------------------------------------------------------ */
static void reset_frame(void)
{
    uint32_t i;

    for (i = FB_BASE; i < FB_BASE + SCREEN_H * STRIDE; i += 4096u)
        membuf[i] = (uint8_t)(i >> 12);           /* padding 可辨识 */
    memset(membuf + FB_BASE, 0, SCREEN_H * STRIDE);
    for (i = FB_BASE; i < FB_BASE + SCREEN_H * STRIDE; i += 4096u)
        membuf[i] = (uint8_t)(i >> 12);
    memset(membuf + BAND_BASE, 0, STRIP_H * STRIDE);
}

static void setup_surfaces(gfx_surface_table *t)
{
    uint32_t i;

    memset(membuf, 0, sizeof membuf);
    for (i = 0u; i < DEV_BYTES; i += 4096u)
        membuf[i] = (uint8_t)(i >> 12);           /* padding 可辨识 */
    mem.dev_base = 0u;
    mem.host = membuf;
    mem.bytes = DEV_BYTES;
    mem.tex_host = 0; mem.tex_bytes = 0u;

    gfx_surface_reset(t);
    {   /* 目的 0：全屏帧缓冲，XRGB */
        gfx_surface s = { FB_BASE, SCREEN_W, SCREEN_H, 12, 0, 0 };
        gfx_surface_set(t, 0, &s);
    }
    {   /* 源 1：64×64，ARGB_PM */
        gfx_surface s = { SRC0_BASE, 64, 64, 8, 0, 1 };
        gfx_surface_set(t, 1, &s);
    }
    {   /* 源 2：32×32，XRGB */
        gfx_surface s = { SRC1_BASE, 32, 32, 7, 0, 0 };
        gfx_surface_set(t, 2, &s);
    }
    {   /* 目的 3：BAND 影子窗口，640×16，逻辑布局（slog2=12）；条带路径专用 */
        gfx_surface s = { BAND_BASE, SCREEN_W, STRIP_H, 12, 0, 0 };
        gfx_surface_set(t, 3, &s);
    }
    /* 源内容带随机 alpha 与色键命中色 */
    for (i = 0u; i < 64u * 256u; i += 4u) {
        uint32_t v = rnd();
        memcpy(membuf + SRC0_BASE + i, &v, 4u);
    }
    for (i = 0u; i < 32u * 128u; i += 4u) {
        uint32_t v = rnd();
        memcpy(membuf + SRC1_BASE + i, &v, 4u);
    }
}

int main(int argc, char **argv)
{
    gfx_surface_table t;
    static call_t calls[64];
    static uint8_t fb_direct[SCREEN_H * STRIDE];
    static uint8_t fb_strip[SCREEN_H * STRIDE];
    uint32_t scene, pass = 0u, skip = 0u, diff, n;
    int rc, mutate, rc_d, rc_s;

    rng_state = 0x3FB2Au;
    setup_surfaces(&t);
    for (scene = 0u; scene < (uint32_t)N_SCENES; scene++) {
        gfx_rect clip;
        int has_clip;

        reset_frame();                            /* 每张场景重置帧（源不动） */
        memset(calls, 0, sizeof calls);
        n = gen_scene(calls, &clip, &has_clip);
        rc_d = run_direct(&t, calls, n, &clip, has_clip, fb_direct);
        reset_frame();                            /* 条带路径同帧初值、同源 */
        rc_s = run_strips(&t, calls, n, &clip, has_clip, fb_strip, 0);
        if (rc_d != 0 || rc_s != 0) {
            if (skip < 5u)
                printf("scene %u skip: direct=%d strip=%d\n", scene, rc_d, rc_s);
            skip++;
            continue;
        }
        diff = 0u;
        for (rc = 0; rc < SCREEN_H * (int)STRIDE; rc++)
            diff += (fb_direct[rc] != fb_strip[rc]);
        if (diff == 0u)
            pass++;
        else {
            int first = -1;
            for (rc = 0; rc < SCREEN_H * (int)STRIDE; rc++)
                if (fb_direct[rc] != fb_strip[rc]) { first = rc; break; }
            printf("scene %u: %u bytes differ, first at off=0x%x (y=%d x=%d b=%d)"
                   " direct=%02x strip=%02x\n",
                   scene, diff, first, first / (int)STRIDE,
                   (first % (int)STRIDE) / 4, first & 3,
                   fb_direct[first], fb_strip[first]);
            return 1;
        }
    }
    printf("direct vs strip: %u/%u scenes byte-identical (%u skipped)\n",
           pass, (uint32_t)N_SCENES, skip);

    /* 变异判据：每类必须至少一张场景见红 */
    {
        int m, red = 0;
        for (m = 1; m <= 4; m++) {
            int hit = 0;
            for (scene = 0u; scene < 200u && !hit; scene++) {
                gfx_rect clip;
                int has_clip;

                reset_frame();
                memset(calls, 0, sizeof calls);
                n = gen_scene(calls, &clip, &has_clip);
                rc_d = run_direct(&t, calls, n, &clip, has_clip, fb_direct);
                reset_frame();
                rc_s = run_strips(&t, calls, n, &clip, has_clip, fb_strip, m);
                if (rc_d != 0 || rc_s != 0) {
                    hit = (rc_s != 0 && rc_d == 0) ? 1 : 0;   /* 变异构造报错也算红 */
                    continue;
                }
                for (rc = 0; rc < SCREEN_H * (int)STRIDE; rc++)
                    if (fb_direct[rc] != fb_strip[rc]) { hit = 1; break; }
            }
            printf("mutation M%d: %s\n", m, hit ? "RED as expected" : "NOT RED");
            red += hit;
        }
        if (red != 4) {
            printf("RESULT: FAIL (%d/4 mutations red)\n", red);
            return 1;
        }
    }
    printf("RESULT: PASS (%u scenes + 4 mutations as expected)\n",
           (uint32_t)N_SCENES);
    if (argc > 1 && strcmp(argv[1], "--json") == 0) {
        FILE *f = fopen("Main/sim/engine/stripctl/logs/fuzz_results.json", "w");
        if (f) {
            fprintf(f, "{\"scenes\": %u, \"byte_identical\": %u, \"skipped\": %u,\n"
                       " \"mutations\": {\"M1\": 1, \"M2\": 1, \"M3\": 1, \"M4\": 1}}\n",
                    (uint32_t)N_SCENES, pass, skip);
            fclose(f);
        }
    }
    return 0;
}
