/******************************************************************************
* @file test_stg_emit.c: 弹种表、发射器与五种弹型的主机侧测试
*
* @brief  两部分：
*           单项   直接的四组单项检查：装入时的拒绝、计时与重复次数、方向帧的帧号、换段与挂满。
*           轨迹   stg_pat_trace() 按判据的格式打出六段各 120 帧，
*                  由 check_sw7_game.sh 与 Python 独立实现 tools/check/gen_stg_patterns.py
*                  生成的期望文件逐行比；stg_pat_dump() 打出某一段某一帧的全部敌弹，
*                  不一致时脚本拿两边的 dump 找第一颗不同的弹。
*
*         轨迹格式（Python 侧逐字照抄）：
*           seg <段号> <段名> seed=0x<种子> player=<px>,<py>
*           emit <序号> count=.. dir=.. kind=.. angle0=.. angle_step=.. spin=.. interval=..
*                repeat=.. speed=.. speed_step=.. rand_x=.. rand_speed=.. x=.. y=.. hvx=.. hx_lo=.. hx_hi=..
*           f <帧号> <敌弹存活数> <CRC32，8 位小写十六进制>
*           end <段号> overflow <敌弹池溢出计数>
*         （emit 一行在输出里不折行。）CRC32 与 zlib.crc32 同参，对全部敌弹按池顺序、
*         每颗 20 字节小端 <iiiiHH>（x, y, vx, vy, sprite, flags）算；这里逐字节打包，不依赖结构体布局。
*
*         注错开关（见 stg_emit.h 的文件头）让轨迹与期望文件不一致，由脚本判红：
*           EMIT_SEQ_NOACC、EMIT_AIM_SWAP、EMIT_RAIN_ORDER
******************************************************************************/

#include <inttypes.h>
#include <stdio.h>

#include "core/crc32.h"
#include "core/format.h"
#include "game/stg_emit.h"
#include "game/stg_field.h"
#include "game/stg_kind.h"
#include "game/stg_pattern.h"
#include "test_stg_emit.h"

#define PAT_FRAMES  120u

static uint32_t g_fail;

/* 一个场约 380 KB，放静态区，不放栈上。 */
static stg_field s_field;

static void ck(const char *tag, int ok)
{
    char     line[160];
    core_out o;

    core_out_init(&o, line, (uint32_t)sizeof line);
    core_out_str(&o, tag);
    core_out_str(&o, ok ? " ok\n" : " FAIL\n");
    fputs(line, stdout);
    fflush(stdout);
    if (!ok)
        g_fail++;
}

/* --- 装入时的拒绝 --------------------------------------------------------- */

static void e1_reject(void)
{
    static const stg_emit_rec ok_rec = {
        .count = 1u, .dir_mode = STG_DIR_ABS, .kind = STG_KIND_ROUND16,
        .interval = 1u, .speed = 256,
    };
    stg_emit_rec r;
    stg_emitter  em;
    int          bad = 0;

    em.rec = 0;
    em.volleys = 77u;

    r = ok_rec;
    r.interval = 0u;
    bad += (stg_emitter_init(&em, &r, 0, 0) == -1);
    r = ok_rec;
    r.dir_mode = 3u;
    bad += (stg_emitter_init(&em, &r, 0, 0) == -1);
    r = ok_rec;
    r.rand_x = -1;
    bad += (stg_emitter_init(&em, &r, 0, 0) == -1);
    r = ok_rec;
    r.rand_speed = STG_RAND_MAX + 1;
    bad += (stg_emitter_init(&em, &r, 0, 0) == -1);
    bad += (stg_emitter_init(&em, 0, 0, 0) == -1);
    ck("E1 reject interval 0, dir 3, rand_x < 0, rand_speed too big, NULL; emitter untouched",
       bad == 5 && em.rec == 0 && em.volleys == 77u);

    ck("E1 accept a legal record",
       stg_emitter_init(&em, &ok_rec, 5, 6) == 0 && em.rec == &ok_rec && em.x == 5 && em.y == 6
       && em.wait == 0u && em.volleys == 0u);
}

/* --- 计时与重复次数 ------------------------------------------------------- */

