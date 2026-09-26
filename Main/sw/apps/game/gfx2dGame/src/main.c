/******************************************************************************
* @file main.c: gfx2dGame —— SW6 基础 Demo
*
* @brief  赛题基础要求 3 的基础 Demo（spec：Engineering/Todo/sw/SW6-基础Demo.md）。
*         同一份应用每帧更新场景、生成命令表、交给 `lib/gfx` 的软件执行器、呈现后缓冲，
*         并逐帧打一行日志。两个场景：
*           A 大量方块随机移动：N=100~5000，每帧 N 条场景命令，瓶颈在命令数；
*           B 三张整屏图快速切换：初始化时预合成进 DDR 离屏层，每帧一次整屏 BLIT COPY，
*             瓶颈在访存带宽。
*
*         两个入口（编译期开关，板上没有 argv，见 spec 2.5）：
*           不给开关       演示模式：实时输入，A／B 每 SW6_STAGE_FRAMES 帧轮换；
*           -DSW6_JUDGE=1  判据模式：不读实时按键，按入库的两段录像各回放两遍，
*                          先打 pak／scene／eq P，再逐帧打 f 行，最后 fail／done。
*
*         判据与检查脚本见 spec 第 3 节与 2.6 节；期望日志入库
*         `logs/sw6_judge.log` 与 `logs/sw6_judge_quick.log`。
*
*         -DSTG_FIELD=<n> 时换成 SW7 B 批的弹幕场入口（stg_main.c，SW7 2.6 节）：
*         启动只做分区窗口、素材包与表面槽三步，不做预合成；SW6 的两种构建不受影响。
******************************************************************************/

#include <stddef.h>
#include <stdint.h>

#include "board.h"

#include "core/assetpak.h"
#include "core/crc32.h"
#include "core/format.h"
#include "core/replay.h"

#include "gfx/gfx.h"

#include "assets.h"
#include "sw6_a.h"      /* 生成物：$(OBJDIR)/replay/sw6_a.h */
#include "sw6_b.h"

#include "game/sw6_cfg.h"
#include "game/sw6_hud.h"
#include "game/sw6_input.h"
#include "game/sw6_report.h"
#include "game/sw6_scene_a.h"
#include "game/sw6_scene_b.h"

#include "stg_main.h"

/* --- 全局状态（都是静态的，不用 malloc）--------------------------------- */

static assetpak          s_pak;
static gfx_surface_table s_tab;
static gfx_mem           s_mem;
static exec_sw_state     s_es;
static exec_sw_error     s_err;
static gfx_cmd          *s_cmds[2];         /* 两张命令表，按帧号奇偶轮换 */
static uint32_t          s_cmd_cap[2];
static sw6_scene_a       s_a;
static sw6_scene_b       s_b;
static uint32_t          s_fail;

/* 上一帧最终的统计与三段耗时：HUD 显示的就是它们 */
static int      s_prev_valid;
static uint32_t s_prev_cmd;
static uint32_t s_prev_pix;
static uint32_t s_last_in;
static uint32_t s_last_bld;
static uint32_t s_last_exe;

/* 上一次交给 board_present 的那一面（-1 表示还没呈现过）。present 等切换完成才返回，
   所以它就是正在扫描的那一面：本帧的目的面必须是另一面，否则板上会撕裂。 */
static int s_shown = -1;

/* 全局的帧序号：目的面与命令表都按它的奇偶轮换，**不随每段每遍的帧号归零**。
   按每遍的帧号选面时，一遍的帧数是奇数（B 段 367 帧）就会在下一遍第 0 帧
   画到正在扫描的那一面上（spec 4.23 第 1 条）。 */
static uint32_t s_seq;

/* 一遍（一个阶段的一遍）里场景命令的按方程累计，给 `eq A` 那一行用 */
static uint32_t s_eq_acc[4];

/* SW FPS 的窗口：逐帧累加真实帧周期（64 位），满一秒结算一次 */
static uint64_t s_cyc_acc;
static uint32_t s_sw_fps;
static uint32_t s_fps_swaps0;
static int      s_fps_valid;    /* 第一个完整的一秒结算之前，HUD 显示 -- 而不是 0 */

