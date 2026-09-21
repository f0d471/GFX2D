/******************************************************************************
* @file main.c: platformTest —— PC 平台层的判据程序
*
* @brief  一次跑完全部场景，不加开关（SW4 的 D5：共享的应用不能用 argv 选模式，
*         板上 `start.S` 是 `call main`）。六个场景按顺序：
*
*           1. cases   十一个整帧用例经软件执行器画进帧缓冲 0，逐帧 CRC 打出来，
*                      由 `check_sw4_pc.sh` 与 `lib/test/golden.txt` 对；
*           2. pace    连续 600 帧空渲染，每一帧的场数增量都必须是 1，
*                      场数与切换数的增量都是 600，间隔落在一场附近；
*           3. drop    人为让一帧耗时 20 毫秒，该帧场数增量 2、切换增量 1；
*           4. live    实时键盘段，每帧打印位图与场号。**这一段要人动手**：
*                      按 F1~F3 看窗口大小、点关闭按钮验第 9 条判据；
*           5. replay  合成一段 30 秒的录像，把录制结果打成文本，再回放两次比对；
*           6. 汇总    fail N 与 done。
*
*         画面一律经命令表产生（SW1-SW9 总览约束 4），内容只依赖帧号与按键，
*         不读时刻，所以同一段录像回放两次的 CRC 必然相同。
*         注错 `INJECT_REPLAY_CLOCK` 把时刻混进画面，两次的序列必须因此不同，
*         用来证明「两次相同」那条判据不是空转。
*
*         判据与取舍见 Engineering/Todo/sw/SW4-PC平台层.md 第 3 节与 1.1 节。
*
*         无窗口模式不改这里的代码：`SDL_VIDEODRIVER=dummy ./platformTest`，
*         跑的是同一个二进制（2.5 节）。
******************************************************************************/

#include <string.h>

#include "board.h"

#include "core/crc32.h"
#include "core/format.h"
#include "core/replay.h"
#include "core/xorshift32.h"
#include "gfx/gfx.h"

/* --- 场景参数。都可以用 EXTRA_CFLAGS 覆盖，默认值是判据里写的那个 --- */
#ifndef PACE_FRAMES
#define PACE_FRAMES     600u        /* 第 3 条：连续 600 帧 */
#endif
#ifndef LIVE_FRAMES
#define LIVE_FRAMES     300u        /* 第 5 条的实时段：5 秒，够按几次键 */
#endif
#ifndef REPLAY_FRAMES
#define REPLAY_FRAMES   1800u       /* 第 6 条：30 秒 x 60 帧 */
#endif
#ifndef REPLAY_SEED
#define REPLAY_SEED     1u
#endif
#ifndef DROP_AT
#define DROP_AT         100u        /* 第 4 条：第几帧注错 */
#endif

/* 20 毫秒折成 100MHz 的周期数，注错用。 */
#define DROP_CYCLES     (BOARD_HZ / 50u)

/* 关键位图：只用 board.h 定义过的十位，录下来的按键是真实可能出现的值。 */
#define KEY_MASK        0x3FFu

#define REC_BYTES       (REPLAY_HEADER_BYTES + REPLAY_RECORD_BYTES * REPLAY_FRAMES)

static uint32_t s_fail;

/* --- 输出 --------------------------------------------------------------- */

static void line_puts(const char *s)
{
    board_puts(s);
}

static void say(const char *s)
{
    board_puts(s);
}

static void put_kv(const char *tag, const char *key, uint32_t v)
{
    char     line[96];
    core_out o;

    core_out_init(&o, line, (uint32_t)sizeof line);
    core_out_str(&o, tag);
    core_out_ch(&o, ' ');
    core_out_str(&o, key);
    core_out_ch(&o, '=');
    core_out_u32(&o, v);
    core_out_str(&o, "\r\n");
    line_puts(line);
}

static void chk(const char *tag, int ok)
{
    char     line[96];
    core_out o;

    core_out_init(&o, line, (uint32_t)sizeof line);
    core_out_str(&o, "chk ");
    core_out_str(&o, tag);
    core_out_str(&o, ok ? " ok\r\n" : " FAIL\r\n");
    line_puts(line);
    if (!ok)
        s_fail++;
}

/* --- 整帧 CRC：逐行取前 width*4 字节，跳过行跨的间隙（D8） --------------- */