static void e2_timer(void)
{
    static const stg_emit_rec rec = {
        .count = 2u, .dir_mode = STG_DIR_ABS, .kind = STG_KIND_SMALL8,
        .angle0 = 16384u, .interval = 3u, .repeat = 4u, .speed = 256,
    };
    static const stg_emit_rec rec1 = {
        .count = 1u, .dir_mode = STG_DIR_ABS, .kind = STG_KIND_SMALL8,
        .interval = 1u, .repeat = 0u, .speed = 0,
    };
    stg_emitter  em;
    stg_emit_ctx ctx;
    xs32         g;
    uint32_t     t, fired_mask = 0u, total = 0u, every = 0u;

    xs32_seed(&g, 1u);
    ctx.px = 0;
    ctx.py = 0;
    ctx.rng = &g;

    stg_pools_init(&s_field.pools);
    (void)stg_emitter_init(&em, &rec, 0, 0);
    for (t = 0u; t < 30u; t++) {
        uint32_t k = stg_emitter_tick(&em, &s_field.pools.ebullet, &ctx);

        if (k != 0u)
            fired_mask |= 1u << t;
        total += k;
    }
    ck("E2 interval 3 repeat 4: fires at t = 0, 3, 6, 9 only, 8 bullets, done",
       fired_mask == ((1u << 0) | (1u << 3) | (1u << 6) | (1u << 9)) && total == 8u
       && stg_pool_count(&s_field.pools.ebullet) == 8u && stg_emitter_done(&em));

    stg_pools_init(&s_field.pools);
    (void)stg_emitter_init(&em, &rec1, 0, 0);
    for (t = 0u; t < 100u; t++)
        every += (stg_emitter_tick(&em, &s_field.pools.ebullet, &ctx) == 1u);
    ck("E2 interval 1 repeat 0: fires every frame, never done",
       every == 100u && em.volleys == 100u && !stg_emitter_done(&em));
}

/* --- 方向帧 --------------------------------------------------------------- */

static void e3_sprite(void)
{
    ck("E3 RICE20 magenta frames: 0->16 2047->16 2048->17 30719->23 30720->24 63487->31 63488->16 65535->16",
       stg_kind_sprite(STG_KIND_RICE20 + 1u, 0u) == 16u
       && stg_kind_sprite(STG_KIND_RICE20 + 1u, 2047u) == 16u
       && stg_kind_sprite(STG_KIND_RICE20 + 1u, 2048u) == 17u
       && stg_kind_sprite(STG_KIND_RICE20 + 1u, 30719u) == 23u
       && stg_kind_sprite(STG_KIND_RICE20 + 1u, 30720u) == 24u
       && stg_kind_sprite(STG_KIND_RICE20 + 1u, 63487u) == 31u
       && stg_kind_sprite(STG_KIND_RICE20 + 1u, 63488u) == 16u
       && stg_kind_sprite(STG_KIND_RICE20 + 1u, 65535u) == 16u
       && stg_kind_sprite(STG_KIND_RICE20 + 3u, 2048u) == 49u);
    ck("E3 ROUND16/SMALL8 frame = colour; radii 4/2/4/3/0 px; eq ALPHA for enemy bullets, ADD for pshot/spark; 14, 15 unused",
       stg_kind_sprite(STG_KIND_ROUND16 + 2u, 12345u) == 2u
       && stg_kind_sprite(STG_KIND_SMALL8 + 3u, 40000u) == 3u
       && stg_kind_tab[14].w == 0u && stg_kind_tab[15].w == 0u && stg_kind_tab[15].r_hit == 0
       && stg_kind_tab[STG_KIND_ROUND16 + 3u].r_hit == 4 * FX_ONE && stg_kind_tab[STG_KIND_ROUND16].w == 16u
       && stg_kind_tab[STG_KIND_SMALL8 + 1u].r_hit == 2 * FX_ONE && stg_kind_tab[STG_KIND_SMALL8].w == 8u
       && stg_kind_tab[STG_KIND_RICE20 + 2u].r_hit == 4 * FX_ONE && stg_kind_tab[STG_KIND_RICE20].w == 20u
       && stg_kind_tab[STG_KIND_PSHOT].r_hit == 3 * FX_ONE && stg_kind_tab[STG_KIND_SPARK].r_hit == 0
       && stg_kind_tab[STG_KIND_RICE20 + 3u].eq == GFX_EQ_ALPHA && stg_kind_tab[STG_KIND_PSHOT].eq == GFX_EQ_ADD
       && stg_kind_tab[STG_KIND_SPARK].eq == GFX_EQ_ADD);
}

/* --- 换段与挂满 ----------------------------------------------------------- */

static void e4_segments(void)
{
    uint32_t seg, ok = 1u, i;

    for (seg = 0u; seg < STG_SEG_N; seg++) {
        uint32_t want = (seg == STG_SEG_MIX) ? STG_PAT_N : 1u;

        if (stg_pattern_load(&s_field, seg) != 0 || s_field.n_em != want
            || stg_pool_count(&s_field.pools.ebullet) != 0u)
            ok = 0u;
    }
    ck("E4 six segments load: 1,1,1,1,1,5 emitters, empty pool", ok);
    ck("E4 segment 6 rejected", stg_pattern_load(&s_field, STG_SEG_N) == -1);

    stg_field_init(&s_field, 1u);
    for (i = 0u; i < STG_FIELD_EMIT_MAX; i++)
        (void)stg_field_add_emitter(&s_field, &stg_patterns[0].rec, 0, 0, 0);
    ck("E4 ninth emitter rejected",
       s_field.n_em == STG_FIELD_EMIT_MAX
       && stg_field_add_emitter(&s_field, &stg_patterns[0].rec, 0, 0, 0) == -1);
}