/* --- 小输出 -------------------------------------------------------------- */

static void say2(const char *tag, uint32_t a, uint32_t b)
{
    char     buf[96];
    core_out o;

    core_out_init(&o, buf, (uint32_t)sizeof buf);
    core_out_str(&o, tag);
    core_out_u32(&o, a);
    core_out_ch(&o, ' ');
    core_out_u32(&o, b);
    core_out_str(&o, "\r\n");
    board_puts(buf);
}

/* --- 初始化 -------------------------------------------------------------- */

/* 按分区表算 gfx_mem 的窗口：素材区起点到 OFF2 末端（spec 2.0 第 1 条）。
   两个数都由分区表与 FB_BYTES 在运行时算出，代码里不写死；
   同时核对相邻分区的指针差等于分区表的偏移差（assetTest 的 chk M1 那一套）。 */
static int boot_mem(void)
{
    const uint32_t  asset_dev = g_mem_regions[REGION_ASSET].offset;
    const uint32_t  fb0_dev   = g_mem_regions[REGION_FB0].offset;
    const uint32_t  fb1_dev   = g_mem_regions[REGION_FB1].offset;
    const uint32_t  off_dev   = g_mem_regions[REGION_OFFSCREEN].offset;
    const uint32_t  off_end   = off_dev
                              + (SW6_SCENES - 1u) * SW6_OFF_SLOT_STEP + FB_BYTES;
    const board_mem asset = board_region(REGION_ASSET);
    const board_mem off   = board_region(REGION_OFFSCREEN);
    const board_mem fb0   = board_region(REGION_FB0);
    const board_mem fb1   = board_region(REGION_FB1);

    if ((uint32_t)((uintptr_t)off.ptr - (uintptr_t)asset.ptr) != off_dev - asset_dev
        || (uint32_t)((uintptr_t)fb0.ptr - (uintptr_t)asset.ptr) != fb0_dev - asset_dev
        || (uint32_t)((uintptr_t)fb1.ptr - (uintptr_t)asset.ptr) != fb1_dev - asset_dev) {
        board_puts("win err 分区指针与分区表不一致\r\n");
        return -1;
    }
    if (off_end > DDR_SIM_BYTES) {
        board_puts("win err 窗口越过 DDR 末端\r\n");
        return -1;
    }

    s_mem.dev_base = asset_dev;
    s_mem.host     = (uint8_t *)asset.ptr;
    s_mem.bytes    = off_end - asset_dev;
    return 0;
}

static void boot_fail_report(int rc)
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
    core_out_u32(&o, s_pak.fail_id);
    core_out_str(&o, "\r\n");
    board_puts(buf);
}

/* 装载素材包、与生成的头对账、打 `pak` 行（spec 2.0 第 1 条）。 */
static int boot_asset(void)
{
    const board_mem asset = board_region(REGION_ASSET);
    assetpak_limits  lim;
    uint32_t         len = 0u;
    int              rc;

    lim.max_total = BOARD_ASSET_MAX_BYTES;
    if (lim.max_total > asset.size)
        lim.max_total = asset.size;

    rc = assetpak_load(&s_pak, board_asset_read, asset.ptr, asset.size, &lim, &len);
    if (rc != ASSETPAK_OK) {
        boot_fail_report(rc);
        return -1;
    }

    sw6_log_pak(s_pak.total_len, s_pak.pack_crc);

    /* 不一致记一次 fail：`pak` 行照实打，D3 与期望日志对账时能立刻归因 */
    if (s_pak.version != ASSET_PAK_VERSION)
        s_fail++;
    if (s_pak.total_len != ASSET_PAK_TOTAL_LEN || s_pak.pack_crc != ASSET_PAK_CRC32
        || s_pak.entry_count != ASSET_PAK_ENTRY_COUNT)
        s_fail++;
    return 0;
}

