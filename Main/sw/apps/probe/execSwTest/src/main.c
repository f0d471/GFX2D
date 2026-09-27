/******************************************************************************
*
* @file main.c: execSwTest —— E7 的板上专项探针（前期研究版）
*
* @brief  E7 要的是「软件执行器各方程在板上的分方程像素吞吐」。本程序是它的
*         第一版可运行骨架，先回答四个问题，数字进 E7 的开发日志：
*
*         1. VERIFY。SW3 的十一个整帧用例在板上跑一遍，整帧 CRC32 与
*            `lib/test/golden.txt` 逐行比。**这一条不过，后面所有数字都没有意义**：
*            吞吐是拿一份没被证明正确的执行器量出来的。同时它把
*            「板上执行器 == PC 执行器」这条判据钉死在同一个产物上。
*         2. THR。逐方程量吞吐：每个用例在 FB0 与 FB1 上各测，
*            只把 `exec_sw_run()` 包在计时区间里，命令构造、CRC、串口都在区间外。
*            每轮独立打点（`board_cycles()` 差），多轮取最小并打出离差，
*            这样「写 FB0 比写 FB1 慢」这类目的面差异不会被平均掉（E7 2.4 节）。
*         3. CMP。S1 同类的混合命令表（整屏 FILL + HUD BLIT + 2000 个 12x12 色键 BLIT），
*            量「整表的每像素成本」与单项负载的差，给折帧用。
*         4. OVH。纯 NOP 的命令表：量命令表遍历与记录校验的固定开销，
*            与逐像素开销分开，S1 整表预算里要用它。
*
*         2026-09-27 已完成注错判分收尾；E7_VERIFY_ONLY 跳过吞吐段。
*         生产吞吐、折帧预算与产物身份已归档；`-Os` 与四像素展开保留为可选优化。
*         口径、判据与切分见 `Engineering/TechReports/platform/09-软件执行器像素吞吐的板上复现.md`。
*
*         计时口径：`board_cycles()` 是 CLINT 低 32 位、1 tick = 10ns、42.9 秒回绕
*         （bsp/include/board.h）。本程序每轮独立取两次读数相减（都在 42.9 秒内）。
*
*         输出末尾是 `fail <n>` 与 `done`，与其余判据程序同一约定。
*
******************************************************************************/

#include <stdint.h>

#include "board.h"
#include "memmap.h"

#include "core/crc32.h"
#include "core/format.h"

#include "gfx/gfx.h"

/* --- 内存布局（都在 memmap.h 的分区之内，指针由 board_region 给出）---------
   纹理放在离屏层的尾部，与帧缓冲、命令表都不重叠。
   命令表用分区 A 的开头即可：一条命令 32 字节，本程序的表最多约 2100 条。 */
#define OFF_BYTES       (8u * 1024u * 1024u)
#define TEX_CMP_OFF     (OFF_BYTES - 0x00400000u)   /* 离屏层 +4MB：S1 子集预乘色键弹纹理 */
#define TEX_ST_OFF      (OFF_BYTES - 0x00300000u)   /* 离屏层 +5MB：512x512 直通 alpha */
#define TEX_SRC_OFF     (OFF_BYTES - 0x00200000u)   /* 离屏层 +6MB：512x512 预乘 */
#define TEX_KEY_OFF     (OFF_BYTES - 0x00100000u)   /* 离屏层 +7MB：512x512 带色键 */

#define CMD_BYTES       0x20000u

/* 注错收尾只跑 VERIFY，生产构建保留完整吞吐测量。 */
#ifndef E7_VERIFY_ONLY
#define E7_VERIFY_ONLY 0
#endif

/* 色键字：与 SW3 的 cases.c 取同一个值，便于与 golden 对照。 */
#define KEY_COLOR       0x00123456u

/* 源纹理的尺寸。**目的窗口不能超过源**：`dst_window()` 会把窗口的右边与下边
   夹到源表面的宽高（exec_sw.c 第 135、140 行），源比窗口小时多出来的像素
   在逐像素循环里按 `src_oob` 跳过，量到的就不是该方程的成本。
   四个方程因此都画同样大的窗口（384x256 = 98304），源与窗口同尺寸、`src_oob` 必须为 0。
   带色键的那一块也做成 512x512：色键要与其余方程同窗口才可比。
   **命令表要给足**：命令表分区 A 有 1MB（32768 条），这里取 128KB。
   取小了混合整表用例会被 `GFX_BUILD_FULL` 截断，量到的是前 255 条命令的成本。 */
#define SRC_W   512
#define SRC_H   512
#define SRC_SL  11u                 /* 512 像素 x 4 字节 = 2048 字节/行 = 2^11 */

#define KEY_W   512
#define KEY_H   512
#define KEY_SL  11u

#define WIN_W   384
#define WIN_H   256
#define WIN_PIX ((uint32_t)WIN_W * (uint32_t)WIN_H)

/* --- 负载矩阵：四个方程 + 一个无比较基准，形状与像素数完全一致 ------------- */

/* 采样轮数。§2.4 要求「每项至少跑 10 帧或累计 100 万个 pixel_total，取较大者」：
   单项负载是 98,304 像素，11 轮 = 1,081,344 > 100 万，所以单项取 11 轮；
   整表 `cmp-s1` 是 693,504 像素/轮，2 轮就超过 100 万，取 10 轮留足余量
   （10 轮约 8.2 秒，串口与 CRC 都在区间外）。
   OVH 不按像素计，取 10 轮。 */
#define ROUNDS       4u         /* 逐轮值打印的轮数（看第一轮冷启动） */
#define ROUNDS_LOAD  11u
#define ROUNDS_CMP   10u
#define ROUNDS_OVH   10u

typedef struct {
    const char *name;
    uint32_t    eq;
    uint32_t    key_en;
    uint32_t    src;        /* 1 = BLIT，0 = FILL */
    int32_t     dx, dy;
    int32_t     w, h;
    uint32_t    argb;
    uint8_t     c;
    uint32_t    pix;
} load_t;

