/******************************************************************************
* @file main.c: gfx2dGame —— 模式框架、基础 Demo 与游戏的入口
*
* @brief  赛题基础要求 3 的基础 Demo。
*         同一份应用每帧更新场景、生成命令表、交给 `lib/gfx` 的软件执行器、呈现后缓冲，
*         并逐帧打一行日志。两个场景：
*           A 大量方块随机移动：N=100~5000，每帧 N 条场景命令，瓶颈在命令数；
*           B 三张整屏图快速切换：初始化时预合成进 DDR 离屏层，每帧一次整屏 BLIT COPY，
*             瓶颈在访存带宽。
*
*         入口（编译期开关，板上没有 argv）：
*           不给开关       模式框架：1 基础 Demo（演示，实时输入，A／B 每 SW6_STAGE_FRAMES 场轮换）、
*                          2 游戏、3 压力测试（stg_modes.c）；上电进 -DGFX2D_START_MODE=<1|2|3>（缺省 2）。
*                          数字键 1~3 直接选模式，Tab 切后端变量；板上用和弦（上 + 下按住 8 帧切下一个模式，
*                          左 + 右切后端，stg_chord）。切换在触发那一帧结束之后生效：离开时游戏状态整体重置，
*                          进入时 exec_sw_reset 复位裁剪框、按新模式重登记表面表；新模式的第一帧之后打一行
*                            mode <m> enter frame <帧号> crc <整帧 CRC32> pool <四个池的存活数> clip <裁剪框>
*                          -DGFX2D_FRAMES=N 限总帧数（不给时沿用 -DSW6_FRAMES）；-DGFX2D_SWITCH_JUDGE=1 用合成的
*                          按键脚本来回切 150 次（切换判据，check_sw8_stats.sh）。
*                          每 60 场一行统计 CSV（gfx2d_stats.c）；CSV 与框架自己的行都进日志队列，
*                          每帧只取出 board_tx_room() 那么多字节。模式 1 里基础 Demo 的逐帧日志照旧直接打，
*                          队列在模式 1 的每帧开头先取空（不让半行被夹断）。
*           -DSW6_JUDGE=1  判据模式：不读实时按键，按入库的两段录像各回放两遍，
*                          先打 pak／scene／eq P，再逐帧打 f 行，最后 fail／done。不走模式框架。
*
*         判据与检查脚本见 tools/check/check_sw6_demo.sh；期望日志入库
*         `logs/sw6_judge.log` 与 `logs/sw6_judge_quick.log`。
*
*         -DSTG_FIELD=<n> 时换成弹幕场入口（stg_main.c）：
*         启动只做分区窗口、素材包与表面槽三步，不做预合成；表面槽用游戏那一套（stg_slots.h）。
*         基础 Demo 的两种构建不受影响。
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

#include "game/stg_chord.h"

#ifdef __riscv
#include "gfx_engine.h"
#include "gfx_regs.h"
#endif

#include "gfx2d_stats.h"
#include "r4.h"
#include "r4_atlas.h"
#include "stg_main.h"
#include "stg_modes.h"
#include "stg_slots.h"
#include "renderer.h"
#include "frontend_game.h"
#include "stg_runtime.h"
static stg_runtime s_runtime;
#ifdef GFX2D_R4_REPLAY
#include "r4_replay.h"
#endif
#include <string.h>

/* --- 全局状态（都是静态的，不用 malloc）--------------------------------- */

static sw6_scene_a       s_a;
static sw6_scene_b       s_b;
#if !SW6_MODE_JUDGE
#endif
#if !SW6_MODE_JUDGE || SW9_PERF
static uint32_t          s_backend_fps[4], s_backend_fps_valid[4];
#endif

/* 上一帧最终的统计与三段耗时：HUD 显示的就是它们 */
static int      s_prev_valid;
static uint32_t s_prev_cmd;
static uint32_t s_prev_pix;
static uint32_t s_last_in;
static uint32_t s_last_bld;
static uint32_t s_last_exe;

/* 上一帧的命令表与执行器统计：模式框架的统计埋点抽样用，基础 Demo 自己不看它们 */
static uint32_t      s_last_list;
static uint32_t      s_last_used;
static exec_sw_stats s_last_xs;
static uint32_t      s_last_tp;         /* 发出呈现请求前一刻的 board_cycles()（帧时间的终点） */

/* 上一次交给 board_present 的那一面（-1 表示还没呈现过）。present 等切换完成才返回，
   所以它就是正在扫描的那一面：本帧的目的面必须是另一面，否则板上会撕裂。 */
static int s_shown = -1;

/* 全局的帧序号：目的面与命令表都按它的奇偶轮换，**不随每段每遍的帧号归零**。
   按每遍的帧号选面时，一遍的帧数是奇数（B 段 367 帧）就会在下一遍第 0 帧
   画到正在扫描的那一面上（双缓冲的要求）。 */
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

/* 按分区表算 gfx_mem 的窗口：素材区起点到整个离屏分区末端。
   两个数都由分区表与 FB_BYTES 在运行时算出，代码里不写死；
   同时核对相邻分区的指针差等于分区表的偏移差（assetTest 的 chk M1 那一套）。 */

/* 装载素材包、与生成的头对账、打 `pak` 行。 */

/* 表面槽：0、1 是两张帧缓冲，2~9 是素材 8 项，10~12 是三张离屏图。 */

/* 三张整屏图预合成进离屏层，并把三个参考 CRC 打出去。 */
static int boot_precompose(void)
{
    sw6_b_ctx ctx;
    uint32_t  eq[4];
    uint32_t  bad = 0u;
    uint32_t  i;
    int       rc;

    ctx.tab  = &s_runtime.tab;
    ctx.mem  = &s_runtime.mem;
    ctx.es   = &s_runtime.es;
    ctx.err  = &s_runtime.err;
    ctx.cmds = s_runtime.cmds[0];
    ctx.cap  = s_runtime.cap[0];

    sw6_b_begin(&s_b, SW6_K_DEFAULT);
    rc = sw6_b_precompose(&s_b, &ctx, eq, &bad);
    if (rc != GFX_OK) {
        say2("exec err code/index=", (uint32_t)rc, s_runtime.err.index);
        return -1;
    }
    s_runtime.fails += bad;

    for (i = 0u; i < SW6_SCENES; i++)
        sw6_log_scene(i, s_b.ref[i]);
    sw6_log_eq('P', eq);
    return 0;
}