/* 表面槽：0、1 是两张帧缓冲，2~9 是素材 8 项，10~12 是三张离屏图（spec 2.0 第 1 条）。 */
static int boot_surfaces(void)
{
    static const struct {
        uint32_t entry;
        int      slot;
    } pick[8] = {
        { ASSET_BG_GRAD_ENTRY,      SW6_SLOT_BG_GRAD },
        { ASSET_CHECKER_TILE_ENTRY, SW6_SLOT_CHECKER },
        { ASSET_TILE_ATLAS_ENTRY,   SW6_SLOT_ATLAS },
        { ASSET_FONT8X16_ENTRY,     SW6_SLOT_FONT },
        { ASSET_BULLET_PM_ENTRY,    SW6_SLOT_BULLET_PM },
        { ASSET_BULLET_ST_ENTRY,    SW6_SLOT_BULLET_ST },
        { ASSET_KEY_DEMO_ENTRY,     SW6_SLOT_KEY_DEMO },
        { ASSET_SOLID8_ENTRY,       SW6_SLOT_SOLID8 },
    };
    const uint32_t asset_dev = g_mem_regions[REGION_ASSET].offset;
    const uint32_t fb_dev[2] = { g_mem_regions[REGION_FB0].offset,
                                 g_mem_regions[REGION_FB1].offset };
    const uint32_t off_dev   = g_mem_regions[REGION_OFFSCREEN].offset;
    gfx_surface    s;
    uint32_t       i;

    gfx_surface_reset(&s_tab);

    for (i = 0u; i < 8u; i++) {
        assetpak_entry e;

        if (assetpak_entry_at(&s_pak, pick[i].entry, &e) != ASSETPAK_OK)
            return -1;
        s.base        = asset_dev + e.offset;
        s.width       = e.width;
        s.height      = e.height;
        s.stride_log2 = e.stride_log2;
        s.space       = GFX_SPACE_DDR;
        s.format      = e.format;
        if (gfx_surface_set(&s_tab, pick[i].slot, &s) != 0)
            return -1;
    }

    s.width       = (uint16_t)FB_WIDTH;
    s.height      = (uint16_t)FB_HEIGHT;
    s.stride_log2 = (uint8_t)FB_STRIDE_LOG2;
    s.space       = GFX_SPACE_DDR;
    s.format      = GFX_FMT_XRGB8888;
    for (i = 0u; i < 2u; i++) {
        s.base = fb_dev[i];
        if (gfx_surface_set(&s_tab, (i == 0u) ? SW6_SLOT_FB0 : SW6_SLOT_FB1, &s) != 0)
            return -1;
    }
    for (i = 0u; i < SW6_SCENES; i++) {
        s.base = off_dev + i * SW6_OFF_SLOT_STEP;
        if (gfx_surface_set(&s_tab, SW6_SLOT_OFF(i), &s) != 0)
            return -1;
    }
    return 0;
}

/* 三张整屏图预合成进离屏层，并把三个参考 CRC 打出去（spec 2.0 第 2 条）。 */
static int boot_precompose(void)
{
    sw6_b_ctx ctx;
    uint32_t  eq[4];
    uint32_t  bad = 0u;
    uint32_t  i;
    int       rc;

    ctx.tab  = &s_tab;
    ctx.mem  = &s_mem;
    ctx.es   = &s_es;
    ctx.err  = &s_err;
    ctx.cmds = s_cmds[0];
    ctx.cap  = s_cmd_cap[0];

    sw6_b_begin(&s_b, SW6_K_DEFAULT);
    rc = sw6_b_precompose(&s_b, &ctx, eq, &bad);
    if (rc != GFX_OK) {
        say2("exec err code/index=", (uint32_t)rc, s_err.index);
        return -1;
    }
    s_fail += bad;

    for (i = 0u; i < SW6_SCENES; i++)
        sw6_log_scene(i, s_b.ref[i]);
    sw6_log_eq('P', eq);
    return 0;
}

static int boot(void)
{
    if (boot_mem() != 0)
        return -1;
    if (boot_asset() != 0)
        return -1;
    if (boot_surfaces() != 0) {
        board_puts("surf err 表面登记失败\r\n");
        return -1;
    }
    if (boot_precompose() != 0)
        return -1;
    return 0;
}