static const load_t g_loads[] = {
    { "fill-copy",     GFX_EQ_COPY,           0u, 0u,  80,  60, WIN_W, WIN_H, 0x00204060u, 0u, WIN_PIX },
    { "fill-const",    GFX_EQ_CONST,          0u, 0u,  80,  60, WIN_W, WIN_H, 0x00010101u, 1u, WIN_PIX },
    { "blit-copy",     GFX_EQ_COPY,           0u, 1u, 128,  96, WIN_W, WIN_H, 0u,          0u, WIN_PIX },
    { "blit-key",      GFX_EQ_COPY,           1u, 1u,  64,  64, WIN_W, WIN_H, 0u,          0u, WIN_PIX },
    { "blit-alpha-pm", GFX_EQ_ALPHA,          0u, 1u, 128,  96, WIN_W, WIN_H, 0u,          0u, WIN_PIX },
    { "blit-alpha-st", GFX_EQ_ALPHA_STRAIGHT, 0u, 1u, 128,  96, WIN_W, WIN_H, 0u,          0u, WIN_PIX },
    { "blit-add",      GFX_EQ_ADD,            0u, 1u, 128,  96, WIN_W, WIN_H, 0u,          0u, WIN_PIX },
    { "blit-mul",      GFX_EQ_MUL,            0u, 1u, 128,  96, WIN_W, WIN_H, 0u,          0u, WIN_PIX }
};

#define LOAD_N (sizeof g_loads / sizeof g_loads[0])

/* --- 全局内存对象 --------------------------------------------------------- */

/* VERIFY 段与性能段都要这几样：表面表、内存窗口、命令表、目的面与色键纹理。 */
static gfx_surface_table s_tab;
static gfx_mem           s_mem;
static gfx_cmd          *s_cmds;
static uint32_t          s_cmd_cap = CMD_BYTES / 32u;
static uint32_t          s_fb_dev[2];
static uint32_t          s_tex_key;

static uint32_t          s_tex_src;
static uint32_t          s_tex_st;
static uint32_t          s_tex_cmp;

static exec_sw_state     s_es;
static exec_sw_error     s_err;

static int s_fail;

/* --- 输出 ---------------------------------------------------------------- */

/* S1 汇总行含两种校验值，最长约 220 字节；留余量避免串口行截断。 */
static char     s_line[320];
static core_out s_out;

static void out_begin(void)
{
    core_out_init(&s_out, s_line, (uint32_t)sizeof s_line);
}

static void out_end(void)
{
    core_out_str(&s_out, "\r\n");
    board_puts(s_line);
}

static void say(const char *s)
{
    out_begin();
    core_out_str(&s_out, s);
    out_end();
}

static void f_num(uint32_t v)
{
    core_out_u32(&s_out, v);
}

static void f_label(const char *name, uint32_t v)
{
    core_out_str(&s_out, name);
    core_out_u32(&s_out, v);
}

/* 周期数以「千周期」打印，读表时不必数零。 */
static void f_kcyc(uint64_t cycles)
{
    core_out_u32(&s_out, (uint32_t)(cycles / 1000u));
}

/* --- 内存与表面 ----------------------------------------------------------- */

/* 分区指针与分区表偏移的一致性：gfx_mem 的窗口是「host 指针对应的设备地址」，
   两者错位时整程序算出来的都是假数据，先核对再跑（assetTest 的 chk M1 同款）。 */
static int mem_window(void)
{
    const board_mem asset = board_region(REGION_ASSET);
    const board_mem fb0   = board_region(REGION_FB0);
    const board_mem fb1   = board_region(REGION_FB1);
    const board_mem off   = board_region(REGION_OFFSCREEN);
    const uint32_t  asset_dev = g_mem_regions[REGION_ASSET].offset;

    if ((uint32_t)((uintptr_t)fb0.ptr - (uintptr_t)asset.ptr)
            != g_mem_regions[REGION_FB0].offset - asset_dev)
        return -1;
    if ((uint32_t)((uintptr_t)fb1.ptr - (uintptr_t)asset.ptr)
            != g_mem_regions[REGION_FB1].offset - asset_dev)
        return -1;
    if ((uint32_t)((uintptr_t)off.ptr - (uintptr_t)asset.ptr)
            != g_mem_regions[REGION_OFFSCREEN].offset - asset_dev)
        return -1;

    s_fb_dev[0] = g_mem_regions[REGION_FB0].offset;
    s_fb_dev[1] = g_mem_regions[REGION_FB1].offset;
    s_tex_src   = g_mem_regions[REGION_OFFSCREEN].offset + TEX_SRC_OFF;
    s_tex_st    = g_mem_regions[REGION_OFFSCREEN].offset + TEX_ST_OFF;
    s_tex_key   = g_mem_regions[REGION_OFFSCREEN].offset + TEX_KEY_OFF;
    s_tex_cmp   = g_mem_regions[REGION_OFFSCREEN].offset + TEX_CMP_OFF;

    /* 窗口盖住素材区起点到离屏层尾部这一段（与 gfx2dGame 同一算法）。 */
    s_mem.dev_base = asset_dev;
    s_mem.host     = (uint8_t *)asset.ptr;
    s_mem.bytes    = (g_mem_regions[REGION_OFFSCREEN].offset + OFF_BYTES) - asset_dev;
    return 0;
}

static int surf_set(gfx_surface_table *t, int id, uint32_t base,
                    uint32_t w, uint32_t h, uint32_t stride_log2, uint32_t fmt)
{
    gfx_surface s;

    s.base        = base;
    s.width       = (uint16_t)w;
    s.height      = (uint16_t)h;
    s.stride_log2 = (uint8_t)stride_log2;
    s.space       = (uint8_t)GFX_SPACE_DDR;
    s.format      = (uint8_t)fmt;
    return gfx_surface_set(t, id, &s);
}

/* 往设备地址写一个字。越界与未对齐直接丢弃：本程序的所有地址都是自己算的。 */
static void poke(uint32_t dev, uint32_t v)
{
    uint32_t off;

    if (dev < s_mem.dev_base)
        return;
    off = dev - s_mem.dev_base;
    if (off + 4u > s_mem.bytes || (off & 3u) != 0u)
        return;
    *(uint32_t *)(void *)(s_mem.host + off) = v;
}

/* 纹理内容：格式随方程，让每个方程都作用在「有变化、会饱和、有半透明」的数据上。
   PREMULT 的每个通道不超过 alpha（规范条款 §2 的素材契约）。
   带色键时让一半像素（8x8 棋盘格）恒为 KEY_COLOR，命中率因此是确定的一半。 */