uint32_t test_stg_emit_run(void)
{
    g_fail = 0u;
    e1_reject();
    e2_timer();
    e3_sprite();
    e4_segments();
    return g_fail;
}

/* --- 轨迹与 dump --------------------------------------------------------- */

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t pool_crc(const stg_pool *p)
{
    crc32_ctx c;
    uint32_t  i;

    crc32_begin(&c);
    for (i = 0u; i < stg_pool_count(p); i++) {
        stg_ent e;
        uint8_t b[20];

        (void)stg_pool_get(p, i, &e);
        put_le32(b + 0, (uint32_t)e.x);
        put_le32(b + 4, (uint32_t)e.y);
        put_le32(b + 8, (uint32_t)e.vx);
        put_le32(b + 12, (uint32_t)e.vy);
        b[16] = (uint8_t)e.sprite;
        b[17] = (uint8_t)(e.sprite >> 8);
        b[18] = (uint8_t)e.flags;
        b[19] = (uint8_t)(e.flags >> 8);
        crc32_update(&c, b, 20u);
    }
    return crc32_end(&c);
}

static void print_params(uint32_t seg, const stg_field *f)
{
    uint32_t i;

    printf("seg %" PRIu32 " %s seed=0x%08" PRIx32 " player=%" PRId32 ",%" PRId32 "\n",
           seg, stg_seg_name(seg), STG_PAT_SEED, f->px, f->py);
    for (i = 0u; i < f->n_em; i++) {
        const stg_emit_rec *r = f->em[i].rec;
        const stg_host     *h = &f->host[i];

        printf("emit %" PRIu32 " count=%u dir=%u kind=%u angle0=%u angle_step=%u spin=%u"
               " interval=%u repeat=%u speed=%" PRId32 " speed_step=%" PRId32
               " rand_x=%" PRId32 " rand_speed=%" PRId32 " x=%" PRId32 " y=%" PRId32
               " hvx=%" PRId32 " hx_lo=%" PRId32 " hx_hi=%" PRId32 "\n",
               i, (unsigned)r->count, (unsigned)r->dir_mode, (unsigned)r->kind,
               (unsigned)r->angle0, (unsigned)r->angle_step, (unsigned)r->spin,
               (unsigned)r->interval, (unsigned)r->repeat, r->speed, r->speed_step,
               r->rand_x, r->rand_speed, f->em[i].x, f->em[i].y, h->vx, h->x_lo, h->x_hi);
    }
}

void stg_pat_trace(void)
{
    uint32_t seg, k;

    for (seg = 0u; seg < STG_SEG_N; seg++) {
        if (stg_pattern_load(&s_field, seg) != 0) {
            printf("seg %" PRIu32 " load failed\n", seg);
            continue;
        }
        print_params(seg, &s_field);
        for (k = 0u; k < PAT_FRAMES; k++) {
            stg_field_advance(&s_field);
            printf("f %" PRIu32 " %" PRIu32 " %08" PRIx32 "\n", k,
                   stg_pool_count(&s_field.pools.ebullet), pool_crc(&s_field.pools.ebullet));
        }
        printf("end %" PRIu32 " overflow %" PRIu32 "\n", seg,
               stg_pool_overflow(&s_field.pools.ebullet));
    }
    fflush(stdout);
}

int stg_pat_dump(uint32_t seg, uint32_t frame)
{
    uint32_t k, i;

    if (seg >= STG_SEG_N || frame >= PAT_FRAMES || stg_pattern_load(&s_field, seg) != 0)
        return -1;
    for (k = 0u; k <= frame; k++)
        stg_field_advance(&s_field);

    printf("dump %" PRIu32 " %" PRIu32 " %" PRIu32 "\n", seg, frame,
           stg_pool_count(&s_field.pools.ebullet));
    for (i = 0u; i < stg_pool_count(&s_field.pools.ebullet); i++) {
        stg_ent e;

        (void)stg_pool_get(&s_field.pools.ebullet, i, &e);
        printf("b %" PRIu32 " %" PRId32 " %" PRId32 " %" PRId32 " %" PRId32 " %u %u\n",
               i, e.x, e.y, e.vx, e.vy, (unsigned)e.sprite, (unsigned)e.flags);
    }
    fflush(stdout);
    return 0;
}