/* --- 一段（一个阶段的一遍）的开始 ---------------------------------------- */

static void stage_begin(uint32_t stage, uint32_t seed, uint32_t n0)
{
    uint32_t i;

    exec_sw_reset(&s_es);
    for (i = 0u; i < 4u; i++)
        s_eq_acc[i] = 0u;

    s_prev_valid = 0;
    s_prev_cmd   = 0u;
    s_prev_pix   = 0u;
    s_last_in    = 0u;
    s_last_bld   = 0u;
    s_last_exe   = 0u;

    s_cyc_acc    = 0u;
    s_sw_fps     = 0u;
    s_fps_swaps0 = board_swap_count();
    s_fps_valid  = 0;

    if (stage == 0u)
        sw6_a_begin(&s_a, seed, n0);
    else
        sw6_b_begin(&s_b, SW6_K_DEFAULT);
}

/* --- 一帧 ---------------------------------------------------------------- */

static void frame_run(uint32_t stage, uint32_t pass, uint32_t frame, sw6_input *in)
{
#ifdef INJECT_SW6_FB_PER_PASS
    /* 反例：退回按每遍的帧号选面，每遍帧数为奇数时双缓冲不变量必须见红 */
    const uint32_t list_idx = frame & 1u;
#else
    const uint32_t list_idx = s_seq & 1u;
#endif
    const int      dst      = list_idx ? SW6_SLOT_FB1 : SW6_SLOT_FB0;
    const uint32_t t_frame  = board_cycles();
    gfx_list        l;
    exec_sw_stats   st;
    sw6_frame_rec   rec;
    sw6_build_stat  fs;
    sw6_hud_in      hud;
    uint32_t        hud_bad0, t0, d_in, d_bld, d_exe, n_field, k_field, idx;
    uint32_t       *fb;
    int             rc;

    /* 双缓冲的不变量：不往正在扫描的那一面上画 */
    if (dst == s_shown) {
        say2("fb err frame/dst=", frame, (uint32_t)dst);
        s_fail++;
    }

    sw6_build_stat_clear(&fs);
    rec.frame = frame;
    rec.stage = (stage == 0u) ? 'A' : 'B';
    rec.pass  = pass;
    rec.scene = -1;

    /* --- 输入更新 --- */
    t0 = board_cycles();
    rec.keys = sw6_input_next(in, frame);
    if (stage == 0u) {
        sw6_a_adjust(&s_a, sw6_input_pressed(in, BOARD_KEY_UP),
                           sw6_input_pressed(in, BOARD_KEY_DOWN));
        sw6_a_update(&s_a);
    } else {
        sw6_b_adjust(&s_b, sw6_input_pressed(in, BOARD_KEY_UP),
                           sw6_input_pressed(in, BOARD_KEY_DOWN));
    }
    d_in = board_cycles() - t0;

    n_field = (stage == 0u) ? s_a.n : 0u;
    k_field = (stage == 0u) ? 0u : s_b.k;

    /* --- 组帧 --- */
    t0 = board_cycles();
    gfx_list_begin(&l, s_cmds[list_idx], s_cmd_cap[list_idx], &s_tab);
    {
        gfx_rect all;

        all.x0 = 0;
        all.y0 = 0;
        all.x1 = (int32_t)FB_WIDTH - 1;
        all.y1 = (int32_t)FB_HEIGHT - 1;

        if (gfx_set_clip(&l, all) != 0)
            fs.bad++;

        if (stage == 0u) {
            /* 打底：一条写满 640×480 可见像素的全屏 FILL（spec 2.0 第 4 条） */
            if (gfx_fill(&l, dst, all, SW6_BG_COLOR, GFX_EQ_COPY, 0u) != 0)
                fs.bad++;
            sw6_a_build(&s_a, &l, dst, &fs);
        } else {
            /* 打底：从离屏图整屏 BLIT COPY，源与目的都是 640×480，写满即覆盖 */
            rec.scene = (int32_t)sw6_b_step(&s_b);
            if (gfx_blit(&l, SW6_SLOT_OFF(rec.scene), 0, 0, dst, 0, 0,
                         (int)FB_WIDTH, (int)FB_HEIGHT, GFX_EQ_COPY, 0u, 0, 0u) != 0)
                fs.bad++;
        }

        hud.n          = n_field;
        hud.k          = k_field;
        hud.sw_fps       = s_sw_fps;
        hud.sw_fps_valid = s_fps_valid;
        hud.cyc_in     = s_last_in;
        hud.cyc_bld    = s_last_bld;
        hud.cyc_exe    = s_last_exe;
        hud.live       = SW6_HUD_LIVE;
        hud.prev_cmd   = s_prev_cmd;
        hud.prev_pix   = s_prev_pix;
        hud.prev_valid = s_prev_valid;

        hud_bad0 = sw6_hud_bad();
        sw6_hud_build(&l, dst, &hud, &rec.hud_cmd, &rec.hud_pix);
        fs.bad += sw6_hud_bad() - hud_bad0;

        if (gfx_list_end(&l) != 0)
            fs.bad++;
        s_fail += fs.bad + l.rejected + l.overflow;
    }
    d_bld = board_cycles() - t0;

    /* --- 执行 --- */
    exec_sw_stats_clear(&st);
    t0 = board_cycles();
    rc = exec_sw_run(&s_es, &s_tab, &s_mem, l.buf, l.used, l.used, &st, &s_err);
    d_exe = board_cycles() - t0;
    if (rc != GFX_OK) {
        say2("exec err code/index=", (uint32_t)rc, s_err.index);
        s_fail++;
    }

    for (idx = 0u; idx < 4u; idx++)
        s_eq_acc[idx] += fs.eq[idx];

    /* --- 三个 CRC 与日志 --- */
    rec.n       = n_field;
    rec.k       = k_field;
    rec.cmd     = st.cmd;
    rec.pix     = st.pixel_written;
    rec.ksk     = st.key_skip;
    rec.off     = fs.off;
    rec.cyc_in  = d_in;
    rec.cyc_bld = d_bld;
    rec.cyc_exe = d_exe;

    fb = board_fb(dst);
    rec.crc  = sw6_crc_full(fb);
    rec.scrc = sw6_crc_scene(fb);
    rec.hcrc = sw6_crc_hud(fb);
    sw6_log_frame(&rec);

    s_last_in    = d_in;
    s_last_bld   = d_bld;
    s_last_exe   = d_exe;
    s_prev_cmd   = st.cmd;
    s_prev_pix   = st.pixel_written;
    s_prev_valid = 1;

    /* --- 呈现：它自己等掉一场 --- */
    board_present(dst);
    s_shown = dst;
    s_seq++;

    /* SW FPS 的窗口：累加整帧周期，满一秒结算一次（board_cycles 是 32 位回绕的，
       逐段累加，不拿首尾两个读数相减） */
    s_cyc_acc += (uint32_t)(board_cycles() - t_frame);
    if (s_cyc_acc >= (uint64_t)BOARD_HZ) {
        const uint32_t now = board_swap_count();

        s_sw_fps     = now - s_fps_swaps0;
        s_fps_swaps0 = now;
        s_fps_valid  = 1;
        s_cyc_acc   -= (uint64_t)BOARD_HZ;
    }
}