static uint32_t frame_crc(const void *base)
{
    const uint8_t *p = (const uint8_t *)base;
    crc32_ctx      ctx;
    uint32_t       y;

    crc32_begin(&ctx);
    for (y = 0u; y < FB_HEIGHT; y++)
        crc32_update(&ctx, p + (y << FB_STRIDE_LOG2), FB_WIDTH * 4u);
    return crc32_end(&ctx);
}

/* 空转 want 个 100MHz 周期。注错要的是「这一帧真的用了 20 毫秒」，
   不调 SDL_Delay：那是宿主的东西，应用看不见。 */
static void busy_cycles(uint32_t want)
{
    uint32_t t0 = board_cycles();

    while ((uint32_t)(board_cycles() - t0) < want)
        ;
}

/* --- 场景 1：十一个整帧用例 --------------------------------------------- */

/* 用例的设备地址落点由 gfx.h 给（0x1000 与 0x20_0000）。
   窗口的 dev_base 取 0x1000，于是用例的帧缓冲正好落在 REGION_FB0 的起点：
   画完直接 present 出来看的就是它本身，纹理落在分区内偏移 0x1F_F000。 */
static void seg_cases(void)
{
    board_mem fb0 = board_region(REGION_FB0);
    gfx_mem   m;
    uint32_t  idx;

    say("cases begin\r\n");

    m.dev_base = GFX_CASE_FB_BASE;
    m.host     = (uint8_t *)fb0.ptr;
    m.bytes    = fb0.size;

    for (idx = 0u; idx < GFX_CASE_N; idx++) {
        gfx_surface_table t;
        gfx_cmd           cmds[2u * GFX_CASE_MAX_CMD];
        exec_sw_state     es;
        exec_sw_stats     st;
        gfx_mem           mm = m;
        uint32_t          n1, n2 = 0u;
        uint32_t          crc;
        char              line[128];
        core_out          o;

        /* 与主机侧测试同一个起点：先把这块窗口清干净再建用例 */
        memset(fb0.ptr, 0, (size_t)(GFX_CASE_DEV_BYTES - GFX_CASE_FB_BASE));

        n1 = gfx_case_build(idx, &t, &mm, GFX_CASE_FB_BASE, GFX_CASE_TEX_BASE, cmds, &n2);

        exec_sw_reset(&es);
        exec_sw_stats_clear(&st);
        (void)exec_sw_run(&es, &t, &mm, cmds, n1, n1, &st, NULL);
        if (n2 > 0u)
            (void)exec_sw_run(&es, &t, &mm, cmds + n1, n2, n2, &st, NULL);

        /* 读回前失效一次（E3）：CRC 要算 DDR 里的字节，不是缓存里的副本 */
        board_dcache_invalidate_all();
        crc = frame_crc(fb0.ptr);

        core_out_init(&o, line, (uint32_t)sizeof line);
        core_out_str(&o, "case ");
        core_out_u32(&o, idx);
        core_out_ch(&o, ' ');
        core_out_str(&o, gfx_case_name(idx));
        core_out_str(&o, " crc ");
        core_out_hex32(&o, crc);
        core_out_str(&o, "\r\n");
        line_puts(line);

        /* 上屏，这一段才看得到（每帧仍要 present：它是场节拍的驱动者） */
        board_present(0);
    }
    say("cases end\r\n");
}

/* --- 场景 2：帧节拍 ----------------------------------------------------- */