static void seed_tex(uint32_t base, uint32_t w, uint32_t h, uint32_t stride_log2,
                     uint32_t fmt, int with_key)
{
    uint32_t x, y;

    for (y = 0u; y < h; y++) {
        for (x = 0u; x < w; x++) {
            const uint32_t a  = (x * 5u) & 0xFFu;
            const uint32_t rr = (x * 7u + y * 3u) & 0xFFu;
            const uint32_t gg = (y * 5u + 17u) & 0xFFu;
            const uint32_t bb = (x + y * 2u) & 0xFFu;
            uint32_t v;

            if (fmt == GFX_FMT_ARGB8888_PM) {
                v = (a << 24) | (((a * rr) / 255u) << 16)
                  | (((a * gg) / 255u) << 8) | ((a * bb) / 255u);
            } else if (fmt == GFX_FMT_ARGB8888_ST) {
                v = (a << 24) | (rr << 16) | (gg << 8) | bb;
            } else {
                v = (rr << 16) | (gg << 8) | bb;
                if (with_key && (((x >> 2) + (y >> 2)) & 1u) != 0u)
                    v = KEY_COLOR;
            }
            poke(base + (y << stride_log2) + (x << 2), v);
        }
    }
}

/* CMP 敌弹纹理：非键区有 64..255 的 alpha 且 RGB 已预乘；键区透明黑。
   12x12 原点窗口内恰有 64 个键像素。 */
static void seed_cmp_tex(void)
{
    uint32_t x, y;

    for (y = 0u; y < SRC_H; y++) {
        for (x = 0u; x < SRC_W; x++) {
            const uint32_t a = 64u + ((x * 5u + y * 3u) % 192u);
            const uint32_t r = (x * 7u + y * 3u) & 0xFFu;
            const uint32_t g = (y * 5u + 17u) & 0xFFu;
            const uint32_t b = (x + y * 2u) & 0xFFu;
            uint32_t v = (a << 24) | (((a * r) / 255u) << 16)
                       | (((a * g) / 255u) << 8) | ((a * b) / 255u);

            if ((((x >> 2) + (y >> 2)) & 1u) != 0u)
                v = 0u;
            poke(s_tex_cmp + (y << SRC_SL) + (x << 2), v);
        }
    }
}

/* 帧缓冲内容：与坐标有关的渐变。性能负载不需要它的具体值，但写一遍是必须的：
   不初始化时 DDR 里的残留会让读改写型方程（ALPHA/ADD）的数字依赖上电状态。
   注意**同一块内存只按一种格式铺一次**：本构型没有写缓冲（platform 第 05 章
   T3/T6），写返回时已落到 DDR，但已装行的读会拿到旧值，先后铺两种格式就会
   混成一份不存在的输入。 */
static void seed_fb(uint32_t base)
{
    uint32_t x, y;

    for (y = 0u; y < FB_HEIGHT; y++) {
        for (x = 0u; x < FB_WIDTH; x++) {
            const uint32_t v = (((x + y) & 0xFFu) << 16)
                             | (((x * 3u) & 0xFFu) <<  8)
                             |  ((y * 5u) & 0xFFu);
            poke(base + (y << FB_STRIDE_LOG2) + (x << 2), v);
        }
    }
}

/* --- 1 VERIFY：十一个整帧用例对 golden.txt --------------------------------- */

/* SW3 的期望值（sw/lib/test/golden.txt）。写成表而不是读文件：板上没有文件系统。 */
static const char *const g_case_name[GFX_CASE_N] = {
    "fill-screen", "blit-copy", "blit-key", "blit-alpha-pm",
    "blit-alpha-straight", "blit-add", "fill-const", "blit-mul",
    "blit-clip-partial", "set-clip-state", "copy-argb-high-byte"
};

static const uint32_t g_case_crc[GFX_CASE_N] = {
    0x9cd34cb7u, 0xbc9be6ffu, 0x084d6bb2u, 0x0b14ce03u,
    0x5b4feb8du, 0xcfcef0eeu, 0xf50e8644u, 0x46667f89u,
    0x0fd9dcafu, 0xa07324eeu, 0x8843eb2fu
};

/* 每个用例实际走的是哪个方程。写得死板但守恒：**这张表与 cases.c 的 build_one
   一一对应**，用例改了这里要一起改。它只服务于「注错见红」判据——
   打开某一个方程的注错宏时，用到该方程的用例 CRC 必须变，其余必须不变。
      0 fill-screen           FILL COPY
      1 blit-copy             BLIT COPY
      2 blit-key              BLIT COPY（色键）
      3 blit-alpha-pm         BLIT ALPHA（预乘）
      4 blit-alpha-straight   BLIT ALPHA_STRAIGHT
      5 blit-add              FILL COPY + BLIT ADD
      6 fill-const            FILL CONST
      7 blit-mul              BLIT MUL
      8 blit-clip-partial     BLIT COPY
      9 set-clip-state        FILL COPY（两张表）
     10 copy-argb-high-byte   BLIT COPY（源是预乘）
   ADD 那一档是两条命令，第一条也走 FILL COPY：打开 COPY 注错时用例 5 也会变，
   打开 ADD 注错时只有它变。判据里按「用到该方程的用例」列，两种都算。 */
#ifdef E7_INJECT_EQ
static const uint8_t g_case_eq[GFX_CASE_N] = {
    GFX_EQ_COPY, GFX_EQ_COPY, GFX_EQ_COPY, GFX_EQ_ALPHA, GFX_EQ_ALPHA_STRAIGHT,
    GFX_EQ_ADD,  GFX_EQ_CONST, GFX_EQ_MUL, GFX_EQ_COPY, GFX_EQ_COPY,
    GFX_EQ_COPY
};
#endif

/* 整帧 CRC：只喂 640 个像素，跳过行跨里的间隙（assetTest 的同一口径）。 */
static uint32_t fb_crc(uint32_t dev)
{
    crc32_ctx ctx;
    uint32_t  y;

    crc32_begin(&ctx);
    for (y = 0u; y < FB_HEIGHT; y++) {
        const void *row = (const void *)(uintptr_t)(dev + (y << FB_STRIDE_LOG2));
        crc32_update(&ctx, row, FB_WIDTH * 4u);
    }
    return crc32_end(&ctx);
}

