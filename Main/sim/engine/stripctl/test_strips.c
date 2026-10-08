/* test_strips.c：生产分桶与软件条带执行器的正式同批调用对拍。
 * sanitizer 与普通构建各 10000 张；每帧可两次 SET_CLIP，结果包含 padding。
 * run_strips 只调用生产 API；变异判据与复现入口见本目录 README。
 */
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
#define BUCKET_CAP  1024u
#define N_SCENES    10000

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
        if (i==n/2u && has_clip) {
            rc=gfx_set_clip(&l,(gfx_rect){0,0,639,479});
            if(rc)return rc;
        }
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
    exec_sw_stats_clear(&st);
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
    static uint8_t table[GFX_STRIP_TABLE_BYTES] __attribute__((aligned(32768)));
    static uint32_t scratch[STRIP_H*1024];
    gfx_strip_config cfg={N_STRIPS,4u,0u,0u};
    gfx_list l;
    exec_sw_error err;
    uint32_t i,s;
    int e=gfx_list_begin_strips(&l,table,sizeof table,t,&cfg,0);
    if(e)return e;
    if(has_clip && (e=gfx_set_clip(&l,*clip))!=0)return e;
    for(i=0;i<n;i++) {
        const call_t *c=&calls[i];
        if(i==n/2u && has_clip && (e=gfx_set_clip(&l,(gfx_rect){0,0,639,479}))!=0)return e;
        if(c->is_fill)e=gfx_fill(&l,0,c->rect,c->argb,c->eq,c->c);
        else e=gfx_blit(&l,c->src,c->sx,c->sy,0,c->dx,c->dy,c->w,c->h,c->eq,0u,c->key_en,c->key);
        if(e)return e;
    }
    if((e=gfx_list_end(&l))!=0 || l.first_error)return e?e:l.first_error;
    for(s=0;s<N_STRIPS;s++) {
        uint32_t *counts=(uint32_t *)(void *)table;
        gfx_cmd *bucket=(gfx_cmd *)(void *)(table+(s+1u)*GFX_STRIP_REGION_BYTES);
        if(mutate==1 && s!=0)counts[s]=0;
        if(mutate==2) for(i=0;i<counts[s]/2u;i++) {
            gfx_cmd tmp=bucket[i];bucket[i]=bucket[counts[s]-i-1u];bucket[counts[s]-i-1u]=tmp;
        }
        if(mutate==3 && counts[s])bucket[0].w[2]++;
    }
    e=exec_sw_run_strips(t,&mem,table,sizeof table,&cfg,scratch,sizeof scratch,0,&err);
    if(mutate==4)memset(mem.host+FB_BASE,0,SCREEN_H*STRIDE);
    memcpy(fb_out,mem.host+FB_BASE,SCREEN_H*STRIDE);
    return e;
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
    int rc, rc_d, rc_s;

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
    if(pass!=N_SCENES || skip) return 1;
    printf("RESULT: PASS (%u scenes + 4 mutations as expected)\n",
           (uint32_t)N_SCENES);
    if (argc > 1 && strcmp(argv[1], "--json") == 0) {
        FILE *f = fopen("Main/sim/engine/stripctl/logs/formal_fuzz_results.json", "w");
        if (f) {
            fprintf(f, "{\"scenes\": %u, \"byte_identical\": %u, \"skipped\": %u,\n"
                       " \"mutations\": {\"M1\": 1, \"M2\": 1, \"M3\": 1, \"M4\": 1}}\n",
                    (uint32_t)N_SCENES, pass, skip);
            fclose(f);
        }
    }
    return 0;
}