/* --- 判据模式 ------------------------------------------------------------ */

#if SW6_MODE_JUDGE

static int run_judge(void)
{
    static const uint8_t *const bufs[2] = { sw6_a_bytes, sw6_b_bytes };
    static const uint32_t       lens[2] = { SW6_A_LEN, SW6_B_LEN };
    uint32_t stage;

    for (stage = 0u; stage < 2u; stage++) {
        uint32_t pass;

        if (SW6_ONLY_STAGE != 0 && (uint32_t)SW6_ONLY_STAGE != stage + 1u)
            continue;

        for (pass = 1u; pass <= 2u; pass++) {
            sw6_input in;
            uint32_t  frames = 0u;
            uint32_t  limit, f;
            int       rc = sw6_input_replay(&in, bufs[stage], lens[stage], &frames);

            if (rc != 0) {
                board_puts("replay err 录像打不开\r\n");
                s_fail++;
                break;
            }
            stage_begin(stage, in.seed, SW6_N_START);

            limit = frames;
            if (SW6_FRAMES_MAX != 0 && limit > (uint32_t)SW6_FRAMES_MAX)
                limit = (uint32_t)SW6_FRAMES_MAX;

            for (f = 0u; f < limit; f++)
                frame_run(stage, pass, f, &in);

            if (stage == 0u)
                sw6_log_eq('A', s_eq_acc);
        }
    }

    sw6_log_end(s_fail);
    return (s_fail == 0u) ? 0 : 1;
}