/* 第二种逐像素校验：按可见区域的 32 位像素字逐一求和（模 2^32）。
   与 CRC32 独立，行跨中的填充字节不参与。 */
static uint32_t fb_pxsum(uint32_t dev)
{
    uint32_t y, x, sum = 0u;
    for (y = 0u; y < FB_HEIGHT; y++) {
        const uint32_t *row = (const uint32_t *)(uintptr_t)(dev + (y << FB_STRIDE_LOG2));
        for (x = 0u; x < FB_WIDTH; x++)
            sum += row[x];
    }
    return sum;
}

/* 窗口 CRC：只喂窗口内的 w 个像素。整帧对不上时用它判断差在窗口内还是窗口外。 */
static uint32_t win_crc(uint32_t dev, int32_t x0, int32_t y0, int32_t w, int32_t h)
{
    crc32_ctx ctx;
    int32_t   y;

    crc32_begin(&ctx);
    for (y = y0; y < y0 + h; y++) {
        const void *row = (const void *)(uintptr_t)
                          (dev + ((uint32_t)y << FB_STRIDE_LOG2) + ((uint32_t)x0 << 2));
        crc32_update(&ctx, row, (uint32_t)w * 4u);
    }
    return crc32_end(&ctx);
}

static void run_verify(void)
{
    const uint32_t fb  = s_fb_dev[0];
    const uint32_t tex = s_tex_key;
    uint32_t       idx;

    say("VERIFY 十一个整帧用例 vs golden.txt");

    for (idx = 0u; idx < GFX_CASE_N; idx++) {
        uint32_t n1, n2 = 0u, got, rc = GFX_OK;
        uint32_t bad = 0u;
        exec_sw_stats st;

        exec_sw_stats_clear(&st);
        n1 = gfx_case_build(idx, &s_tab, &s_mem, fb, tex, s_cmds, &n2);
        if (n1 == 0u)
            bad++;
        if (n1 + n2 > s_cmd_cap)
            bad++;

        exec_sw_reset(&s_es);
        rc = (uint32_t)exec_sw_run(&s_es, &s_tab, &s_mem, s_cmds, n1, n1, &st, &s_err);
        if (rc != GFX_OK)
            bad++;
        if (n2 != 0u) {
            rc = (uint32_t)exec_sw_run(&s_es, &s_tab, &s_mem, s_cmds + n1, n2, n2,
                                       &st, &s_err);
            if (rc != GFX_OK)
                bad++;
        }

        got = fb_crc(fb);

#if defined(E7_INJECT_EQ)
        /* 注错版（`make INJECT=EQ_ALPHA` 等）：**期望的就是「该变的变」**。
           用到该方程的用例 CRC 必须与 golden 不同，其余必须相同。
           这一条比「CRC 相等」更能证明 CRC 盖住的确实是像素结果：
           如果像素核心被优化掉、或 CRC 只盖住了命令表的解析结果，
           打开注错宏时不会有任何用例变化，这里就会 FAIL。 */
        {
            int uses_eq = (g_case_eq[idx] == (uint32_t)E7_INJECT_EQ);

            /* 用例 5 的第一条命令是 FILL COPY，COPY 注错时它也会变。 */
            if (idx == 5u && (uint32_t)E7_INJECT_EQ == (uint32_t)GFX_EQ_COPY)
                uses_eq = 1;
            {
                const int changed = (got != g_case_crc[idx]);
                const int ok = (changed == uses_eq);

                if (!ok)
                    bad++;
                out_begin();
                f_num(idx);
                core_out_ch(&s_out, ' ');
                core_out_str(&s_out, g_case_name[idx]);
                core_out_str(&s_out, " crc 0x");
                core_out_hex32(&s_out, got);
                core_out_str(&s_out, changed ? " CHANGED" : " same");
                core_out_str(&s_out, ok ? " ok" : " FAIL");
                out_end();
            }
            /* 判据「注错见红」：该变的变、不该变的不变，**两边都符合才算 ok**。
               所以这里的 bad 是「注错行为不符合预期」的处数，不是「CRC 变了」。 */
            if (bad != 0u)
                s_fail += bad;
            continue;
        }
#else
        if (got != g_case_crc[idx])
            bad++;
#endif

        out_begin();
        f_num(idx);
        core_out_ch(&s_out, ' ');
        core_out_str(&s_out, g_case_name[idx]);
        core_out_str(&s_out, " crc 0x");
        core_out_hex32(&s_out, got);
        if (bad == 0u) {
            core_out_str(&s_out, " ok");
        } else {
            s_fail += bad;
            core_out_str(&s_out, " FAIL");
            f_label(" rc ", rc);
            f_label(" cmd ", n1 + n2);
            f_label(" pix ", st.pixel_total);
        }
        out_end();
    }
}

/* --- 计时 ---------------------------------------------------------------- */

/* 一轮：只把 exec_sw_run 包在计时区间里。计时区间内不做 CRC、不发串口。
   `first` 为真时先失效一次数据缓存：这只影响**第一轮**——
   实测那一轮比后三轮慢约 2%（装行与 `.bss` 的冷启动），
   而每轮都失效会把稳态数字一起抬上去。E7 报的是稳态，所以只在首轮起点失效一次。 */
static uint32_t run_once(uint32_t n, exec_sw_stats *st, int *rc_out, int first)
{
    uint32_t t0, dt;

    exec_sw_stats_clear(st);
    if (first)
        board_dcache_invalidate_all();
    t0 = board_cycles();
    *rc_out = exec_sw_run(&s_es, &s_tab, &s_mem, s_cmds, n, n, st, &s_err);
    dt = board_cycles() - t0;
    return dt;
}

/* --- 2 THR：分方程吞吐 ----------------------------------------------------- */

