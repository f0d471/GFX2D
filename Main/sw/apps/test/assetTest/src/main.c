/******************************************************************************
* @file main.c: assetTest —— 素材流水线的判据程序
*
* @brief  一次跑完素材流水线软件侧的判据：
*           1. 经板级读入口把素材包装进 REGION_ASSET（PC 读文件、板上读 Flash），
*              由 lib/core/assetpak 两段式装载：先读包头校验总长，再读正文；
*           2. 与编译时生成的 assets.h 对账（版本、条目数、总长、整包 CRC、逐项几何）；
*           3. 按当前画面需要**挑选**若干条目登记成表面——包 ID 不是表面槽号，
*              v1 只有 16 槽，不能按 ID 一次全登；
*           4. 用软件执行器把一整帧画进帧缓冲：FILL + 整屏背景 + 棋盘平铺 +
*              图集 ALPHA 子矩形 + 预乘/直通弹幕对照 + 色键 + 字库文字；
*           5. 打印整帧 CRC、一块已知窗口的 CRC，以及一张 64×24 的字符画。
*              字符画在两个平台上逐字节相同，是留档的那张「可目视核对」的图。
*
*         串起来跑的是 Main/tools/check/check_sw5_assets.sh。
*         板上与 PC 跑的是同一份源码、同一份素材包，整帧 CRC 必须相同。
******************************************************************************/

#include <stddef.h>
#include <stdint.h>

#include "board.h"
#include "core/assetpak.h"
#include "core/crc32.h"
#include "core/format.h"
#include "gfx/gfx.h"

#include "assets.h"         /* 生成物：Main/assets/build/assets.h */

/* assets.h 与解析器认的版本必须一致，否则编译期就挡住。 */
_Static_assert(ASSET_PAK_VERSION == ASSETPAK_VERSION, "assets.h 与 assetpak.h 的版本不一致");

/* 表面槽的分配（演示「按需要挑选、登记」，不是按 ID 全登）：
   0 背景、1 图集、2 字库、3 帧缓冲、4/5 弹幕对照、6 色键图。 */
#define SLOT_FB        3
#define SLOT_BULLET_PM 4
#define SLOT_BULLET_ST 5
#define SLOT_KEY_DEMO  6

static uint32_t g_fail;

/* --- 输出 ---------------------------------------------------------------- */

static void say(const char *tag, int ok)
{
    board_puts(tag);
    board_puts(ok ? " ok\r\n" : " FAIL\r\n");
    if (!ok)
        g_fail++;
}

static void line1(const char *tag, uint32_t v)
{
    char     buf[128];
    core_out o;

    core_out_init(&o, buf, (uint32_t)sizeof buf);
    core_out_str(&o, tag);
    core_out_hex32(&o, v);
    core_out_str(&o, "\r\n");
    board_puts(buf);
}

static void line2(const char *tag, uint32_t a, uint32_t b)
{
    char     buf[160];
    core_out o;

    core_out_init(&o, buf, (uint32_t)sizeof buf);
    core_out_str(&o, tag);
    core_out_u32(&o, a);
    core_out_str(&o, " ");
    core_out_u32(&o, b);
    core_out_str(&o, "\r\n");
    board_puts(buf);
}

/* --- 条目几何：与 assets.h 对账 ------------------------------------------ */

static int entry_matches(const assetpak *pk, uint32_t index, uint32_t id, uint32_t w,
                         uint32_t h, uint32_t sl, uint32_t fmt, uint32_t len)
{
    assetpak_entry e;

    if (assetpak_entry_at(pk, index, &e) != ASSETPAK_OK)
        return 0;
    return e.id == id && e.width == w && e.height == h
        && e.stride_log2 == sl && e.format == fmt && e.length == len;
}