static void seg_pace(void)
{
    uint32_t f0, s0, f1, s1, df, ds, bad = 0u;
    uint32_t min_us = 0xFFFFFFFFu, max_us = 0u;
    uint32_t f;

    say("pace begin\r\n");

    /* 先对齐到一场的起点，免得第一帧量到半场 */
    board_wait_frame();
    f0 = board_frame_count();
    s0 = board_swap_count();

    for (f = 0u; f < PACE_FRAMES; f++) {
        uint32_t a = board_frame_count();
        uint32_t c0, c1;

        c0 = board_cycles();
        board_present((int)(f & 1u));
        c1 = board_cycles();

        /* 每一帧恰好等掉一场：这是「present 等到下一个场边界」的直接判据 */
        if ((uint32_t)(board_frame_count() - a) != 1u)
            bad++;

        {
            uint32_t us = (uint32_t)(c1 - c0) / (BOARD_HZ / 1000000u);

            if (us < min_us) min_us = us;
            if (us > max_us) max_us = us;
        }
    }

    f1 = board_frame_count();
    s1 = board_swap_count();
    df  = (uint32_t)(f1 - f0);
    ds  = (uint32_t)(s1 - s0);

    put_kv("pace", "frames", PACE_FRAMES);
    put_kv("pace", "dfield", df);
    put_kv("pace", "dswap", ds);
    put_kv("pace", "min_us", min_us);
    put_kv("pace", "max_us", max_us);
    put_kv("pace", "missed", bad);

    chk("P1 每帧都恰等一场", bad == 0u);
    chk("P2 场数增量等于帧数", df == PACE_FRAMES);
    chk("P3 切换增量等于帧数", ds == PACE_FRAMES);
    chk("P4 没有掉帧", (uint32_t)(df - ds) == 0u);
    /* 挂钟的两条只是数量级检查，**结构性判据是 P1**：
       「立即返回」会让某帧的场数增量变成 0、「等两场」会变成 2，两者都被 P1 抓住，
       而挂钟的尾巴是宿主调度给的，不是本层给的。实测：空载 16.5~16.9 毫秒，
       同时跑一次编译时最坏到过 15.3~18.0 毫秒（SW4 开发日志 4.5）。
       所以下界取 10 毫秒、上界取 25 毫秒：只挡「等两场」（约 33 毫秒）这一档数量级错误。 */
    chk("P5 最短间隔不低于 10 毫秒", min_us >= 10000u);
    chk("P6 最长间隔不超过 25 毫秒", max_us <= 25000u);

    say("pace end\r\n");
}

/* --- 场景 3：掉帧注错 --------------------------------------------------- */

static void seg_drop(void)
{
    uint32_t f0, s0, f1, s1, df, ds;

    say("drop begin\r\n");

    /* 先对齐，再量：不对齐时那 20 毫秒可能跨三场 */
    board_wait_frame();
    f0 = board_frame_count();
    s0 = board_swap_count();

    busy_cycles(DROP_CYCLES);
    board_present(0);

    f1 = board_frame_count();
    s1 = board_swap_count();
    df = (uint32_t)(f1 - f0);
    ds = (uint32_t)(s1 - s0);

    put_kv("drop", "cycles", DROP_CYCLES);
    put_kv("drop", "dfield", df);
    put_kv("drop", "dswap", ds);

    chk("D1 场数增量是 2", df == 2u);
    chk("D2 切换增量是 1", ds == 1u);
    /* 掉帧数由应用做减法，且**先在 32 位上减完**（board.h 的契约） */
    chk("D3 减法算出的掉帧数是 1", (uint32_t)(df - ds) == 1u);

    say("drop end\r\n");
}

/* --- 场景 4：实时输入与退出 --------------------------------------------- */

static void seg_live(void)
{
    uint32_t f;
    int      quit_seen = 0;

    say("live begin（按 F1~F3 看窗口大小、点关闭按钮验退出）\r\n");

    for (f = 0u; f < LIVE_FRAMES; f++) {
        uint32_t keys = board_keys();
        uint32_t a    = board_frame_count();
        uint32_t d;
        char     line[128];
        core_out o;

        board_present((int)(f & 1u));
        d = (uint32_t)(board_frame_count() - a);

        core_out_init(&o, line, (uint32_t)sizeof line);
        core_out_str(&o, "live ");
        core_out_u32(&o, f);
        core_out_str(&o, " field ");
        core_out_u32(&o, board_frame_count());
        core_out_str(&o, " df ");
        core_out_u32(&o, d);
        core_out_str(&o, " keys ");
        core_out_hex32(&o, keys);
        core_out_str(&o, "\r\n");
        line_puts(line);

        if (board_should_quit()) {
            quit_seen = 1;
            put_kv("live", "quit_frame", f);
            put_kv("live", "quit_df", d);
            /* 关窗之后不再等场边界：看到退出的这一帧，场数增量必须是 0 */
            chk("L1 关窗当帧没有等场", d == 0u);
            break;
        }
    }

    if (!quit_seen) {
        put_kv("live", "frames", LIVE_FRAMES);
        say("live 没有关窗（退出那一条要人点窗口的关闭按钮）\r\n");
    }
    say("live end\r\n");
}