/* 按负载表构造一张表：0 号是目的帧缓冲，1 号是源纹理（FILL 用例不登记）。 */
static uint32_t build_load(const load_t *L, uint32_t dst)
{
    const uint32_t fmt = (L->eq == GFX_EQ_ALPHA)          ? GFX_FMT_ARGB8888_PM
                       : (L->eq == GFX_EQ_ALPHA_STRAIGHT) ? GFX_FMT_ARGB8888_ST
                                                          : GFX_FMT_XRGB8888;
    const uint32_t keyed = (L->key_en != 0u) && (L->eq == GFX_EQ_COPY);
    gfx_list l;
    gfx_rect r;

    gfx_surface_reset(&s_tab);
    (void)surf_set(&s_tab, 0, dst, FB_WIDTH, FB_HEIGHT, FB_STRIDE_LOG2, GFX_FMT_XRGB8888);
    if (L->src != 0u) {
        if (keyed)
            (void)surf_set(&s_tab, 1, s_tex_key, KEY_W, KEY_H, KEY_SL, fmt);
        else
            (void)surf_set(&s_tab, 1,
                           L->eq == GFX_EQ_ALPHA_STRAIGHT ? s_tex_st : s_tex_src,
                           SRC_W, SRC_H, SRC_SL, fmt);
    }

    gfx_list_begin(&l, s_cmds, s_cmd_cap, &s_tab);

    r.x0 = L->dx;
    r.y0 = L->dy;
    r.x1 = L->dx + (L->w - 1);
    r.y1 = L->dy + (L->h - 1);

    if (L->src != 0u)
        (void)gfx_blit(&l, 1, 0, 0, 0, (int)L->dx, (int)L->dy, (int)L->w, (int)L->h,
                       (int)L->eq, L->c, (int)L->key_en, KEY_COLOR);
    else
        (void)gfx_fill(&l, 0, r, L->argb, (int)L->eq, L->c);

    (void)gfx_list_end(&l);
    return l.used;
}

/* 一行结果的公共部分：从 best/worst、各轮原始值、均值与统计算出各列。
   `crc` 与 `pxsum` 是这一行负载跑完之后**整帧**的两种校验（计时区间外算），
   由 `Main/tools/check/e7_output_check.py` 独立复算：校验只盖住解析结果
   或者像素核心被优化掉时，这一列会立刻报出来（判据「金标准」与「注错见红」那条）。 */
static void report_row(const char *name, uint32_t fb, uint32_t n,
                       uint32_t best, uint32_t worst, uint64_t sum, uint32_t rounds_n,
                       const uint32_t *rounds, const uint32_t *crcs, uint32_t pxsum,
                       const exec_sw_stats *st, uint32_t bad)
{
    uint32_t i;
    const uint64_t per_px_x100 = (st->pixel_total != 0u)
                               ? ((uint64_t)best * 100u) / st->pixel_total : 0u;
    /* 100MHz / 1e6 = 100；保留两位小数需再乘 100。 */
    const uint32_t mpx_x100    = (best != 0u)
                               ? (uint32_t)(((uint64_t)st->pixel_total * 10000u) / best)
                               : 0u;

    out_begin();
    core_out_str(&s_out, name);
    core_out_str(&s_out, " fb");
    f_num(fb);
    core_out_str(&s_out, " cmd ");
    f_num(n);
    core_out_str(&s_out, " px ");
    f_num(st->pixel_total);
    core_out_str(&s_out, " wr ");
    f_num(st->pixel_written);
    core_out_str(&s_out, " ksk ");
    f_num(st->key_skip);
    core_out_str(&s_out, " oob ");
    f_num(st->src_oob);
    core_out_str(&s_out, " cyc_k ");
    f_kcyc(best);
    core_out_str(&s_out, " us ");
    f_num(best / 100u);
    core_out_str(&s_out, " c/px ");
    f_num((uint32_t)per_px_x100 / 100u);
    core_out_ch(&s_out, '.');
    if ((uint32_t)per_px_x100 % 100u < 10u)
        core_out_ch(&s_out, '0');
    f_num((uint32_t)per_px_x100 % 100u);
    core_out_str(&s_out, " Mpx/s ");
    f_num(mpx_x100 / 100u);
    core_out_ch(&s_out, '.');
    if (mpx_x100 % 100u < 10u)
        core_out_ch(&s_out, '0');
    f_num(mpx_x100 % 100u);
    core_out_str(&s_out, " rounds ");
    f_num(rounds_n);
    core_out_str(&s_out, " mean_k ");
    f_kcyc(sum / rounds_n);
    core_out_str(&s_out, " spread% ");
    f_num((best != 0u) ? (uint32_t)(((uint64_t)worst * 100u) / best) : 0u);
    core_out_str(&s_out, " r1..r4_k ");
    for (i = 0u; i < ROUNDS; i++) {
        if (i != 0u)
            core_out_ch(&s_out, '/');
        f_kcyc(rounds[i]);
    }
    core_out_str(&s_out, " crc 0x");
    core_out_hex32(&s_out, crcs[0]);
    core_out_str(&s_out, " pxsum 0x");
    core_out_hex32(&s_out, pxsum);
    if (crcs[1] != crcs[0]) {
        core_out_str(&s_out, " crc2 0x");
        core_out_hex32(&s_out, crcs[1]);
    }
    if (bad != 0u) {
        core_out_str(&s_out, " FAIL");
    }
    out_end();
}

/* BMP：把每个负载跑完之后**几个固定地址上的像素值**打出来。
   整帧 CRC 只能告诉「对不上」，定位不到是哪一段；这几个点对到
   `Main/tools/check/e7_output_check.py` 的同名打印，能直接看出
   是源读错了、窗口算错了还是混合算错了。
   点是按「源左上角、源中心、目的窗口边界外一格」选的，与负载参数无关的那些
   一律取负载的 dx/dy 与 w/h 推出来的位置。 */
#define BMP_POINTS 6