/* 逐项打印，给 check_sw5_assets.sh 的独立实现（Python 侧直接解 .pak）对账。 */
static void dump_entries(const assetpak *pk)
{
    uint32_t i;

    for (i = 0u; i < pk->entry_count; i++) {
        assetpak_entry e;
        char          buf[192];
        core_out      o;

        if (assetpak_entry_at(pk, i, &e) != ASSETPAK_OK) {
            board_puts("entry BAD\r\n");
            g_fail++;
            return;
        }
        core_out_init(&o, buf, (uint32_t)sizeof buf);
        core_out_str(&o, "entry ");
        core_out_u32(&o, i);
        core_out_str(&o, " id=");
        core_out_u32(&o, e.id);
        core_out_str(&o, " fmt=");
        core_out_u32(&o, e.format);
        core_out_str(&o, " ");
        core_out_u32(&o, e.width);
        core_out_str(&o, "x");
        core_out_u32(&o, e.height);
        core_out_str(&o, " sl=");
        core_out_u32(&o, e.stride_log2);
        core_out_str(&o, " len=");
        core_out_u32(&o, e.length);
        core_out_str(&o, " crc=");
        core_out_hex32(&o, e.data_crc);
        core_out_str(&o, "\r\n");
        board_puts(buf);
    }
}

/* --- 帧缓冲 -------------------------------------------------------------- */

static const char *RAMP = " .:-=+*#%@";

/* 逐行取前 width * 4 字节、跳过行跨的间隙：算进间隙会与板上 DDR 的上电残值对不上。 */
static uint32_t fb_crc(const uint32_t *fb)
{
    crc32_ctx ctx;
    uint32_t  y;

    crc32_begin(&ctx);
    for (y = 0u; y < FB_HEIGHT; y++) {
        const uint8_t *row = (const uint8_t *)fb + (size_t)y * FB_STRIDE_BYTES;
        crc32_update(&ctx, row, FB_WIDTH * 4u);
    }
    return crc32_end(&ctx);
}

static uint32_t window_crc(const uint32_t *fb, uint32_t x, uint32_t y,
                           uint32_t w, uint32_t h)
{
    crc32_ctx ctx;
    uint32_t  r;

    crc32_begin(&ctx);
    for (r = 0u; r < h; r++) {
        const uint8_t *p = (const uint8_t *)fb
                         + (size_t)(y + r) * FB_STRIDE_BYTES + (size_t)x * 4u;
        crc32_update(&ctx, p, w * 4u);
    }
    return crc32_end(&ctx);
}

/* 64×24 的字符画：从帧缓冲按固定步长采样。两个平台逐字节相同，
   既是给 check 脚本比对的又一条判据，也是留档的那张可目视核对的图。 */
static void thumb(const uint32_t *fb)
{
    uint32_t r, c;
    char     buf[80];

    board_puts("thumb 64x24\r\n");
    for (r = 0u; r < 24u; r++) {
        uint32_t y = r * (FB_HEIGHT / 24u);
        for (c = 0u; c < 64u; c++) {
            uint32_t x = c * (FB_WIDTH / 64u);
            uint32_t px = fb[(size_t)y * (FB_STRIDE_BYTES / 4u) + x];
            uint32_t lum = ((px >> 16) & 0xFFu) * 77u
                         + ((px >> 8) & 0xFFu) * 150u
                         + (px & 0xFFu) * 29u;
            buf[c] = RAMP[((lum >> 8) * 9u) / 255u];
        }
        buf[64] = '\0';
        board_puts(buf);
        board_puts("\r\n");
    }
}

/* 报错退出：装不出来就没有可用的表面，不继续往下跑。 */
static int fail_load(const assetpak *pk, int rc)
{
    char     buf[128];
    core_out o;

    core_out_init(&o, buf, (uint32_t)sizeof buf);
    core_out_str(&o, "pak err ");
    if (rc > 0) {
        core_out_u32(&o, (uint32_t)rc);
        core_out_str(&o, " ");
        core_out_str(&o, assetpak_strerror(rc));
    } else {
        core_out_u32(&o, (uint32_t)(-rc));
        core_out_str(&o, " board_read");
    }
    core_out_str(&o, " id=");
    core_out_u32(&o, pk->fail_id);
    core_out_str(&o, "\r\n");
    board_puts(buf);
    board_puts("fail 1\r\ndone\r\n");
    return 1;
}

/* --- 主流程 -------------------------------------------------------------- */