/* --- 场景 5：录制与两次回放 --------------------------------------------- */

static uint32_t          s_keys[REPLAY_FRAMES];
static uint32_t          s_crc1[REPLAY_FRAMES];
static uint32_t          s_crc2[REPLAY_FRAMES];
static uint8_t           s_rec[REC_BYTES];

/* 画一帧：全屏铺一层底色，再按按键位图放一个方块。
   内容只依赖帧号与按键，所以同一段录像回放两次必然逐帧相同；
   注错版把 board_cycles() 混进底色，两次必然出现不同。 */
static uint32_t draw_frame(int buf, uint32_t frame, uint32_t keys)
{
    board_mem        reg = board_region(buf == 0 ? REGION_FB0 : REGION_FB1);
    gfx_mem          m;
    gfx_surface_table t;
    gfx_surface      s;
    gfx_cmd          cmds[4];
    gfx_list         l;
    exec_sw_state    es;
    exec_sw_stats    st;
    gfx_rect         full;
    gfx_rect         box;
    uint32_t         color;
    uint32_t         count;

    m.dev_base = 0u;
    m.host     = (uint8_t *)reg.ptr;
    m.bytes    = reg.size;

    s.base        = 0u;
    s.width       = (uint16_t)FB_WIDTH;
    s.height      = (uint16_t)FB_HEIGHT;
    s.stride_log2 = (uint8_t)FB_STRIDE_LOG2;
    s.space       = GFX_SPACE_DDR;
    s.format      = GFX_FMT_XRGB8888;

    gfx_surface_reset(&t);
    (void)gfx_surface_set(&t, 0, &s);

    full.x0 = 0;
    full.y0 = 0;
    full.x1 = (int32_t)FB_WIDTH - 1;
    full.y1 = (int32_t)FB_HEIGHT - 1;

    color = 0x10u + (((frame * 2654435761u) >> 24) & 0x7Fu);
#ifdef INJECT_REPLAY_CLOCK
    /* 注错：让画面依赖当前时刻。两次回放的 CRC 序列必须因此出现不同。 */
    color ^= (board_cycles() & 0x7Fu);
#endif

    gfx_list_begin(&l, cmds, 4u, &t);
    (void)gfx_fill(&l, 0, full, 0xFF000000u | (color << 16) | (color << 8) | color,
                   GFX_EQ_COPY, 0u);

    box.x0 = (int32_t)(keys & 0x3Fu) * 8;
    box.y0 = (int32_t)((keys >> 6) & 0x1Fu) * 8;
    box.x1 = box.x0 + 47;
    box.y1 = box.y0 + 47;
    (void)gfx_fill(&l, 0, box, 0xFF000000u | (0x40u << 16) | (0x80u << 8) | 0xC0u,
                   GFX_EQ_ADD, 0u);
    (void)gfx_list_end(&l);
    count = l.used;

    exec_sw_reset(&es);
    exec_sw_stats_clear(&st);
    (void)exec_sw_run(&es, &t, &m, cmds, count, count, &st, NULL);

    board_dcache_invalidate_all();
    return frame_crc(reg.ptr);
}

/* 回放一遍，把每帧的 CRC 写进 crc_out，并把按键列打出来（第 5 条：
   回放段不读键盘，所以这一列必须与录像里的按键序列逐帧相同）。
   返回真正跑完的帧数：中途关窗会少跑，比对那一步要拿它当条件，
   否则两串没跑完的数组互比会得出「相同」这个假绿。 */
static uint32_t play_pass(const char *tag, uint32_t *crc_out)
{
    replay_view v;
    uint32_t    f;
    uint32_t    mismatch = 0u;

    if (replay_open(s_rec, REC_BYTES, &v) != 0) {
        chk("R2 录像打不开", 0);
        return 0u;
    }

    for (f = 0u; f < v.frames; f++) {
        uint32_t k = 0u;
        uint32_t crc;
        char     line[160];
        core_out o;

        (void)replay_key_at(&v, f, &k);
        if (k != s_keys[f])
            mismatch++;
        if (board_should_quit())
            break;

        crc = draw_frame((int)(f & 1u), f, k);
        crc_out[f] = crc;

        core_out_init(&o, line, (uint32_t)sizeof line);
        core_out_str(&o, tag);
        core_out_ch(&o, ' ');
        core_out_u32(&o, f);
        core_out_str(&o, " crc ");
        core_out_hex32(&o, crc);
        core_out_str(&o, " keys ");
        core_out_hex32(&o, k);
        core_out_str(&o, "\r\n");
        line_puts(line);

        board_present((int)(f & 1u));
    }
    chk(tag, mismatch == 0u);
    return f;
}