static void bmp_dump(const char *name, uint32_t fb, const load_t *L)
{
    uint32_t i;
    int32_t  pts[BMP_POINTS][2];

    pts[0][0] = L->dx;             pts[0][1] = L->dy;              /* 窗口左上 */
    pts[1][0] = L->dx + L->w / 2;  pts[1][1] = L->dy + L->h / 2;   /* 窗口中 */
    pts[2][0] = L->dx;             pts[2][1] = L->dy + L->h - 1;   /* 左下 */
    pts[3][0] = L->dx + L->w - 1;  pts[3][1] = L->dy;              /* 右上 */
    pts[4][0] = L->dx - 1;         pts[4][1] = L->dy;              /* 窗口外左 */
    pts[5][0] = L->dx + 1;         pts[5][1] = L->dy + 1;          /* 窗口内右下一格 */

    out_begin();
    core_out_str(&s_out, "BMP ");
    core_out_str(&s_out, name);
    core_out_str(&s_out, " fb");
    f_num(fb);
    core_out_str(&s_out, " wincrc 0x");
    core_out_hex32(&s_out, win_crc(fb, L->dx, L->dy, L->w, L->h));
    for (i = 0u; i < BMP_POINTS; i++) {
        uint32_t v = 0u;

        if (pts[i][0] >= 0 && pts[i][0] < (int32_t)FB_WIDTH
            && pts[i][1] >= 0 && pts[i][1] < (int32_t)FB_HEIGHT)
            v = *(const uint32_t *)(const void *)(uintptr_t)
                (fb + ((uint32_t)pts[i][1] << FB_STRIDE_LOG2)
                    + ((uint32_t)pts[i][0] << 2));
        core_out_ch(&s_out, ' ');
        core_out_u32(&s_out, (uint32_t)pts[i][0]);
        core_out_ch(&s_out, ',');
        core_out_u32(&s_out, (uint32_t)pts[i][1]);
        core_out_ch(&s_out, '=');
        core_out_str(&s_out, "0x");
        core_out_hex32(&s_out, v);
    }
    out_end();
}

/* 跑一个负载：`rounds_n` 轮取最小，核对口径，并在计时外算两遍整帧 CRC
   （两遍必须相同，否则说明这一行负载的输出不稳定）。 */
static void run_load(const load_t *L, uint32_t fb_i)
{
    static uint32_t rounds[ROUNDS_LOAD];
    static uint32_t crcs[2];
    uint32_t pxsums[2];
    uint32_t r, worst = 0u, best = 0xFFFFFFFFu, bad = 0u;
    uint64_t sum = 0u;
    exec_sw_stats best_st;
    uint32_t n;

    n = build_load(L, s_fb_dev[fb_i]);
    if (n == 0u) {
        s_fail++;
        return;
    }

    exec_sw_reset(&s_es);
    exec_sw_stats_clear(&best_st);
    for (r = 0u; r < ROUNDS_LOAD; r++) {
        exec_sw_stats st_r;
        int rc_r = GFX_OK;

        /* 每轮都恢复同一份目的面，构造/铺底仍在计时区间之外。 */
        seed_fb(s_fb_dev[fb_i]);

        rounds[r] = run_once(n, &st_r, &rc_r, r == 0u);
        sum += rounds[r];
        if (rc_r != GFX_OK)
            bad++;
        if (rounds[r] < best) {
            best    = rounds[r];
            best_st = st_r;
        }
        if (rounds[r] > worst)
            worst = rounds[r];
    }

    /* 判据「复现」：同一份输入连跑两遍的整帧 CRC 必须相同（计时区间外）。 */
    {
        int rc2 = GFX_OK;

        seed_fb(s_fb_dev[fb_i]);
        (void)run_once(n, &best_st, &rc2, 0);
        crcs[0] = fb_crc(s_fb_dev[fb_i]);
        pxsums[0] = fb_pxsum(s_fb_dev[fb_i]);
        seed_fb(s_fb_dev[fb_i]);
        (void)run_once(n, &best_st, &rc2, 0);
        crcs[1] = fb_crc(s_fb_dev[fb_i]);
        pxsums[1] = fb_pxsum(s_fb_dev[fb_i]);
        if (rc2 != GFX_OK || crcs[0] != crcs[1] || pxsums[0] != pxsums[1])
            bad++;
    }

    /* 口径核对：对账等式成立、窗口面积与表里写的一致、源不越界
       （越界说明负载构造错了，量到的不是该方程的成本）。 */
    if (best_st.pixel_total
            != best_st.pixel_written + best_st.key_skip + best_st.src_oob)
        bad++;
    if (best_st.pixel_total != L->pix)
        bad++;
    if (best_st.src_oob != 0u)
        bad++;
    if (best_st.pixel_total == 0u)
        bad++;
    /* 同时检查实际纹理与受控棋盘期望，避免 VERIFY 覆盖后的纹理被静默接受。 */
    if (L->key_en != 0u && L->eq == GFX_EQ_COPY) {
        uint32_t sx, sy, nk = 0u;

        for (sy = 0u; sy < (uint32_t)L->h; sy++) {
            const uint32_t off = (s_tex_key - s_mem.dev_base) + (sy << KEY_SL);

            for (sx = 0u; sx < (uint32_t)L->w; sx++) {
                const uint32_t *p = (const uint32_t *)(const void *)
                                    (s_mem.host + off + (sx << 2));

                if (*p == KEY_COLOR)
                    nk++;
            }
        }
        if (best_st.key_skip != nk || nk != L->pix / 2u) {
            bad++;
            out_begin();
            core_out_str(&s_out, "KSK-MISMATCH scan ");
            f_num(nk);
            core_out_str(&s_out, " exec ");
            f_num(best_st.key_skip);
            core_out_str(&s_out, " win ");
            f_num((uint32_t)L->w);
            core_out_ch(&s_out, 'x');
            f_num((uint32_t)L->h);
            core_out_str(&s_out, " srcbase 0x");
            core_out_hex32(&s_out, s_tex_key);
            out_end();
        }
    }

    if (bad != 0u)
        s_fail += bad;
    report_row(L->name, fb_i, n, best, worst, sum, ROUNDS_LOAD, rounds, crcs, pxsums[0],
               &best_st, bad);
    bmp_dump(L->name, s_fb_dev[fb_i], L);
}

static void run_loads(void)
{
    uint32_t fb_i, i;

    say("THR 每方程在 FB0/FB1 各 11 轮取最小（区间只包 exec_sw_run）");
    say("case fb cmd px wr ksk oob cyc_k us c/px Mpx/s rounds mean_k spread% r1..r4_k crc pxsum");

    for (fb_i = 0u; fb_i < 2u; fb_i++)
        for (i = 0u; i < LOAD_N; i++)
            run_load(&g_loads[i], fb_i);
}

/* --- 3 S1：设计文档 4.5 节场景 S1 的精确命令表 ----------------------------- */

