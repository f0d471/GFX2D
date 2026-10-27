/******************************************************************************
* @file stg_runtime.c: 共享启动运行时（SW10）
*
* @brief  各函数与 gfx2dGame/main.c 的 boot_mem/boot_asset/boot_surfaces_stg/
*         boot_surfaces_r4/boot_tex/tex_overlay/backend_apply 及 r4 钩子逐段
*         对应（行为等价迁移，不改判据路径：gfx2dGame 仍用自己的那份）。
******************************************************************************/

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "board.h"

#include "core/crc32.h"
#include "core/format.h"

#include "gfx/gfx.h"

#include "assets.h"
#include "stg_slots.h"

#ifdef __riscv
#include "gfx_engine.h"
#endif

#include "r4.h"
#include "r4_atlas.h"

#include "stg_runtime.h"

static stg_runtime *s_active;

void stg_runtime_set_active(stg_runtime *rt)
{
    s_active = rt;
}

/* --- 小输出（与 main.c 的 boot_fail_report/say 同一格式） ------------------ */

static void say_win_err(const char *s)
{
    board_puts(s);
}

static void say_pak_err(const stg_runtime *rt, int rc)
{
    char     buf[128];
    core_out o;

    core_out_init(&o, buf, (uint32_t)sizeof buf);
    core_out_str(&o, "pak err ");
    if (rc > 0) {
        core_out_u32(&o, (uint32_t)rc);
        core_out_ch(&o, ' ');
        core_out_str(&o, assetpak_strerror(rc));
    } else {
        core_out_u32(&o, (uint32_t)(-rc));
        core_out_str(&o, " board_read");
    }
    core_out_str(&o, " id=");
    core_out_u32(&o, rt->pak.fail_id);
    core_out_str(&o, "\r\n");
    board_puts(buf);
}

static void say_pak_line(const stg_runtime *rt)
{
    char     buf[96];
    core_out o;

    core_out_init(&o, buf, (uint32_t)sizeof buf);
    core_out_str(&o, "pak len ");
    core_out_u32(&o, rt->pak.total_len);
    core_out_str(&o, " crc ");
    core_out_hex32(&o, rt->pak.pack_crc);
    core_out_str(&o, "\r\n");
    board_puts(buf);
}

/* --- 启动 ---------------------------------------------------------------- */

int stg_runtime_boot(stg_runtime *rt)
{
    const uint32_t  asset_dev = g_mem_regions[REGION_ASSET].offset;
    const uint32_t  fb0_dev   = g_mem_regions[REGION_FB0].offset;
    const uint32_t  fb1_dev   = g_mem_regions[REGION_FB1].offset;
    const uint32_t  off_dev   = g_mem_regions[REGION_OFFSCREEN].offset;
    const uint32_t  off_end   = off_dev + g_mem_regions[REGION_OFFSCREEN].size;
    const board_mem asset = board_region(REGION_ASSET);
    const board_mem off   = board_region(REGION_OFFSCREEN);
    const board_mem fb0   = board_region(REGION_FB0);
    const board_mem fb1   = board_region(REGION_FB1);
    const board_mem cmdA  = board_region(REGION_CMDLIST_A);
    const board_mem cmdB  = board_region(REGION_CMDLIST_B);
    assetpak_limits  lim;
    uint32_t         len = 0u;
    int              rc;

    if ((uint32_t)((uintptr_t)off.ptr - (uintptr_t)asset.ptr) != off_dev - asset_dev
        || (uint32_t)((uintptr_t)fb0.ptr - (uintptr_t)asset.ptr) != fb0_dev - asset_dev
        || (uint32_t)((uintptr_t)fb1.ptr - (uintptr_t)asset.ptr) != fb1_dev - asset_dev) {
        say_win_err("win err 分区指针与分区表不一致\r\n");
        return -1;
    }
    if (off_end > DDR_SIM_BYTES) {
        say_win_err("win err 窗口越过 DDR 末端\r\n");
        return -1;
    }

    rt->mem.dev_base = asset_dev;
    rt->mem.host     = (uint8_t *)asset.ptr;
    rt->mem.bytes    = off_end - asset_dev;

    rt->cmds[0] = (gfx_cmd *)cmdA.ptr;
    rt->cap[0]  = cmdA.size / (uint32_t)sizeof(gfx_cmd);
    rt->cmds[1] = (gfx_cmd *)cmdB.ptr;
    rt->cap[1]  = cmdB.size / (uint32_t)sizeof(gfx_cmd);


    lim.max_total = BOARD_ASSET_MAX_BYTES;
    if (lim.max_total > asset.size)
        lim.max_total = asset.size;

    rc = assetpak_load(&rt->pak, board_asset_read, asset.ptr, asset.size, &lim, &len);
    if (rc != ASSETPAK_OK) {
        say_pak_err(rt, rc);
        return -1;
    }
    say_pak_line(rt);

    /* 与生成的 assets.h 对账：不一致记一次 fail，判据口径能立刻归因 */
    if (rt->pak.version != ASSET_PAK_VERSION)
        rt->fails++;
    if (rt->pak.total_len != ASSET_PAK_TOTAL_LEN || rt->pak.pack_crc != ASSET_PAK_CRC32
        || rt->pak.entry_count != ASSET_PAK_ENTRY_COUNT)
        rt->fails++;
    return 0;
}