static void seg_replay(void)
{
    replay_rec r;
    replay_view v;
    xs32       g;
    uint32_t   i;
    uint32_t   len;
    uint32_t   n1, n2;
    uint32_t   diff = 0u;
    char       line[64];

    say("replay begin\r\n");

    /* 1. 合成一段录像。按键用 lib/core 的 xorshift32，与板上同一个算法。 */
    replay_rec_begin(&r, s_rec, (uint32_t)sizeof s_rec, REPLAY_SEED);
    xs32_seed(&g, REPLAY_SEED);
    for (i = 0u; i < REPLAY_FRAMES; i++) {
        s_keys[i] = xs32_next(&g) & KEY_MASK;
        if (replay_rec_put(&r, s_keys[i]) != 0) {
            chk("R1 录像缓冲不够", 0);
            break;
        }
    }
    len = replay_rec_end(&r);
    chk("R1 录到的帧数", replay_rec_frames(&r) == REPLAY_FRAMES);
    chk("R1 录像长度", replay_open(s_rec, len, &v) == 0 && v.frames == REPLAY_FRAMES);

    /* 2. 录制结果就是日志：按 replay.h 定的两种行打出来，
          Main/tools/check/replay2c.py 能把这段文本收回同一份字节。
          再回放，所以这一段只有一组 g2dr/key 行。 */
    (void)replay_text_header(v.seed, v.frames, line, (uint32_t)sizeof line);
    line_puts(line);
    for (i = 0u; i < v.frames; i++) {
        uint32_t k = 0u;

        (void)replay_key_at(&v, i, &k);
        (void)replay_text_key(i, k, line, (uint32_t)sizeof line);
        line_puts(line);
    }

    /* 3. 同一段录像回放两次（30 秒 x 2），逐帧比对 CRC */
    n1 = play_pass("play1", s_crc1);
    n2 = play_pass("play2", s_crc2);

    put_kv("replay", "frames", REPLAY_FRAMES);
    put_kv("replay", "played", (n1 < n2) ? n1 : n2);

    if (n1 != REPLAY_FRAMES || n2 != REPLAY_FRAMES) {
        /* 中途关窗会少跑几帧。两串没跑完的数组互比会得出「相同」这个假绿，
           所以不跑满就不比，直接报红。 */
        chk("R3 两次回放都跑满", 0);
    } else {
        for (i = 0u; i < REPLAY_FRAMES; i++) {
            if (s_crc1[i] != s_crc2[i])
                diff++;
        }
        put_kv("replay", "diff", diff);
#ifdef INJECT_REPLAY_CLOCK
        /* 注错版：画面依赖时刻，两次必须不同 */
        chk("R3 注错后两次回放必须不同", diff > 0u);
#else
        chk("R3 两次回放逐帧相同", diff == 0u);
#endif
    }

    say("replay end\r\n");
}

/* --- 主程序 ------------------------------------------------------------- */

int main(void)
{
    board_init();
    crc32_init();

    line_puts("\r\nplatformTest SW4\r\n");

    /* 关窗之后剩下的场景跑不了（present 与 wait 都立即返回，帧节拍没有了），
       所以一段一段看：谁把窗口关了，后面的就明说没跑，不留半截结果当结论。 */
    seg_cases();
    if (!board_should_quit()) {
        seg_pace();
    }
    if (!board_should_quit()) {
        seg_drop();
    }
    if (!board_should_quit()) {
        seg_live();
    }
    if (board_should_quit()) {
        put_kv("quit", "at_frame", board_frame_count());
        say("quit：关窗之后剩下的场景没有跑\r\n");
    } else {
        seg_replay();
    }

    put_kv("fail", "", s_fail);
    line_puts("done\r\n");
    return (s_fail == 0u) ? 0 : 1;
}