int main(void)
{
    board_mem         asset, fb0;
    assetpak          pak;
    assetpak_entry    e;
    assetpak_limits   lim;
    gfx_surface_table tab;
    gfx_mem           mem;
    gfx_list          list;
    exec_sw_state     es;
    exec_sw_stats     st;
    exec_sw_error     err;
    gfx_cmd          *cmds;
    uint32_t          cmd_cap, len = 0u, rc, i;
    const uint32_t    asset_dev = g_mem_regions[REGION_ASSET].offset;
    const uint32_t    fb_dev    = g_mem_regions[REGION_FB0].offset;

    board_init();
    board_puts("assetTest SW5\r\n");

    asset = board_region(REGION_ASSET);
    fb0   = board_region(REGION_FB0);

    /* --- 装载 + 解析 ---------------------------------------------------- */
    lim.max_total = BOARD_ASSET_MAX_BYTES;
    if (lim.max_total > asset.size)
        lim.max_total = asset.size;

    rc = assetpak_load(&pak, board_asset_read, asset.ptr, asset.size, &lim, &len);
    if (rc != ASSETPAK_OK)
        return fail_load(&pak, rc);

    line2("pak bytes/entries=", len, pak.entry_count);
    line1("pak crc=", pak.pack_crc);

    /* --- 与生成的头对账 -------------------------------------------------- */
    say("chk P1 版本与 assets.h 一致", pak.version == ASSET_PAK_VERSION);
    say("chk P2 整包 CRC 与 assets.h 一致", pak.pack_crc == ASSET_PAK_CRC32);
    say("chk P3 总长与条目数与 assets.h 一致",
        pak.total_len == ASSET_PAK_TOTAL_LEN && pak.entry_count == ASSET_PAK_ENTRY_COUNT);

    say("chk E1 BG_GRAD 几何与 assets.h 一致",
        entry_matches(&pak, ASSET_BG_GRAD_ENTRY, ASSET_BG_GRAD_ID,
                      ASSET_BG_GRAD_W, ASSET_BG_GRAD_H, ASSET_BG_GRAD_STRIDE_LOG2,
                      ASSET_BG_GRAD_FORMAT, ASSET_BG_GRAD_LENGTH));
    say("chk E2 CHECKER_TILE 几何与 assets.h 一致",
        entry_matches(&pak, ASSET_CHECKER_TILE_ENTRY, ASSET_CHECKER_TILE_ID,
                      ASSET_CHECKER_TILE_W, ASSET_CHECKER_TILE_H,
                      ASSET_CHECKER_TILE_STRIDE_LOG2, ASSET_CHECKER_TILE_FORMAT,
                      ASSET_CHECKER_TILE_LENGTH));
    say("chk E3 TILE_ATLAS 几何与 assets.h 一致",
        entry_matches(&pak, ASSET_TILE_ATLAS_ENTRY, ASSET_TILE_ATLAS_ID,
                      ASSET_TILE_ATLAS_W, ASSET_TILE_ATLAS_H,
                      ASSET_TILE_ATLAS_STRIDE_LOG2, ASSET_TILE_ATLAS_FORMAT,
                      ASSET_TILE_ATLAS_LENGTH));
    say("chk E4 FONT8X16 几何与 assets.h 一致",
        entry_matches(&pak, ASSET_FONT8X16_ENTRY, ASSET_FONT8X16_ID,
                      ASSET_FONT8X16_W, ASSET_FONT8X16_H, ASSET_FONT8X16_STRIDE_LOG2,
                      ASSET_FONT8X16_FORMAT, ASSET_FONT8X16_LENGTH));
    say("chk E5 BULLET_PM 是预乘 ARGB8888",
        assetpak_entry_at(&pak, ASSET_BULLET_PM_ENTRY, &e) == ASSETPAK_OK
        && e.format == ASSETPAK_FMT_ARGB8888_PM && e.width == 32u && e.height == 32u);
    say("chk E6 BULLET_ST 是直通 ARGB8888",
        assetpak_entry_at(&pak, ASSET_BULLET_ST_ENTRY, &e) == ASSETPAK_OK
        && e.format == ASSETPAK_FMT_ARGB8888_ST);
    say("chk E7 KEY_DEMO 带色键且键字相符",
        assetpak_entry_at(&pak, ASSET_KEY_DEMO_ENTRY, &e) == ASSETPAK_OK
        && (e.flags & ASSETPAK_FLAG_KEY) != 0u && e.key == ASSET_KEY_DEMO_KEY);
    say("chk E8 SOLID8 是最小的 8x8 表面",
        assetpak_entry_at(&pak, ASSET_SOLID8_ENTRY, &e) == ASSETPAK_OK
        && e.width == 8u && e.height == 8u && e.stride_log2 == 5u);

    dump_entries(&pak);

    /* --- 设备地址窗口 ---------------------------------------------------- */
    /* gfx_mem 必须同时盖住素材区与目标帧缓冲：素材条目的设备地址是
       asset_dev + 条目偏移，帧缓冲在 fb_dev。只填 REGION_ASSET.size 会让帧缓冲
       落在窗口外，BLIT 按 GFX_E_BAD_SURFACE 失败（sw 第 04 章 3.8 节）。 */
    {
        uint32_t span = fb_dev - asset_dev + FB_BYTES;

        say("chk M1 相邻分区指针与分区表偏移一致",
            (uint32_t)((uintptr_t)fb0.ptr - (uintptr_t)asset.ptr)
            == fb_dev - asset_dev);
        mem.dev_base = asset_dev;
        mem.host     = (uint8_t *)asset.ptr;
        mem.bytes    = span;
        line2("mem window bytes/base=", span, mem.dev_base);
    }

    /* --- 按需要挑选条目登记表面 ------------------------------------------ */
    gfx_surface_reset(&tab);
    {
        static const struct {
            uint32_t    index;
            const char *name;
            int         slot;
        } pick[] = {
            { ASSET_BG_GRAD_ENTRY,    "BG_GRAD",    0 },
            { ASSET_TILE_ATLAS_ENTRY, "TILE_ATLAS", 1 },
            { ASSET_FONT8X16_ENTRY,   "FONT8X16",   2 },
            { ASSET_BULLET_PM_ENTRY,  "BULLET_PM",  SLOT_BULLET_PM },
            { ASSET_BULLET_ST_ENTRY,  "BULLET_ST",  SLOT_BULLET_ST },
            { ASSET_KEY_DEMO_ENTRY,   "KEY_DEMO",   SLOT_KEY_DEMO },
        };

        for (i = 0u; i < sizeof pick / sizeof pick[0]; i++) {
            gfx_surface s;
            char        tag[96];
            core_out    o;

            core_out_init(&o, tag, (uint32_t)sizeof tag);
            core_out_str(&o, "chk S");
            core_out_u32(&o, i + 1u);
            core_out_str(&o, " ");
            core_out_str(&o, pick[i].name);

            if (assetpak_entry_at(&pak, pick[i].index, &e) != ASSETPAK_OK) {
                say(tag, 0);
                continue;
            }
            s.base        = asset_dev + e.offset;
            s.width       = e.width;
            s.height      = e.height;
            s.stride_log2 = e.stride_log2;
            s.space       = GFX_SPACE_DDR;
            s.format      = e.format;
            say(tag, gfx_surface_set(&tab, pick[i].slot, &s) == 0);
        }
    }
    {
        gfx_surface s;

        s.base        = fb_dev;
        s.width       = FB_WIDTH;
        s.height      = FB_HEIGHT;
        s.stride_log2 = FB_STRIDE_LOG2;
        s.space       = GFX_SPACE_DDR;
        s.format      = GFX_FMT_XRGB8888;
        say("chk S7 FB0 登记成功", gfx_surface_set(&tab, SLOT_FB, &s) == 0);
    }

    /* --- 组帧 ------------------------------------------------------------ */
    cmds    = (gfx_cmd *)board_region(REGION_CMDLIST_A).ptr;
    cmd_cap = board_region(REGION_CMDLIST_A).size / (uint32_t)sizeof(gfx_cmd);
    gfx_list_begin(&list, cmds, cmd_cap, &tab);

    {
        gfx_rect all;
        int      bad = 0;       /* 被构造层拒绝的追加次数（返回码非 0） */

        all.x0 = 0;
        all.y0 = 0;
        all.x1 = (int32_t)FB_WIDTH - 1;
        all.y1 = (int32_t)FB_HEIGHT - 1;

        /* 1 整屏 FILL(COPY) 打底：保证没有未初始化的像素进 CRC */
        bad += (gfx_fill(&list, SLOT_FB, all, 0x00101820u, GFX_EQ_COPY, 0u) != 0);

        /* 2 整屏背景 BLIT COPY */
        bad += (gfx_blit(&list, 0, 0, 0, SLOT_FB, 0, 0, 640, 480,
                         GFX_EQ_COPY, 0u, 0, 0u) != 0);

        /* 3 棋盘小块 3x3 平铺成 384x384，盖在背景上 */
        for (i = 0u; i < 9u; i++) {
            bad += (gfx_blit(&list, 1, 0, 0, SLOT_FB,
                             (int)(320 + (i % 3u) * 128u),
                             (int)(48 + (i / 3u) * 128u),
                             128, 128, GFX_EQ_COPY, 0u, 0, 0u) != 0);
        }

        /* 4 图集子矩形：ALPHA(预乘) 取 4 个格子，格子号与落点都固定 */
        for (i = 0u; i < 4u; i++) {
            uint32_t cell = 9u + i * 5u;
            uint32_t sx = (cell % ASSET_TILE_ATLAS_COLS) * ASSET_TILE_ATLAS_CELL_W;
            uint32_t sy = (cell / ASSET_TILE_ATLAS_COLS) * ASSET_TILE_ATLAS_CELL_H;

            bad += (gfx_blit(&list, 1, (int)sx, (int)sy, SLOT_FB,
                             (int)(40 + i * 40u), 40, 16, 16,
                             GFX_EQ_ALPHA, 0u, 0, 0u) != 0);
        }

        /* 5 预乘与直通两条路径各画一颗：画面不同，用来对照量化差别 */
        bad += (gfx_blit(&list, SLOT_BULLET_PM, 0, 0, SLOT_FB, 60, 400,
                         32, 32, GFX_EQ_ALPHA, 0u, 0, 0u) != 0);
        bad += (gfx_blit(&list, SLOT_BULLET_ST, 0, 0, SLOT_FB, 120, 400,
                         32, 32, GFX_EQ_ALPHA_STRAIGHT, 0u, 0, 0u) != 0);

        /* 6 色键：KEY_DEMO 的背景就是键字，整块 COPY 只留下中间那个方块 */
        bad += (gfx_blit(&list, SLOT_KEY_DEMO, 0, 0, SLOT_FB, 480, 40,
                         32, 32, GFX_EQ_COPY, 0u, 1, ASSET_KEY_DEMO_KEY) != 0);

        /* 7 字库文字：xrgb 图集，背景是 0x00000000，用色键跳过 */
        {
            static const char *text = "GFX2D SW5 ASSET PAK 0123456789";
            int                x = 20;

            for (i = 0u; text[i] != '\0'; i++) {
                uint32_t gi = (uint32_t)text[i] - 0x20u;

                if (gi < ASSET_FONT8X16_CELLS) {
                    uint32_t sx = (gi % ASSET_FONT8X16_COLS) * ASSET_FONT8X16_CELL_W;
                    uint32_t sy = (gi / ASSET_FONT8X16_COLS) * ASSET_FONT8X16_CELL_H;

                    bad += (gfx_blit(&list, 2, (int)sx, (int)sy, SLOT_FB, x, 440,
                                     (int)ASSET_FONT8X16_CELL_W,
                                     (int)ASSET_FONT8X16_CELL_H,
                                     GFX_EQ_COPY, 0u, 1, 0x00000000u) != 0);
                }
                x += (int)ASSET_FONT8X16_CELL_W;
            }
        }

        say("chk C1 全部命令都被构造层接受", bad == 0 && list.rejected == 0u);
        line2("list used/cap=", list.used, list.cap);
    }

    if (gfx_list_end(&list) != 0) {
        board_puts("chk C2 gfx_list_end FAIL\r\n");
        g_fail++;
    }

    exec_sw_reset(&es);
    exec_sw_stats_clear(&st);
    rc = exec_sw_run(&es, &tab, &mem, cmds, list.used, list.used, &st, &err);
    say("chk X1 命令表执行无错", rc == GFX_OK);
    if (rc != GFX_OK)
        line2("exec err code/index=", (uint32_t)err.code, err.index);
    line2("exec cmd/pixel=", st.cmd, st.pixel_written);

    /* --- 画面 ------------------------------------------------------------ */
    {
        uint32_t *fb = board_fb(0);

        line1("frame crc=", fb_crc(fb));
        line1("win crc=", window_crc(fb, 40, 40, 64, 64));
        thumb(fb);
        board_present(0);
        for (i = 0u; i < 30u; i++)  /* 保留窗口显示 30 场；板上没有窗口就立即返回 */
            board_wait_frame();
    }

    {
        char     buf[48];
        core_out o;

        core_out_init(&o, buf, (uint32_t)sizeof buf);
        core_out_str(&o, "fail ");
        core_out_u32(&o, g_fail);
        core_out_str(&o, "\r\ndone\r\n");
        board_puts(buf);
    }
    return (g_fail == 0u) ? 0 : 1;
}