/* --- 表面槽 -------------------------------------------------------------- */

static int surface_from_entry(stg_runtime *rt, uint32_t entry, int slot)
{
    assetpak_entry e;
    gfx_surface    s;

    if (assetpak_entry_at(&rt->pak, entry, &e) != ASSETPAK_OK)
        return -1;
    s.base        = g_mem_regions[REGION_ASSET].offset + e.offset;
    s.width       = e.width;
    s.height      = e.height;
    s.stride_log2 = e.stride_log2;
    s.space       = GFX_SPACE_DDR;
    s.format      = e.format;
    return gfx_surface_set(&rt->tab, slot, &s);
}

static int surface_fb_pair(stg_runtime *rt, int slot0, int slot1)
{
    const uint32_t fb_dev[2] = { g_mem_regions[REGION_FB0].offset,
                                 g_mem_regions[REGION_FB1].offset };
    gfx_surface    s;
    uint32_t       i;

    s.width       = (uint16_t)FB_WIDTH;
    s.height      = (uint16_t)FB_HEIGHT;
    s.stride_log2 = (uint8_t)FB_STRIDE_LOG2;
    s.space       = GFX_SPACE_DDR;
    s.format      = GFX_FMT_XRGB8888;
    for (i = 0u; i < 2u; i++) {
        s.base = fb_dev[i];
        if (gfx_surface_set(&rt->tab, (i == 0u) ? slot0 : slot1, &s) != 0)
            return -1;
    }
    return 0;
}

int stg_runtime_surfaces_stg(stg_runtime *rt)
{
    static const struct {
        uint32_t entry;
        int      slot;
    } pick[STG_SLOT_N_ASSETS] = {
        { ASSET_BUL16_ENTRY,     STG_SLOT_BUL16 },
        { ASSET_BUL8_ENTRY,      STG_SLOT_BUL8 },
        { ASSET_RICE20_ENTRY,    STG_SLOT_RICE },
        { ASSET_FONT8X16_ENTRY,  STG_SLOT_FONT },
        { ASSET_PSHOT_ENTRY,     STG_SLOT_PSHOT },
        { ASSET_SPARK_ENTRY,     STG_SLOT_SPARK },
        { ASSET_SHIPS_ENTRY,     STG_SLOT_SHIPS },
        { ASSET_BOSS_ENTRY,      STG_SLOT_BOSS },
        { ASSET_STARS_ENTRY,     STG_SLOT_STARS },
        { ASSET_TITLE_ENTRY,     STG_SLOT_TITLE },
        { ASSET_DIGITS_ENTRY,    STG_SLOT_DIGITS },
        { ASSET_ICONS_ENTRY,     STG_SLOT_ICONS },
        { ASSET_FONT_CYAN_ENTRY, STG_SLOT_FONT_CYAN },
        { ASSET_BG_LAYERS_ENTRY, STG_SLOT_BGLAYERS },
    };
    uint32_t i;

    gfx_surface_reset(&rt->tab);
    rt->tex_on = 0;               /* 表重建回 DDR 基址，覆盖层随之失效 */

    for (i = 0u; i < (uint32_t)STG_SLOT_N_ASSETS; i++)
        if (surface_from_entry(rt, pick[i].entry, pick[i].slot) != 0)
            return -1;
    return surface_fb_pair(rt, STG_SLOT_FB0, STG_SLOT_FB1);
}