/* S1 的一帧（设计文档 4.5 节）：
     整屏 FILL 打底            640x480        307,200 px  COPY
     HUD 底图（这里用色键 BLIT）384x256         98,304 px  COPY + 色键
     敌弹                    2000 x 12x12     288,000 px  ALPHA（预乘）+ 色键
   合计 2003 条命令、693,504 个进入像素循环的像素。
   比例与 4.5 节的表同构（整屏打底 + 一张 HUD + 大量小尺寸半透明弹幕），
   与 4.5 节「约 2320 个 Sprite、约 68 万像素」同量级。
   **弹幕位置由下面的 LCG 现场生成**，不是随机读现成数据：主机侧的
   `Main/tools/check/e7_output_check.py` 用同一段代码复算整帧 CRC。 */
#define S1_N        2000u
#define S1_SPR_W    12
#define S1_SPR_H    12
#define S1_X0       100
#define S1_X1       540
#define S1_Y0       100
#define S1_Y1       340
#define S1_HUD_X    128
#define S1_HUD_Y    96

static uint32_t s1_rng_step(uint32_t *s)
{
    uint32_t v = *s;

    v ^= v << 13;
    v ^= v >> 17;
    v ^= v << 5;
    *s = v;
    return v;
}

static uint32_t build_s1(uint32_t dst, uint32_t *pix_expect, uint32_t *key_expect)
{
    gfx_list l;
    gfx_rect all;
    uint32_t i, rng = 0x12345678u;

    gfx_surface_reset(&s_tab);
    (void)surf_set(&s_tab, 0, dst, FB_WIDTH, FB_HEIGHT, FB_STRIDE_LOG2, GFX_FMT_XRGB8888);
    (void)surf_set(&s_tab, 1, s_tex_key, KEY_W, KEY_H, KEY_SL, GFX_FMT_XRGB8888);
    (void)surf_set(&s_tab, 2, s_tex_cmp, SRC_W, SRC_H, SRC_SL, GFX_FMT_ARGB8888_PM);

    gfx_list_begin(&l, s_cmds, s_cmd_cap, &s_tab);

    all.x0 = 0; all.y0 = 0;
    all.x1 = (int32_t)FB_WIDTH - 1;
    all.y1 = (int32_t)FB_HEIGHT - 1;

    *pix_expect = (uint32_t)FB_WIDTH * (uint32_t)FB_HEIGHT;      /* 整屏打底 */
    *key_expect = 0u;
    (void)gfx_fill(&l, 0, all, 0x00101820u, (int)GFX_EQ_COPY, 0u);

    *pix_expect += WIN_PIX;                                       /* HUD 底图 */
    *key_expect += ((uint32_t)WIN_W / 8u) * ((uint32_t)WIN_H / 8u) * 32u;
    (void)gfx_blit(&l, 1, 0, 0, 0, S1_HUD_X, S1_HUD_Y, WIN_W, WIN_H,
                   (int)GFX_EQ_COPY, 0u, 1, KEY_COLOR);

    *pix_expect += S1_N * S1_SPR_W * S1_SPR_H;
    /* 敌弹都取源原点 12x12：键区固定 4 个 4x4 小格，即每弹 64 像素。 */
    *key_expect += S1_N * 64u;
    for (i = 0u; i < S1_N; i++) {
        int32_t x, y;

        rng = s1_rng_step(&rng);
        x = S1_X0 + (int32_t)(rng % (uint32_t)(S1_X1 - S1_X0));
        rng = s1_rng_step(&rng);
        y = S1_Y0 + (int32_t)(rng % (uint32_t)(S1_Y1 - S1_Y0));

        (void)gfx_blit(&l, 2, 0, 0, 0, x, y, S1_SPR_W, S1_SPR_H,
                       (int)GFX_EQ_ALPHA, 0u, 1, 0u);
    }

    (void)gfx_list_end(&l);
    return l.used;
}

static void run_s1(uint32_t fb_i)
{
    static uint32_t rounds[ROUNDS_CMP];
    static uint32_t crcs[2];
    uint32_t pxsums[2];
    uint32_t r, worst = 0u, best = 0xFFFFFFFFu, bad = 0u;
    uint64_t sum = 0u;
    uint32_t pix_expect = 0u, key_expect = 0u, n;
    exec_sw_stats best_st;

    n = build_s1(s_fb_dev[fb_i], &pix_expect, &key_expect);
    if (n == 0u || n > s_cmd_cap) {
        s_fail++;
        return;
    }

    exec_sw_reset(&s_es);
    exec_sw_stats_clear(&best_st);
    for (r = 0u; r < ROUNDS_CMP; r++) {
        exec_sw_stats st_r;
        int rc_r = GFX_OK;

        seed_fb(s_fb_dev[fb_i]);
        rounds[r] = run_once(n, &st_r, &rc_r, r == 0u);
        sum += rounds[r];
        if (rc_r != GFX_OK)
            bad++;
        if (rounds[r] < best) {
            best    = rounds[r];
            best_st = st_r;
        }
        if (rounds[r] > worst)
            worst = rounds[r];
    }

    {
        int rc2 = GFX_OK;

        seed_fb(s_fb_dev[fb_i]);
        (void)run_once(n, &best_st, &rc2, 0);
        crcs[0] = fb_crc(s_fb_dev[fb_i]);
        pxsums[0] = fb_pxsum(s_fb_dev[fb_i]);
        seed_fb(s_fb_dev[fb_i]);
        (void)run_once(n, &best_st, &rc2, 0);
        crcs[1] = fb_crc(s_fb_dev[fb_i]);
        pxsums[1] = fb_pxsum(s_fb_dev[fb_i]);
        if (rc2 != GFX_OK || crcs[0] != crcs[1] || pxsums[0] != pxsums[1])
            bad++;
    }

    if (best_st.pixel_total
            != best_st.pixel_written + best_st.key_skip + best_st.src_oob)
        bad++;
    if (best_st.pixel_total != pix_expect)
        bad++;
    if (best_st.src_oob != 0u)
        bad++;
    if (best_st.key_skip != key_expect)
        bad++;
    if (best_st.cmd != n - 1u)          /* 除 END 外全部执行完 */
        bad++;

    if (bad != 0u)
        s_fail += bad;
    report_row("s1-exact", fb_i, n, best, worst, sum, ROUNDS_CMP, rounds, crcs, pxsums[0],
               &best_st, bad);
}