static int boot(void)
{
    if (stg_runtime_boot_mem(&s_runtime) != 0)
        return -1;
    if (stg_runtime_boot_asset(&s_runtime) != 0)
        return -1;
    if (stg_runtime_surfaces_demo(&s_runtime) != 0) {
        board_puts("surf err 表面登记失败\r\n");
        return -1;
    }
    if (boot_precompose() != 0)
        return -1;
    return 0;
}

#if !SW6_MODE_JUDGE || STG_FIELD_MODE != 0

/* 游戏的表面槽（stg_slots.h）：0、1 是两张帧缓冲，2~14 是字库与 12 张霓虹素材。
   不登记离屏图：游戏没有预合成。 */

#endif /* 模式框架或弹幕场入口 */

/* --- 一段（一个阶段的一遍）的开始 ---------------------------------------- */

static void stage_begin(uint32_t stage, uint32_t seed, uint32_t n0)
{
    uint32_t i;

    exec_sw_reset(&s_runtime.es);
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
#if !SW9_PERF
    uint32_t       *fb;
#endif
    int             rc;

    /* 双缓冲的不变量：不往正在扫描的那一面上画 */
    if (!renderer_pipeline_enabled((int)s_runtime.backend) && dst == s_shown) {
        say2("fb err frame/dst=", frame, (uint32_t)dst);
        s_runtime.fails++;
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
    gfx_list_begin(&l, s_runtime.cmds[list_idx], s_runtime.cap[list_idx], &s_runtime.tab);
    {
        gfx_rect all;

        all.x0 = 0;
        all.y0 = 0;
        all.x1 = (int32_t)FB_WIDTH - 1;
        all.y1 = (int32_t)FB_HEIGHT - 1;

        if (gfx_set_clip(&l, all) != 0)
            fs.bad++;

        if (stage == 0u) {
            /* 打底：一条写满 640×480 可见像素的全屏 FILL */
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
#if SW9_PERF
        hud.sw_fps       = s_backend_fps[0];
        hud.sw_fps_valid = (int)s_backend_fps_valid[0];
        hud.hw_fps       = s_backend_fps[s_runtime.backend ? s_runtime.backend : 1u];
        hud.hw_fps_valid = (int)s_backend_fps_valid[s_runtime.backend ? s_runtime.backend : 1u];
#else
        /* 只有一个窗口，实测值按当前后端归栏；判据只走软件，HUD 与基础 Demo 期望日志不变 */
        hud.sw_fps       = s_sw_fps;
        hud.sw_fps_valid = s_fps_valid && s_runtime.backend == 0u;
        hud.hw_fps       = s_sw_fps;
        hud.hw_fps_valid = s_fps_valid && s_runtime.backend != 0u;
#endif
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
        s_runtime.fails += fs.bad + l.rejected + l.overflow;
    }
    d_bld = board_cycles() - t0;

    /* --- 执行 --- */
    if (renderer_pipeline_enabled((int)s_runtime.backend)) {
        int presented;
        rc = renderer_pipeline_step(&l, dst, &st, &d_exe, &s_last_tp, &presented);
        if (presented >= 0)
            s_shown = presented;
    } else {
        rc = renderer_run((int)s_runtime.backend, &s_runtime.tab, &s_runtime.mem, &s_runtime.es, &s_runtime.err,
                          &l, &st, &d_exe);
    }
    if (rc != GFX_OK) {
        say2("exec err code/index=", (uint32_t)rc, s_runtime.err.index);
        s_runtime.fails++;
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

#if !SW9_PERF
    if (s_runtime.backend != 0u)
        board_dcache_invalidate_all();
    fb = board_fb(dst);
    rec.crc  = sw6_crc_full(fb);
    rec.scrc = sw6_crc_scene(fb);
    rec.hcrc = sw6_crc_hud(fb);
    sw6_log_frame(&rec);
#endif

    s_last_in    = d_in;
    s_last_bld   = d_bld;
    s_last_exe   = d_exe;
    s_last_list  = list_idx;
    s_last_used  = l.used;
    s_last_xs    = st;
    s_prev_cmd   = st.cmd;
    s_prev_pix   = st.pixel_written;
    s_prev_valid = 1;

    /* --- 呈现：它自己等掉一场 --- */
    if (!renderer_pipeline_enabled((int)s_runtime.backend)) {
        uint32_t ticket;
        s_last_tp = board_cycles();
        if (board_present_request(dst, &ticket) != 0 || board_present_wait(ticket) != 0) {
            say2("present err frame/dst=", frame, (uint32_t)dst);
            s_runtime.fails++;
        } else {
            s_shown = dst;
        }
    }
    s_seq++;

    /* SW FPS 的窗口：累加整帧周期，满一秒结算一次（board_cycles 是 32 位回绕的，
       逐段累加，不拿首尾两个读数相减） */
    s_cyc_acc += (uint32_t)(board_cycles() - t_frame);
    if (s_cyc_acc >= (uint64_t)BOARD_HZ) {
        const uint32_t now = board_swap_count();

        s_sw_fps     = now - s_fps_swaps0;
#if SW9_PERF
        s_backend_fps[s_runtime.backend] = s_sw_fps;
        s_backend_fps_valid[s_runtime.backend] = 1u;
#endif
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
                s_runtime.fails++;
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

    sw6_log_end(s_runtime.fails);
    return (s_runtime.fails == 0u) ? 0 : 1;
}

/* --- 演示模式 ------------------------------------------------------------ */

#else   /* !SW6_MODE_JUDGE */

/* --- 模式框架（默认构建）--------------------------------------------------- */

/* 上电进哪个模式（2026-09-26 定案）：缺省 2（游戏） */
#ifndef GFX2D_START_MODE
#define GFX2D_START_MODE    2
#endif
#ifndef GFX2D_START_BACKEND
#define GFX2D_START_BACKEND 0
#endif
#if GFX2D_START_BACKEND < 0 || GFX2D_START_BACKEND > 3
#error "GFX2D_START_BACKEND 只认 0（软件）、1（硬件/DDR）、2（硬件/TEX）或 3（硬件/STRIP）"
#endif
#if GFX2D_START_MODE < 1 || GFX2D_START_MODE > 4
#error "GFX2D_START_MODE 只认 1、2、3、4"
#endif

/* 总帧数上限：-DGFX2D_FRAMES=N，不给时沿用 -DSW6_FRAMES（D2b 靠它结束），都不给时不限。
   与 SW6_FRAMES_MAX 同一个理由不带 (uint32_t)。 */
#ifdef GFX2D_FRAMES
#define GFX2D_FRAMES_MAX    GFX2D_FRAMES
#else
#define GFX2D_FRAMES_MAX    SW6_FRAMES_MAX
#endif
#ifndef GFX2D_FIELDS_MAX
#define GFX2D_FIELDS_MAX    0
#endif

/* 切换判据：合成的按键脚本代替实时按键 */
#ifndef GFX2D_SWITCH_JUDGE
#define GFX2D_SWITCH_JUDGE  0
#endif
#define SWJ_SWITCHES        150u    /* 数字键直达 1~3 与和弦循环（现含模式 4）混合来回 50 次 */

/* 模式 3 的两遍搜索结束之后就退出（检查脚本用；平时停在结果那一档继续跑） */
#ifndef GFX2D_PRESS_EXIT
#define GFX2D_PRESS_EXIT    0
#endif
#ifndef GFX2D_PRESS_AB_BIN
#define GFX2D_PRESS_AB_BIN 0
#endif
#define SWJ_DWELL           5u      /* 每次切进来之后先停几帧不按键 */
#define SWJ_DWELL_PRESS     60u     /* 模式 3 多停：旋钮 N0 = 64 时门控约 47 帧才攒够一次 24 发的齐射，
                                       停到池里有弹再切走，「切换时漏清池」才看得出来 */

static stg_chord    s_chord;
static uint32_t     s_mode;             /* 当前模式 1~4 */
static uint32_t     s_mode_frames;      /* 进入当前模式以来的帧数 */
static gfx_rect     s_enter_clip;       /* 进入时复位之后的裁剪框（进入行里打） */
static sw6_input    s_m1_in;            /* 模式 1：由框架喂的输入 */
static uint32_t     s_m1_stage;
static uint32_t     s_m1_frame;         /* 模式 1：本段的帧号 */
static uint32_t     s_m1_field0;        /* 模式 1：本段开始时的场号（轮换按场数） */
static uint32_t     s_mfps_cyc0, s_mfps_swaps0, s_mfps;
static uint32_t     s_swj_n;            /* 切换判据：已经切了几次 */

/* --- 模式 4「三维演示」----------------------------------------------------- */

/* 模式键组合（R4 定案）：1+2+3 三个键同时按住 = 直接选 4；10 位旧包格式不变。 */
#define R4_MODE_COMBO       (BOARD_KEY_MODE1 | BOARD_KEY_MODE2 | BOARD_KEY_MODE3)
static int          s_combo_prev;       /* 组合的按下沿检测 */
static uint32_t     s_r4_ladder = GFX2D_R4_LADDER;
static uint32_t     s_r4_hw = GFX2D_R4_HW;
static uint32_t     s_r4_scene = GFX2D_R4_SCENE;
static r4_out       s_r4_out;
static int          s_r4_restart;       /* 模式 4 内 SWAP/SLOW 的帧边界重进请求 */
static int          s_r4_ready;
#ifdef GFX2D_R4_SWITCH_JUDGE
static uint32_t     s_r4_preserved_crc;
#endif

#ifdef __riscv
#endif

/* R4 的表面槽：帧缓冲 0、1，字库 5/14，大数字 12，离屏背景 15（stg 空着的槽）。 */

/* R4 钩子：装载 atlas——两个平台都先把生成词拷进 rast_ref 的影子，
   板上再写 TEX 窗口并读回 CRC 对账。 */

/* --- v1-TEX：热集装载与 HW/TEX 后端 --------------------------------------- */

/* 热集 = 标准圆弹/小圆弹/火花（游戏口径），按现行 manifest 顺序紧凑摆进
   16 KiB 窗口；装载一次、读回 CRC 自校验，通过后 HW/TEX 后端才可用。
   装载、读回与重登记都要求引擎空闲（驱动纪律：驱动层拒绝忙时访问），
   上传只在启动与重载时发生，不进每帧循环、不藏在引擎计时之外。 */

#if (defined(G1_TEX_COMPARE) && G1_TEX_COMPARE) || (defined(G4_STRIP_COMPARE) && G4_STRIP_COMPARE)
#endif

/* 上电装载一次：写 APB 窗口、分块读回、与素材区内容逐字节 CRC 对账；
   不符不启用（该次上电 HW/TEX 不可用，其余后端照常）。 */
#ifdef __riscv

#endif

/* 热集槽在 DDR 基址与 TEX 偏移之间改写（HW/TEX 后端的覆盖层）。
   表面表重建（进模式/切模式）会把表打回 DDR，那时 s_runtime.tex_on 已被清 0。 */

/* 后端切换前的表面表准备；HW/TEX 不可用或覆盖层失败返回 -1（调用方退回 HW/DDR）。 */

/* 统计埋点与日志队列：框架自己的行都进队列，每帧只取出 board_tx_room() 那么多字节 */
#define LOGQ_BYTES          8192u
#define LOG_TAKE_MAX        256u
static logq         s_logq;
static char         s_logq_mem[LOGQ_BYTES];
static char         s_log_out[LOG_TAKE_MAX + 1u];
static stats_state  s_stats;
static stg_mode_out s_mout;             /* 模式 2、3 最近一帧的输出（抽样用） */
#if SW9_PERF
static uint32_t     s_diag_min, s_diag_last_frame, s_diag_samples;
static uint32_t     s_diag_zero, s_diag_under;
#endif

static void log_line(const char *s)
{
    (void)logq_put(&s_logq, s);
}

/* 每帧：取出不超过 board_tx_room() 的字节交给 board_puts。 */
static void log_pump(void)
{
    const uint32_t room = board_tx_room();
    uint32_t       n = (room < LOG_TAKE_MAX) ? room : LOG_TAKE_MAX;

    if (n == 0u || logq_used(&s_logq) == 0u)
        return;
    n = logq_take(&s_logq, s_log_out, n);
    if (n > room) {
        s_runtime.fails++;           /* 自查：取出的不超过空位（按构造恒成立，留这一道闸给以后改取法的人） */
        board_puts("err log over room\r\n");
    }
    board_puts(s_log_out);
}

/* 模式 1：基础 Demo 的逐帧日志照旧直接打（会阻塞），所以队列在模式 1 的每帧开头先取空，
   不让上一帧按空位取出的半行被基础 Demo 的行夹断。 */
static void log_drain(void)
{
    while (logq_used(&s_logq) != 0u) {
        (void)logq_take(&s_logq, s_log_out, LOG_TAKE_MAX);
        board_puts(s_log_out);
    }
}

/* 运行结束：把队列里剩下的全取出来，空位不够就等下一场。 */
static void log_flush(void)
{
    while (logq_used(&s_logq) != 0u) {
        if (board_tx_room() == 0u)
            board_wait_frame();
        log_pump();
        if (board_should_quit())
            break;
    }
}

static stg_main_ctx mode_ctx(void)
{
    stg_main_ctx c;

    c.tab     = &s_runtime.tab;
    c.mem     = &s_runtime.mem;
    c.es      = &s_runtime.es;
    c.err     = &s_runtime.err;
    c.cmds[0] = s_runtime.cmds[0];
    c.cmds[1] = s_runtime.cmds[1];
    c.cap[0]  = s_runtime.cap[0];
    c.cap[1]  = s_runtime.cap[1];
    c.hw      = (int)s_runtime.backend;
    return c;
}

static void mode_enter(uint32_t m)
{
    const uint32_t previous_mode=s_mode;
    if(frontend_game_reset()) {
        board_puts("frontend lease busy\r\n");s_runtime.fails++;return;
    }
    exec_sw_reset(&s_runtime.es);
    s_enter_clip  = s_runtime.es.clip;
    s_mode        = m;
    s_mode_frames = 0u;
    s_mfps_cyc0    = board_cycles();
    s_mfps_swaps0  = board_swap_count();
    s_mfps         = 0u;
    if (m == 1u) {
        if (stg_runtime_surfaces_demo(&s_runtime) != 0) {
            board_puts("surf err 表面登记失败\r\n");
            s_runtime.fails++;
        }
        sw6_input_fed(&s_m1_in, SW6_SEED_DEMO);
        s_m1_stage  = 0u;
        s_m1_frame  = 0u;
        s_m1_field0 = board_frame_count();
        stage_begin(0u, SW6_SEED_DEMO, SW6_N_START);
    } else if (m == 4u) {
        s_r4_ready=0;
#ifdef GFX2D_R4_SWITCH_JUDGE
        s_r4_preserved_crc=crc32_ieee(board_region(REGION_OFFSCREEN).ptr,R4_BG_OFFSET);
#endif
        r4_scene_select(s_r4_scene);
        if (stg_runtime_surfaces_r4(&s_runtime) != 0) {
            board_puts("surf err 表面登记失败\r\n");
            s_runtime.fails++;
        }
        /* 先复位引擎并登记 R4 的表面表（独立背景、字库、两张帧缓冲），
           再让 r4 写 TEX 窗口（R3 板测的同序：prepare -> tex）。 */
        if (renderer_prepare(1, &s_runtime.tab) != 0) {
            board_puts("engine prepare err\r\n");
            s_runtime.fails++;return;
        }
        /* 模式 4 自管引擎提交（串行 submit->wait->present），不走 renderer。 */
        if (r4_mode_enter(&s_runtime.tab, &s_runtime.mem, (int)s_r4_hw, s_r4_ladder) != 0) {
            board_puts("r4 enter err\r\n");
            s_runtime.fails++;return;
        }
        s_r4_ready=1;
        /* Present a cleared inactive buffer before clearing the old front.
           Timing starts only after the display warmup and statistics clear. */
        {
            const int blank = s_shown == STG_SLOT_FB0 ? STG_SLOT_FB1 : STG_SLOT_FB0;
            memset(board_fb(blank),0,FB_BYTES);
            board_present(blank);
            memset(board_fb(blank^1),0,FB_BYTES);
            board_wait_frame();
            (void)board_display_stats_clear();
            r4_timing_start();
            s_shown = blank;
            s_seq = (s_seq & ~1u) | (uint32_t)(blank^1);
        }
        return;                 /* 模式 4 不走尾部的 backend_apply/prepare */
    } else {
        if (stg_runtime_surfaces_stg(&s_runtime) != 0) {
            board_puts("surf err 表面登记失败\r\n");
            s_runtime.fails++;
        }
        if (m == 2u)
            stg_mode_game_enter();
        else
            stg_mode_press_enter();
    }
    if (s_runtime.backend >= 2u && stg_runtime_backend_apply(&s_runtime,s_runtime.backend,s_mode) != 0) {
        board_puts("tex err 覆盖层失败，退回 HW/DDR\r\n");
        s_runtime.backend = 1u;
    }
    if (renderer_prepare((int)s_runtime.backend, &s_runtime.tab) != 0) {
        board_puts("engine prepare err\r\n");
        s_runtime.fails++;
    }
    if(previous_mode==4u)stats_init(&s_stats,&s_logq,s_mode,s_runtime.backend,stg_modes_overflow());
}

/* 离开旧模式：游戏状态整体重置（池与计数清零）。模式 1 的状态在下次进入时由 stage_begin 复位。 */
static void mode_leave(uint32_t m)
{
    int presented;
    if (renderer_pipeline_flush(&presented) != 0) {
        board_puts("engine flush err\r\n");
        s_runtime.fails++;
    }
    if (presented >= 0)
        s_shown = presented;
    if (m == 4u) {
        r4_mode_leave();
#ifdef GFX2D_R4_SWITCH_JUDGE
        uint32_t got=crc32_ieee(board_region(REGION_OFFSCREEN).ptr,R4_BG_OFFSET);
        if(got!=s_r4_preserved_crc){s_runtime.fails++;board_puts("r4 err legacy offscreen changed\r\n");}
        else board_puts("R4 LEGACY OFFSCREEN preserved\r\n");
#endif
    } else if (m != 1u)
        stg_modes_reset();
}

/* 模式 2、3 的 SW FPS：累计满一秒（BOARD_HZ 个周期）结算一次最近一秒的呈现次数。 */
static void fps_tick(void)
{
    const uint32_t now = board_cycles();

    if (now - s_mfps_cyc0 >= (uint32_t)BOARD_HZ) {
        const uint32_t sw = board_swap_count();

        s_mfps        = sw - s_mfps_swaps0;
        s_backend_fps[s_runtime.backend] = s_mfps;
        s_backend_fps_valid[s_runtime.backend] = 1u;
        s_mfps_swaps0 = sw;
        s_mfps_cyc0   = now;
    }
}

/* 当前模式的一帧；返回刚呈现的那一面的整帧 CRC。 */
static uint32_t mode_frame(uint32_t keys, int entering)
{
    if (s_mode == 1u) {
        /* 场景 A、B 按场数轮换（SW6_STAGE_FRAMES 在框架里当场数用） */
        if (board_frame_count() - s_m1_field0 >= (uint32_t)SW6_STAGE_FRAMES) {
            int presented;
            if (renderer_pipeline_flush(&presented) != 0) {
                board_puts("engine flush err\r\n");
                s_runtime.fails++;
            }
            if (presented >= 0)
                s_shown = presented;
            s_m1_stage ^= 1u;
            s_m1_frame  = 0u;
            s_m1_field0 = board_frame_count();
            stage_begin(s_m1_stage, SW6_SEED_DEMO, SW6_N_START);
        }
        sw6_input_feed(&s_m1_in, keys);
        frame_run(s_m1_stage, 1u, s_m1_frame, &s_m1_in);
        s_m1_frame++;
    } else if (s_mode == 4u) {
        /* 模式 4：30Hz 逻辑，一帧占两场；present 由 r4 内部按锚定节拍发出。 */
        const int dst = (s_seq & 1u) ? STG_SLOT_FB1 : STG_SLOT_FB0;
        int rc;
        if(!s_r4_ready)return 0u;

        rc = r4_mode_frame(&s_runtime.tab, &s_runtime.mem, dst, keys, &s_r4_out);
        if (rc == 0)
            s_shown = dst;      /* r4 成功路径已确认呈现 */
        /* 统计埋点的公共字段：t_present 近似取返回时刻；cyc 三段照 r4 的五段归并。 */
        s_mout.t_present = board_cycles();
        s_mout.list_idx = 0u;
        s_mout.used = 0u;
        s_mout.cyc_logic = s_r4_out.cyc_logic + s_r4_out.cyc_geom;
        s_mout.cyc_build = s_r4_out.cyc_bin + s_r4_out.cyc_hud;
        s_mout.cyc_exec = s_r4_out.cyc_submit;
        s_mout.cyc_auto = 0u;
        s_mout.st.act.total = s_r4_out.records;
        s_mout.st.act.ebullet = 0u;
        s_mout.presented_dst = (rc == 0) ? dst : -1;
        s_seq++;
        fps_tick();
        return s_r4_out.crc;
    } else {
        const stg_main_ctx ctx = mode_ctx();
        const int          dst = (s_seq & 1u) ? STG_SLOT_FB1 : STG_SLOT_FB0;
        stg_hud_live       live;

        if (!renderer_pipeline_enabled((int)s_runtime.backend) && dst == s_shown) {
            say2("fb err frame/dst=", s_mode_frames, (uint32_t)dst);
            s_runtime.fails++;
        }
        live.sw_fps       = s_backend_fps[0];
        live.sw_fps_valid = (int)s_backend_fps_valid[0];
        live.hw_fps       = s_backend_fps[s_runtime.backend ? s_runtime.backend : 1u];
        live.hw_fps_valid = (int)s_backend_fps_valid[s_runtime.backend ? s_runtime.backend : 1u];
        live.seg_text     = 0;
        if (s_mode == 2u)
            stg_mode_game_frame(&ctx, dst, keys, &live, &s_mout);
        else
            stg_mode_press_frame(&ctx, dst, &live, &s_mout);
        if (s_mout.presented_dst >= 0)
            s_shown = s_mout.presented_dst;
        s_seq++;
        fps_tick();
    }
#if SW9_PERF
    (void)entering;
    return 0u;
#else
    (void)entering;
    if (s_shown < 0)
        s_shown = 0;        /* 首帧失败还没呈现过任何面：CRC 落在 FB0，不解引用 -1 */
    return stg_modes_frame_crc(board_fb(s_shown));
#endif
}

static void say_enter(uint32_t frame, uint32_t crc)
{
    char     buf[160];
    core_out o;
    uint32_t pool[4], i;

    stg_modes_pools(pool);
    core_out_init(&o, buf, (uint32_t)sizeof buf);
    core_out_str(&o, "mode ");
    core_out_u32(&o, s_mode);
    core_out_str(&o, " enter frame ");
    core_out_u32(&o, frame);
#if SW9_PERF
    (void)crc;
    core_out_str(&o, " crc skip");
#else
    core_out_str(&o, " crc ");
    core_out_hex32(&o, crc);
#endif
    core_out_str(&o, " pool");
    for (i = 0u; i < 4u; i++) {
        core_out_ch(&o, ' ');
        core_out_u32(&o, pool[i]);
    }
    core_out_str(&o, " clip ");
    core_out_u32(&o, (uint32_t)s_enter_clip.x0);
    core_out_ch(&o, ' ');
    core_out_u32(&o, (uint32_t)s_enter_clip.y0);
    core_out_ch(&o, ' ');
    core_out_u32(&o, (uint32_t)s_enter_clip.x1);
    core_out_ch(&o, ' ');
    core_out_u32(&o, (uint32_t)s_enter_clip.y1);
    core_out_str(&o, "\r\n");
    log_line(buf);
}

/* 本帧的按键：实时按键，或切换判据的合成脚本（done 置 1 表示脚本走完）。 */
static uint32_t frame_keys(int *done)
{
#ifdef GFX2D_R4_SWITCH_JUDGE
    static uint32_t step;
    const uint32_t at=step++;
    (void)done;
    switch(at) {
    case 5:case 25:case 30:case 50:return R4_MODE_COMBO;
    case 10:return BOARD_KEY_SLOW;
    case 15:case 20:return BOARD_KEY_SWAP;
    case 35:case 55:return BOARD_KEY_MODE2;
    case 40:return BOARD_KEY_MODE3;
    case 45:return BOARD_KEY_MODE1;
    default:return 0;
    }
#endif
#if GFX2D_SWITCH_JUDGE
    const uint32_t target = s_mode % 3u + 1u;
    const uint32_t mkey[4] = { 0u, BOARD_KEY_MODE1, BOARD_KEY_MODE2, BOARD_KEY_MODE3 };

    if (s_mode_frames < ((s_mode == 3u) ? SWJ_DWELL_PRESS : SWJ_DWELL))
        return 0u;
    if (s_swj_n >= SWJ_SWITCHES) {
        *done = 1;
        return 0u;
    }
    /* 偶数次用数字键（按一帧），奇数次用上 + 下的和弦（按住 8 帧） */
    if ((s_swj_n & 1u) == 0u)
        return mkey[target];
    return BOARD_KEY_UP | BOARD_KEY_DOWN;
#else
    (void)done;
    return board_keys();
#endif
}

/* 这一秒的抽样帧：普查刚执行完的那张命令表，连同执行器统计与三段周期数。 */
static void sample_frame(void)
{
    stats_sample smp;
    uint32_t     idx, used;

    if (s_mode == 1u) {
        idx            = s_last_list;
        used           = s_last_used;
        smp.xs         = s_last_xs;
        smp.n_active   = (s_m1_stage == 0u) ? s_a.n : 0u;
        smp.n_bullet   = 0u;
        smp.cyc_logic  = s_last_in;
        smp.cyc_build  = s_last_bld;
        smp.cyc_exec   = s_last_exe;
    } else if (s_mode == 4u) {
        /* 模式 4 没有可普查的 2D 命令表：方程列记零，五段耗时照实报。 */
        gfx_census empty;

        memset(&empty, 0, sizeof empty);
        idx            = 0u;
        used           = 0u;
        memset(&smp.xs, 0, sizeof smp.xs);
        smp.n_active   = s_r4_out.records;
        smp.n_bullet   = 0u;
        smp.cyc_logic  = s_mout.cyc_logic;
        smp.cyc_build  = s_mout.cyc_build;
        smp.cyc_exec   = s_mout.cyc_exec;
        stats_sample_counted(&s_stats, &empty, &smp);
        return;
    } else {
        idx            = s_mout.list_idx;
        used           = s_mout.used;
        smp.xs         = s_mout.xs;
        smp.n_active   = s_mout.st.act.total;
        smp.n_bullet   = s_mout.st.act.ebullet;
        smp.cyc_logic  = s_mout.cyc_logic;
        smp.cyc_build  = s_mout.cyc_build;
        smp.cyc_exec   = s_mout.cyc_exec;
    }
    smp.cyc_auto = (s_mode == 2u) ? s_mout.cyc_auto : 0u;
    smp.hw_real = 0;
    {
        uint32_t i;
        for (i = 0u; i < 9u; i++)
            smp.hw_perf[i] = 0u;
    }
#ifdef __riscv
    if (s_runtime.backend != 0u) {
        smp.hw_real = 1;
        if (renderer_perf_read(smp.hw_perf) != 0)
            s_runtime.fails++;
    }
#endif
    if (s_mode != 1u && (s_runtime.backend == 3u || s_mout.instances)) {
        gfx_list view;
        gfx_census cc;
        gfx_list_begin(&view, s_runtime.cmds[idx], s_runtime.cap[idx], &s_runtime.tab);
        if(s_runtime.backend==3u && !s_mout.instances) {
            view.strip_table = (uint8_t *)(void *)s_runtime.cmds[idx];
            view.strips = (gfx_strip_config){480u >> GFX2D_STRIP_LOG2,
                                             GFX2D_STRIP_LOG2, idx, idx};
        } else {
            view.instances=s_mout.instances;
            view.instance_generation=s_mout.instance_generation;
        }
        view.used = used;
#if GFX_FRONTEND_BACKGROUND
        view.frame=s_mout.frame_plan;
#endif
        if (renderer_list_census(&s_runtime.tab, &view, &cc) != 0)
            s_runtime.fails++;
        stats_sample_counted(&s_stats, &cc, &smp);
    } else
        stats_sample_list(&s_stats, &s_runtime.tab, s_runtime.cmds[idx], used, &smp);
}

static int run_modes(void)
{
    uint32_t total = 0u, prev = 0u, fail;
#if GFX2D_FIELDS_MAX != 0
    const uint32_t field_start = board_frame_count();
#endif
    int      done = 0, entered;

    logq_init(&s_logq, s_logq_mem, LOGQ_BYTES);
    stg_modes_set_log(&s_logq);
    r4_set_log(&s_logq);
    stg_runtime_set_active(&s_runtime);stg_runtime_r4_install_hooks();
#ifdef GFX2D_R4_REPLAY
    if(r4_set_replay(r4_replay_data,sizeof r4_replay_data)) {s_runtime.fails++;return 1;}
#endif
    stg_chord_init(&s_chord);
#ifdef __riscv
    uint32_t tex_caps = 0u;
    if (gfx_engine_caps_read(&tex_caps) == GFX_ENG_OK && !(tex_caps & GFX_CAPS_TEX_M)) {
        s_runtime.tex_ready = 0;
        board_puts("tex unavailable caps\r\n");
    } else if (stg_runtime_tex_load(&s_runtime) != 0) {
        s_runtime.fails++;
        s_runtime.tex_ready = 0;
        board_puts("tex err 热集装载或 CRC 自校验失败，HW/TEX 后端不可用\r\n");
    } else {
        s_runtime.tex_ready = 1;
    }
#else
    s_runtime.tex_ready = 0;
#endif
    s_runtime.backend = (uint32_t)GFX2D_START_BACKEND;
    if (s_runtime.backend >= 2u && !s_runtime.tex_ready)
#ifdef __riscv
        s_runtime.backend = 1u;         /* 装载失败或宿主无引擎：退回 HW/DDR */
#else
        s_runtime.backend = (s_runtime.backend == 3u) ? 3u : 1u; /* 宿主条带后端执行软件 v2。 */
#endif
    s_swj_n   = 0u;
#if SW9_PERF
    s_diag_min = 0xFFFFFFFFu;
    s_diag_samples = 0u;
    s_diag_last_frame = 0u;
    s_diag_zero = 0u;
    s_diag_under = 0u;
#endif
    mode_enter((uint32_t)GFX2D_START_MODE);
    stats_init(&s_stats, &s_logq, s_mode, s_runtime.backend, stg_modes_overflow());
    if(GFX2D_PRESS_AB_BIN)board_puts("f5 backend TEX begin\r\n");
    entered = 1;

    for (;;) {
        uint32_t keys, pressed, ev, next, crc, t0, ft;
        char     buf[64];
        core_out o;

        if (board_should_quit())
            break;
#if GFX2D_FRAMES_MAX != 0
        if (total >= (uint32_t)GFX2D_FRAMES_MAX)
            break;
#endif
#if GFX2D_FIELDS_MAX != 0
        if ((uint32_t)(board_frame_count() - field_start) >= (uint32_t)GFX2D_FIELDS_MAX)
            break;
#endif
        keys = frame_keys(&done);
        if(s_mode==4u)keys|=r4_pending_keys();
        if (done)
            break;
        if (s_mode == 1u)
            log_drain();
        t0      = board_cycles();
        pressed = keys & ~prev;
        prev    = keys;

        /* 系统键：数字键与 Tab 取按下沿，和弦由 stg_chord 按帧计。
           模式 4 的组合键先于单键识别：三个模式键全按住 = 选 4；
           组合按住期间抑制单键模式切换（避免「1+2 按住再按 3」误先进 3）。 */
        {
            const int combo_now = (keys & R4_MODE_COMBO) == R4_MODE_COMBO;

            if (combo_now && !s_combo_prev)
                pressed |= (uint32_t)0x80000000u;   /* 组合按下沿（伪位，不与键位重叠） */
            s_combo_prev = combo_now;
        }
        ev   = stg_chord_step(&s_chord, keys, BOARD_KEY_UP, BOARD_KEY_DOWN, BOARD_KEY_LEFT, BOARD_KEY_RIGHT);
        next = s_mode;
        if ((pressed & 0x80000000u) != 0u) {
            next = 4u;
            if(s_mode==4u){s_r4_scene^=1u;s_r4_restart=1;}
        } else if ((keys & R4_MODE_COMBO) != R4_MODE_COMBO) {
            if (pressed & BOARD_KEY_MODE1)
                next = 1u;
            if (pressed & BOARD_KEY_MODE2)
                next = 2u;
            if (pressed & BOARD_KEY_MODE3)
                next = 3u;
        }
        if (ev & STG_CHORD_NEXT_MODE)
            next = s_mode % 4u + 1u;
        if (s_mode == 4u) {
            /* 模式 4 里 SWAP=硬/软光栅、SLOW=L1/L2 阶梯；next 仍等于 4，
               用重启标志走帧边界重进（next != s_mode 的开关对同模式不生效）。 */
            if ((pressed & BOARD_KEY_SWAP) || (ev & STG_CHORD_SWAP)) {
                s_r4_hw ^= 1u;
                s_r4_restart = 1;
            }
            if (pressed & BOARD_KEY_SLOW) {
                s_r4_ladder = (s_r4_ladder >= 2u) ? 1u : 2u;
                s_r4_restart = 1;
            }
        } else if ((pressed & BOARD_KEY_SWAP) || (ev & STG_CHORD_SWAP)) {
            int presented;
            uint32_t nb = s_runtime.backend + 1u;           /* 软件→DDR→TEX→STRIP→软件 */

            if (nb >= 4u)
                nb = 0u;
            if (nb >= 2u && !s_runtime.tex_ready)
                nb = 0u;                            /* 热集没装载成功：跳过 HW/TEX */
            if (renderer_pipeline_flush(&presented) != 0) {
                board_puts("engine flush err\r\n");
                s_runtime.fails++;
            }
            if (presented >= 0)
                s_shown = presented;
            if(frontend_game_reset()) {
                board_puts("frontend lease busy\r\n");s_runtime.fails++;continue;
            }
            if (stg_runtime_backend_apply(&s_runtime,nb,s_mode) != 0) {
                nb = 1u;                            /* 覆盖层失败：退回 HW/DDR */
                (void)stg_runtime_backend_apply(&s_runtime,nb,s_mode);
            }
            s_runtime.backend = nb;
            if (renderer_prepare((int)s_runtime.backend, &s_runtime.tab) != 0) {
                board_puts("engine prepare err\r\n");
                s_runtime.fails++;
            }
            core_out_init(&o, buf, (uint32_t)sizeof buf);
            core_out_str(&o, s_runtime.backend == 0u ? "backend sw frame "
                             : (s_runtime.backend == 1u ? "backend hw frame "
                             : (s_runtime.backend == 2u ? "backend hw-tex frame "
                                                : "backend hw-strip frame ")));
            core_out_u32(&o, total);
            core_out_str(&o, "\r\n");
            log_line(buf);
            stats_window(&s_stats, s_mode, s_runtime.backend, stg_modes_overflow());
        }

        /* 这一帧仍属当前模式：和弦期间的按键照常交给它 */
        crc = mode_frame(keys, entered);
        ft  = ((s_mode == 1u) ? s_last_tp : s_mout.t_present) - t0;    /* 到发出呈现请求为止，不含等场与下面的抽样普查 */
        if (entered) {
            say_enter(total, crc);
            entered = 0;
        }
        if (s_mode != 4u && stats_want_sample(&s_stats))
            sample_frame();
        if (s_mode != 4u) {
            const uint32_t *hw_rollup = NULL;
            uint32_t cpu_cyc = (s_mode == 1u)
                                 ? s_last_in + s_last_bld
                                 : s_mout.cyc_auto + s_mout.cyc_logic + s_mout.cyc_build;
#if SW9_PERF
            /* 每帧都读：先问「这一帧会不会出行」再读，场边界落在两次读场号之间时
               这一行会拿到窗口开头的计数，忙周期跟着错两行 */
            uint32_t hw_end[9];
            if (s_runtime.backend != 0u && renderer_perf_read(hw_end) == 0)
                hw_rollup = hw_end;
#endif
        stats_frame(&s_stats, ft, cpu_cyc, stg_modes_overflow(), hw_rollup);
        }
#if SW9_PERF
        {
            uint32_t snap, min, zero, under;
            if (board_display_diag(&snap, &min, &zero, &under) == 0) {
                if (snap != s_diag_last_frame) {
                    if (min < s_diag_min)
                        s_diag_min = min;
                    s_diag_last_frame = snap;
                    s_diag_samples++;
                }
                s_diag_zero = zero;
                s_diag_under = under;
            }
        }
#endif
        log_pump();
        total++;
        s_mode_frames++;
        if (GFX2D_PRESS_EXIT && s_mode == 3u && stg_modes_press_done()){
            if(GFX2D_PRESS_AB_BIN && s_runtime.backend==2u){
                s_runtime.fails+=stg_modes_fail()+s_stats.fail;
                stats_end(&s_stats);log_drain();board_puts("f5 backend TEX end\r\n");
                mode_leave(s_mode);s_runtime.backend=3u;mode_enter(3u);
                stats_init(&s_stats,&s_logq,s_mode,s_runtime.backend,stg_modes_overflow());
                entered=1;board_puts("f5 backend PB begin\r\n");
            }else break;
        }

        /* 切换在触发那一帧结束之后生效；统计的这一秒窗口从新模式的第一帧重新计 */
        if (s_r4_restart) {
            s_r4_restart = 0;
            mode_leave(4u);
            mode_enter(4u);
            stats_window(&s_stats, s_mode, s_runtime.backend, stg_modes_overflow());
            entered = 1;
        } else if (next != s_mode) {
            mode_leave(s_mode);
            mode_enter(next);
            stats_window(&s_stats, s_mode, s_runtime.backend, stg_modes_overflow());
            entered = 1;
            s_swj_n++;
        }
    }

    if (GFX2D_SWITCH_JUDGE) {
        char     buf[48];
        core_out o;

        core_out_init(&o, buf, (uint32_t)sizeof buf);
        core_out_str(&o, "switch done ");
        core_out_u32(&o, s_swj_n);
        core_out_str(&o, "\r\n");
        log_line(buf);
    }
    mode_leave(s_mode);
    if(s_mode!=4u)stats_end(&s_stats);
#if SW9_PERF
    {
        char buf[180];
        core_out o;
        core_out_init(&o, buf, (uint32_t)sizeof buf);
        core_out_str(&o, "diag fifo_min ");
        core_out_u32(&o, s_diag_samples ? s_diag_min : 0u);
        core_out_str(&o, " samples ");
        core_out_u32(&o, s_diag_samples);
        core_out_str(&o, " zero ");
        core_out_u32(&o, s_diag_zero);
        core_out_str(&o, " under ");
        core_out_u32(&o, s_diag_under);
        core_out_str(&o, " irqA ");
        core_out_u32(&o, board_irq_frame_events());
        core_out_str(&o, " irqB ");
        core_out_u32(&o, board_irq_engine_events());
        core_out_str(&o, " faults ");
        core_out_u32(&o, board_irq_faults());
        core_out_str(&o, " swap_retry ");
        core_out_u32(&o, board_present_retries());
        core_out_str(&o, "\r\n");
        log_line(buf);
    }
#endif
    log_flush();
    /* 最后两行等下一场、额度补满之后再打（PC 上 GFX2D_TX_BUDGET 模拟串口时不超额） */
    board_wait_frame();
    fail = s_runtime.fails + stg_modes_fail() + s_stats.fail + r4_fail_count();
    sw6_log_end(fail);
    return (fail == 0u) ? 0 : 1;
}

#endif  /* SW6_MODE_JUDGE */

/* --- 弹幕场入口------------------------------------------------------------- */

#if STG_FIELD_MODE != 0

static int run_stg(void)
{
    stg_main_ctx ctx;

    if (stg_runtime_boot_mem(&s_runtime) != 0 || stg_runtime_boot_asset(&s_runtime) != 0) {
        sw6_log_end(s_runtime.fails + 1u);
        return 1;
    }
    if (stg_runtime_surfaces_stg(&s_runtime) != 0) {
        board_puts("surf err 表面登记失败\r\n");
        sw6_log_end(s_runtime.fails + 1u);
        return 1;
    }

    ctx.tab     = &s_runtime.tab;
    ctx.mem     = &s_runtime.mem;
    ctx.es      = &s_runtime.es;
    ctx.err     = &s_runtime.err;
    ctx.cmds[0] = s_runtime.cmds[0];
    ctx.cmds[1] = s_runtime.cmds[1];
    ctx.cap[0]  = s_runtime.cap[0];
    ctx.cap[1]  = s_runtime.cap[1];
#if defined(SW9_REPLAY_HW) && SW9_REPLAY_HW
    ctx.hw =
#if defined(G4_STRIP_RENDER) && G4_STRIP_RENDER
        3;
#else
        1;
#endif
#else
    ctx.hw = 0;
#endif
#if (defined(G1_TEX_COMPARE) && G1_TEX_COMPARE) || (defined(G4_STRIP_COMPARE) && G4_STRIP_COMPARE)
    if (stg_runtime_tex_load(&s_runtime) != 0) {
        board_puts("compare tex load err\r\n");
        sw6_log_end(s_runtime.fails+1u); return 1;
    }
    s_runtime.tex_ready=1;
    if (stg_runtime_backend_apply(&s_runtime,2u,s_mode) != 0 || board_begin_compare() != 0) {
        board_puts("compare tex prepare err\r\n");
        sw6_log_end(s_runtime.fails+1u); return 1;
    }
    board_puts("compare tex ready; scan pattern\r\n");
#endif
#if defined(__riscv) && defined(G4_STRIP_RENDER) && G4_STRIP_RENDER
    if (stg_runtime_tex_load(&s_runtime) != 0) {
        board_puts("replay tex load err\r\n");
        sw6_log_end(s_runtime.fails + 1u); return 1;
    }
    s_runtime.tex_ready = 1;
    if (stg_runtime_backend_apply(&s_runtime,2u,s_mode) != 0) {
        sw6_log_end(s_runtime.fails + 1u); return 1;
    }
#endif
    if (renderer_prepare(ctx.hw, &s_runtime.tab) != 0) {
        board_puts("engine prepare err\r\n");
        sw6_log_end(s_runtime.fails + 1u);
        return 1;
    }
#if defined(SW9_COMPARE) && SW9_COMPARE
    if (renderer_prepare(1, &s_runtime.tab) != 0) {
        board_puts("compare prepare err\r\n");
        return 1;
    }
#endif
    return stg_main(&ctx, s_runtime.fails);
}

#endif /* STG_FIELD_MODE != 0 */

/* --- 入口 ---------------------------------------------------------------- */

int main(void)
{
    board_mem cmdA, cmdB;

    board_init();
    if (board_irq_init() != 0) {
        board_puts("irq init err\r\n");
        s_runtime.fails++;
    }
    crc32_init();
#if STG_FIELD_MODE != 0
    board_puts("gfx2dGame STG\r\n");
#elif SW6_MODE_JUDGE
    board_puts("gfx2dGame SW6\r\n");
#else
    board_puts("gfx2dGame MODES\r\n");
#endif

    cmdA = board_region(REGION_CMDLIST_A);
    cmdB = board_region(REGION_CMDLIST_B);
    s_runtime.cmds[0]    = (gfx_cmd *)cmdA.ptr;
    s_runtime.cap[0] = cmdA.size / (uint32_t)sizeof(gfx_cmd);
    s_runtime.cmds[1]    = (gfx_cmd *)cmdB.ptr;
    s_runtime.cap[1] = cmdB.size / (uint32_t)sizeof(gfx_cmd);

#if STG_FIELD_MODE != 0
    /* 下面基础 Demo 的启动与主循环不走；留在原处，其静态函数仍被引用，不触发未用告警 */
    return run_stg();
#endif

    if (boot() != 0) {
        sw6_log_end(s_runtime.fails + 1u);
        return 1;
    }

#if SW6_MODE_JUDGE
    return run_judge();
#else
    return run_modes();
#endif
}