int stg_runtime_surfaces_r4(stg_runtime *rt)
{
    static const struct {
        uint32_t entry;
        int      slot;
    } pick[4] = {
        { ASSET_FONT8X16_ENTRY,  STG_SLOT_FONT },
        { ASSET_DIGITS_ENTRY,    STG_SLOT_DIGITS },
        { ASSET_FONT_CYAN_ENTRY, STG_SLOT_FONT_CYAN },
        { ASSET_TITLE_ENTRY,     STG_SLOT_TITLE },
    };
    gfx_surface s;
    uint32_t    i;

    gfx_surface_reset(&rt->tab);
    rt->tex_on = 0;

    for (i = 0u; i < 4u; i++)
        if (surface_from_entry(rt, pick[i].entry, pick[i].slot) != 0)
            return -1;
    if (surface_fb_pair(rt, STG_SLOT_FB0, STG_SLOT_FB1) != 0)
        return -1;
    /* 背景面：LOAD 每帧整屏读它；内容由 r4 的进入路径预建。 */
    s.width       = (uint16_t)FB_WIDTH;
    s.height      = (uint16_t)FB_HEIGHT;
    s.stride_log2 = (uint8_t)FB_STRIDE_LOG2;
    s.space       = GFX_SPACE_DDR;
    s.format      = GFX_FMT_XRGB8888;
    s.base        = g_mem_regions[REGION_OFFSCREEN].offset + R4_BG_OFFSET;
    return gfx_surface_set(&rt->tab, R4_SLOT_BG, &s);
}

/* --- TEX 热集与后端覆盖层 ------------------------------------------------ */

/* 热集 = 标准圆弹/小圆弹/火花（游戏口径），按现行 manifest 顺序紧凑摆进
   16 KiB 窗口；装载一次、读回 CRC 自校验。装载与读回要求引擎空闲。 */
static const struct {
    uint32_t entry;
    int      slot;
} s_tex_hot[3] = {
    { ASSET_BUL16_ENTRY, STG_SLOT_BUL16 },
    { ASSET_BUL8_ENTRY,  STG_SLOT_BUL8 },
    { ASSET_SPARK_ENTRY, STG_SLOT_SPARK },
};

/* 返回 0 成功；负值为失败定位：-10 caps 读不到/-11 无 TEX 窗口/
   -12 写入失败/-13 读回 CRC 不符/-14 包内查不到/格式与窗口限制。 */
int stg_runtime_tex_load(stg_runtime *rt)
{
    const uint32_t asset_dev = g_mem_regions[REGION_ASSET].offset;
    uint32_t off = 0u;
    uint32_t i;

    rt->tex_ready = 0;
#ifdef __riscv
    {
        uint32_t caps = 0u;

        if (gfx_engine_caps_read(&caps) != GFX_ENG_OK)
            return -10;
        if ((caps & GFX_CAPS_TEX_M) == 0u)
            return -11;
    }
    for (i = 0u; i < 3u; i++) {
        assetpak_entry e;
        uint32_t bytes, pos, want, got;
        crc32_ctx c1, c2;

        if (assetpak_entry_at(&rt->pak, s_tex_hot[i].entry, &e) != ASSETPAK_OK)
            return -14;
        bytes = e.length;                       /* = height << stride_log2，含行 padding */
        if ((bytes & 3u) != 0u || off + bytes > GFX_TEX_BYTES)
            return -14;
        board_dcache_invalidate_range((const void *)(uintptr_t)(asset_dev + e.offset), bytes);
        if (gfx_engine_tex_write((const void *)(uintptr_t)(asset_dev + e.offset),
                                 off, bytes) != GFX_ENG_OK)
            return -12;
        crc32_begin(&c1);
        crc32_update(&c1, (const void *)(uintptr_t)(asset_dev + e.offset), bytes);
        want = crc32_end(&c1);
        crc32_begin(&c2);
        for (pos = 0u; pos < bytes; ) {
            uint32_t chunk[64];
            uint32_t n = bytes - pos;

            if (n > sizeof chunk)
                n = sizeof chunk;
            if (gfx_engine_tex_read(chunk, off + pos, n) != GFX_ENG_OK)
                return -12;
            crc32_update(&c2, chunk, n);
            pos += n;
        }
        got = crc32_end(&c2);
        if (want != e.data_crc || got != want)
            return -13;
        rt->tex_off[i] = off;
        rt->tex_ddr[i] = asset_dev + e.offset;
        off += bytes;
    }
#endif
    rt->tex_ready = 1;
    return 0;
}