/* --- 4 OVH：纯 NOP 表的固定开销 ------------------------------------------- */

static uint32_t build_nop(uint32_t n)
{
    gfx_list l;
    uint32_t i;

    gfx_surface_reset(&s_tab);
    (void)surf_set(&s_tab, 0, s_fb_dev[0], FB_WIDTH, FB_HEIGHT, FB_STRIDE_LOG2,
                   GFX_FMT_XRGB8888);

    gfx_list_begin(&l, s_cmds, s_cmd_cap, &s_tab);
    for (i = 0u; i + 1u < n && i + 1u < s_cmd_cap; i++)
        (void)gfx_nop(&l);
    (void)gfx_list_end(&l);
    return l.used;
}

static void run_ovh(void)
{
    static const uint32_t sizes[3] = { 16u, 256u, 1024u };
    uint32_t i;

    say("OVH 纯 NOP 表的固定开销（每条命令 32 字节，不含末尾 END）");
    say("nop_n cyc_k mean_k rounds spread% cyc_per_cmd_x100");

    for (i = 0u; i < 3u; i++) {
        const uint32_t n = build_nop(sizes[i]);
        uint32_t rounds[ROUNDS_OVH], r, best = 0xFFFFFFFFu, worst = 0u;
        uint64_t sum = 0u;
        exec_sw_stats st;

        if (n < 2u) {
            s_fail++;
            continue;
        }
        exec_sw_reset(&s_es);
        for (r = 0u; r < ROUNDS_OVH; r++) {
            int rc_r = GFX_OK;

            rounds[r] = run_once(n, &st, &rc_r, r == 0u);
            sum += rounds[r];
            if (rc_r != GFX_OK)
                s_fail++;
            if (rounds[r] < best)
                best = rounds[r];
            if (rounds[r] > worst)
                worst = rounds[r];
        }

        out_begin();
        f_num(n - 1u);
        core_out_str(&s_out, " cyc_k ");
        f_kcyc(best);
        core_out_str(&s_out, " mean_k ");
        f_kcyc(sum / ROUNDS_OVH);
        core_out_str(&s_out, " rounds ");
        f_num(ROUNDS_OVH);
        core_out_str(&s_out, " spread% ");
        f_num((best != 0u) ? (uint32_t)(((uint64_t)worst * 100u) / best) : 0u);
        core_out_str(&s_out, " cyc/cmd_x100 ");
        f_num((best * 100u) / (n - 1u));
        out_end();
    }
}

/* --- main ---------------------------------------------------------------- */


int main(void)
{
    s_cmds = (gfx_cmd *)(uintptr_t)g_mem_regions[REGION_CMDLIST_A].offset;

    board_init();
    crc32_init();

    say("");
    say("execSwTest E7 前期研究版");
    out_begin();
    f_label("BOARD_HZ ", BOARD_HZ);
    core_out_str(&s_out, " cmd_cap ");
    f_num(s_cmd_cap);
    out_end();

    if (mem_window() != 0) {
        say("win err 分区指针与分区表不一致");
        say("fail 1");
        say("done");
        return 0;
    }

    /* 扫描输出保持开启：E7 2.4 节要求量的是「扫描在跑」时的吞吐。
       present 自己等掉一场，也把 FB0 设为当前扫描源。 */
    board_present(0);

    /* 一块色键纹理与两个目的面。铺完立刻自检：
       `key[1][1]` 必须等于梯度原值 0x000a1603（按 8x8 棋盘，x=1、y=1 落在
       「不嵌键色」的那半格里）——这是「后面所有数字都建立在正确输入上」的入口判据。 */
    seed_tex(s_tex_key, KEY_W, KEY_H, KEY_SL, GFX_FMT_XRGB8888, 1);
    seed_fb(s_fb_dev[0]);
    seed_fb(s_fb_dev[1]);
    seed_tex(s_tex_src, SRC_W, SRC_H, SRC_SL, GFX_FMT_ARGB8888_PM, 0);
    seed_tex(s_tex_st, SRC_W, SRC_H, SRC_SL, GFX_FMT_ARGB8888_ST, 0);

    {
        crc32_ctx ctx;
        uint32_t  y;
        const uint32_t probe =
            *(const uint32_t *)(const void *)
            (s_mem.host + (s_tex_key - s_mem.dev_base) + (1u << KEY_SL) + 4u);

        crc32_begin(&ctx);
        for (y = 0u; y < KEY_H; y++)
            crc32_update(&ctx,
                         s_mem.host + (s_tex_key - s_mem.dev_base) + (y << KEY_SL),
                         KEY_W * 4u);

        out_begin();
        core_out_str(&s_out, "SEED key[1][1] 0x");
        core_out_hex32(&s_out, probe);
        core_out_str(&s_out, " key_crc 0x");
        core_out_hex32(&s_out, crc32_end(&ctx));
        core_out_str(&s_out, " fb0_crc 0x");
        core_out_hex32(&s_out, fb_crc(s_fb_dev[0]));
        core_out_str(&s_out, " keybase 0x");
        core_out_hex32(&s_out, s_tex_key);
        out_end();
        if (probe != 0x000a1603u)
            s_fail++;
    }

    exec_sw_reset(&s_es);

    run_verify();

    if (!E7_VERIFY_ONLY) {
        /* VERIFY 的 gfx_case_build 会把色键纹理改写成 SW3 的 64x64 纹理，
           性能段必须重新铺受控输入，否则量到的是别人留下的数据。 */
        seed_tex(s_tex_src, SRC_W, SRC_H, SRC_SL, GFX_FMT_ARGB8888_PM, 0);
        seed_tex(s_tex_st, SRC_W, SRC_H, SRC_SL, GFX_FMT_ARGB8888_ST, 0);
        seed_tex(s_tex_key, KEY_W, KEY_H, KEY_SL, GFX_FMT_XRGB8888, 1);
        seed_cmp_tex();
        run_loads();
        run_s1(0u);
        run_s1(1u);
        run_ovh();
    }

    out_begin();
    core_out_str(&s_out, "fail ");
    f_num((uint32_t)s_fail);
    out_end();
    say("done");
    return 0;
}