/* --- 演示模式 ------------------------------------------------------------ */

#else   /* !SW6_MODE_JUDGE */

static int run_demo(void)
{
    sw6_input in;
    uint32_t  stage       = 0u;
    uint32_t  pass_frame  = 0u;
    uint32_t  stage_frame = 0u;
    uint32_t  total       = 0u;

    sw6_input_live(&in, SW6_SEED_DEMO);
    stage_begin(0u, SW6_SEED_DEMO, SW6_N_START);

    for (;;) {
        if (board_should_quit())
            break;
#if SW6_FRAMES_MAX != 0
        if (total >= (uint32_t)SW6_FRAMES_MAX)
            break;
#endif
        if (stage_frame >= SW6_STAGE_FRAMES) {
            stage ^= 1u;
            stage_frame = 0u;
            pass_frame  = 0u;
            stage_begin(stage, SW6_SEED_DEMO, SW6_N_START);
        }
        frame_run(stage, 1u, pass_frame, &in);
        pass_frame++;
        stage_frame++;
        total++;
    }

    sw6_log_end(s_fail);
    return (s_fail == 0u) ? 0 : 1;
}

#endif  /* SW6_MODE_JUDGE */

/* --- 弹幕场入口（SW7 2.6 节）--------------------------------------------- */

#if STG_FIELD_MODE != 0

static int run_stg(void)
{
    stg_main_ctx ctx;

    if (boot_mem() != 0 || boot_asset() != 0) {
        sw6_log_end(s_fail + 1u);
        return 1;
    }
    if (boot_surfaces() != 0) {
        board_puts("surf err 表面登记失败\r\n");
        sw6_log_end(s_fail + 1u);
        return 1;
    }

    ctx.tab     = &s_tab;
    ctx.mem     = &s_mem;
    ctx.es      = &s_es;
    ctx.err     = &s_err;
    ctx.cmds[0] = s_cmds[0];
    ctx.cmds[1] = s_cmds[1];
    ctx.cap[0]  = s_cmd_cap[0];
    ctx.cap[1]  = s_cmd_cap[1];
    return stg_main(&ctx, s_fail);
}

#endif /* STG_FIELD_MODE != 0 */

/* --- 入口 ---------------------------------------------------------------- */

int main(void)
{
    board_mem cmdA, cmdB;

    board_init();
    crc32_init();
#if STG_FIELD_MODE != 0
    board_puts("gfx2dGame STG\r\n");
#else
    board_puts("gfx2dGame SW6\r\n");
#endif

    cmdA = board_region(REGION_CMDLIST_A);
    cmdB = board_region(REGION_CMDLIST_B);
    s_cmds[0]    = (gfx_cmd *)cmdA.ptr;
    s_cmd_cap[0] = cmdA.size / (uint32_t)sizeof(gfx_cmd);
    s_cmds[1]    = (gfx_cmd *)cmdB.ptr;
    s_cmd_cap[1] = cmdB.size / (uint32_t)sizeof(gfx_cmd);

#if STG_FIELD_MODE != 0
    /* 下面 SW6 的启动与主循环不走；留在原处，SW6 的静态函数仍被引用，不触发未用告警 */
    return run_stg();
#endif

    if (boot() != 0) {
        sw6_log_end(s_fail + 1u);
        return 1;
    }

#if SW6_MODE_JUDGE
    return run_judge();
#else
    return run_demo();
#endif
}