int stg_runtime_tex_overlay(stg_runtime *rt, int on)
{
    uint32_t i;

    for (i = 0u; i < 3u; i++) {
        assetpak_entry e;
        gfx_surface s;

        if (assetpak_entry_at(&rt->pak, s_tex_hot[i].entry, &e) != ASSETPAK_OK)
            return -1;
        s.width = e.width;
        s.height = e.height;
        s.stride_log2 = e.stride_log2;
        s.format = e.format;
        s.base = on ? rt->tex_off[i] : rt->tex_ddr[i];
        s.space = on ? GFX_SPACE_TEX : GFX_SPACE_DDR;
        if (gfx_surface_set(&rt->tab, s_tex_hot[i].slot, &s) != 0)
            return -1;
    }
    rt->tex_on = on;
    return 0;
}

int stg_runtime_backend_apply(stg_runtime *rt, uint32_t backend)
{
    /* 演示模式（模式 1）与 r4（模式 4）独占 TEX，不能覆盖成 stg 的热集槽；
       console 里这两类应用自带独占路径，这里只服务 stg 后端。 */
    const int want = (backend >= 2u);

    if (!want)
        return rt->tex_on ? stg_runtime_tex_overlay(rt, 0) : 0;
    if (!rt->tex_ready)
        return -1;
    return rt->tex_on ? 0 : stg_runtime_tex_overlay(rt, 1);
}

/* --- r4 钩子（经 r4_set_hooks 装上，钩子无上下文参数 -> 用当前实例） ------- */

static int hook_r4_load_atlas(void)
{
    memcpy((void *)r4_atlas_shadow(), r4_atlas_words, (uint32_t)sizeof r4_atlas_words);
#ifdef __riscv
    if (s_active != NULL) {
        uint32_t got;
        uint32_t pos;
        crc32_ctx c;

        board_dcache_invalidate_range(r4_atlas_words, (uint32_t)sizeof r4_atlas_words);
        if (gfx_engine_caps_read(&got) != GFX_ENG_OK || !(got & GFX_CAPS_TEX_M))
            return -1;
        if (gfx_engine_tex_write(r4_atlas_words, 0u, (uint32_t)sizeof r4_atlas_words)
            != GFX_ENG_OK)
            return -1;
        crc32_begin(&c);
        for (pos = 0u; pos < (uint32_t)sizeof r4_atlas_words; ) {
            uint32_t chunk[64];
            uint32_t n = (uint32_t)sizeof r4_atlas_words - pos;

            if (n > sizeof chunk)
                n = sizeof chunk;
            if (gfx_engine_tex_read(chunk, pos, n) != GFX_ENG_OK)
                return -1;
            crc32_update(&c, chunk, n);
            pos += n;
        }
        got = crc32_end(&c);
        return (got == R4_ATLAS_CRC) ? 0 : -1;
    }
    return -1;
#else
    return 0;                   /* 宿主没有 TEX 窗口：影子即可（rast_ref 用） */
#endif
}

static void hook_r4_tex_dirty(void)
{
    if (s_active != NULL)
        s_active->tex_ready = 0;    /* TEX 窗口被 3D atlas 覆盖：热集必须重装 */
}

static int hook_r4_tex_restore(void)
{
    if (s_active == NULL)
        return -1;
#ifdef __riscv
    if (stg_runtime_tex_load(s_active) != 0)
        return -1;
    s_active->tex_ready = 1;
#else
    s_active->tex_ready = 0;
#endif
    return 0;
}

void stg_runtime_r4_install_hooks(void)
{
    r4_set_hooks(hook_r4_load_atlas, hook_r4_tex_dirty, hook_r4_tex_restore);
}
