/******************************************************************************
* @file r4_render.c: 模式 4 的渲染适配——串行 RAST 提交、软件对照、HUD 条带与呈现节拍
*
* @brief  首版串行路径（研究 §4.3 定案）：submit -> wait -> present，CPU 不跨在途帧
*         覆盖主/子表。硬件路径 = gfx_engine_submit_raster（LOAD 背景 ∥ 清 z -> RAST ->
*         每带 2D HUD -> FLUSH，R3 ABI）；软件路径 = rast_ref 逐带直写帧缓冲 + exec_sw
*         跑 HUD（宿主与板上「软件光栅」对照同一段代码）。
*
*         呈现节拍：进入时锚定 F0，第 n 帧的目标场 = F0 + 2n（30Hz 两场节拍），
*         在目标场前一场发 present 请求；晚到不重置锚点，由 R4CAD 行如实记录。
*
*         HUD 走每带 32KiB 槽的旧 2D 命令（字库来自 DDR 表面），世界只走 RAST；
*         整帧只提交一次，不在完成后另发一张 2D 表。
******************************************************************************/

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "board.h"

#include "core/crc32.h"
#include "core/format.h"
#include "core/logq.h"
#include "core/replay_r4.h"

#include "gfx/gfx.h"
#include "gfx/rast_ref.h"
#include "gfx/rast_setup.h"

#include "assets.h"
#include "stg_slots.h"
#include "game/stg_text.h"

#include "r4.h"
#include "r4_scene.h"
#include "r4_cup.h"

#ifdef __riscv
#include "gfx_engine.h"
#include "gfx_regs.h"
#endif

/* --- 模式 4 独占的共享内存窗口（见 r4.h 内存注释） ------------------------- */
#define R4_REC_OFF      0x00000000u     /* 主表 64KiB（512 条 128B） */
#define R4_SUB_OFF      0x00010000u
#define R4_DEPTH_OFF    0x00018000u

#define R4_REC_MAX      (65536u / RAST_RECORD_BYTES)
#define R4_SUB_CAP      32768u
#ifndef R4_HUD_TEXT
#define R4_HUD_TEXT 1
#endif

#ifndef R4_BOARD_CRC
#define R4_BOARD_CRC GFX2D_R4_JUDGE
#endif

#define R4_HUD_BG       0x00050912u
#define R4_HUD_LINE     0x0035f2ffu

/* 录像缓冲：32 + 4*2100 ≈ 8.4KiB，够 70 秒。 */
#define R4_REC_BUF_BYTES (REPLAY_R4_HEADER_BYTES + 4u * 2100u)

static logq        *s_log;
static r4_world     s_world;
static fx16_mat4    s_proj;
static const rast_viewport s_vp = { R4_VP_CORRIDOR };
static rast_record *s_rec;
static uint8_t     *s_sub;
static uint16_t    *s_depth;
static uint32_t     s_atlas[4096];          /* rast_ref 用影子（16KiB） */
static uint32_t     s_ladder = 1u;
static uint32_t     s_hw = 1u;
static uint32_t     s_fail;
static r4_out       s_out;
static uint32_t     s_anchor;
static uint32_t     s_swaps0, s_cyc_acc, s_fps;
static uint32_t     s_fps_start;
static int          s_fps_valid;
static uint32_t     s_late, s_exact;
static uint32_t     s_records;              /* 上一帧记录数（HUD/统计） */
static uint32_t     s_rast_cyc;
static uint32_t     s_scene;
static uint32_t     s_times[2048], s_time_count, s_window_field, s_window_swaps;
static uint32_t     s_window_late, s_window_exact, s_window_over;
static uint32_t     s_window_fields_end, s_window_swaps_end;
static uint32_t     s_render_fps[2], s_render_valid[2];
static uint64_t     s_render_sum;
static uint32_t     s_render_n;
static exec_sw_state s_es;
static replay_r4_view s_replay;
static int s_replay_on;
static uint32_t s_pending_keys;
uint32_t r4_pending_keys(void) {uint32_t k=s_pending_keys;s_pending_keys=0;return k;}

int r4_set_replay(const uint8_t *data,uint32_t bytes)
{
    s_replay_on=0;
    if(replay_r4_open(data,bytes,&s_replay))return -1;
    s_replay_on=1;return 0;
}

void r4_scene_select(uint32_t scene) {s_scene=scene?1u:0u;}
#if GFX2D_R4_RECORD
static replay_r4_rec s_tape;
static uint8_t      s_tape_buf[R4_REC_BUF_BYTES];
#endif

/* --- 框架钩子与小输出 ------------------------------------------------------ */

static struct {
    int  (*tex_load_atlas)(void);    /* 装载 atlas 进影子与 TEX 窗口（宿主只装影子） */
    void (*tex_hotset_dirty)(void);  /* 热集被覆盖（main.c 置 s_tex_ready=0） */
    int  (*tex_hotset_restore)(void);/* 离开模式：重装 2D 热集 */
} s_hooks;

void r4_set_hooks(int (*load_atlas)(void), void (*hotset_dirty)(void),
                  int (*hotset_restore)(void))
{
    s_hooks.tex_load_atlas = load_atlas;
    s_hooks.tex_hotset_dirty = hotset_dirty;
    s_hooks.tex_hotset_restore = hotset_restore;
}

void r4_set_log(logq *q)
{
    s_log = q;
}

static void say(const char *s)
{
    if (s_log != NULL)
        (void)logq_put(s_log, s);
    else
        board_puts(s);
}

static void say_u32(const char *tag, uint32_t a, uint32_t b)
{
    char     line[96];
    core_out o;

    core_out_init(&o, line, (uint32_t)sizeof line);
    core_out_str(&o, tag);
    core_out_u32(&o, a);
    core_out_ch(&o, ' ');
    core_out_u32(&o, b);
    core_out_str(&o, "\r\n");
    say(line);
}

/* 周期数 -> 16.16 定点毫秒（仅显示用）。 */
#if !R4_HUD_NONE
static uint32_t cycles_to_ms16(uint32_t cycles)
{
    uint64_t v = (uint64_t)cycles * 65536u / 100000u;

    return (v > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)v;
}
#endif  /* !R4_HUD_NONE */

/* --- HUD：右栏 240 像素，每逻辑行恰占一个 16 像素条带 ----------------------- */

#define R4_HUD_ROWS 30

#if !R4_HUD_NONE
static uint32_t hud_build_strips(gfx_surface_table *tab, int dst, uint32_t *list)
{
#if R4_HUD_TEXT
    const stg_font white = { STG_SLOT_FONT, ASSET_FONT8X16_CELL_W, ASSET_FONT8X16_CELL_H,
                             ASSET_FONT8X16_COLS, 0x20u, ASSET_FONT8X16_CELLS,
                             0x00000000u };
    const stg_font cyan = { STG_SLOT_FONT_CYAN, ASSET_FONT8X16_CELL_W,
                            ASSET_FONT8X16_CELL_H, ASSET_FONT_CYAN_COLS, 0x20u,
                            ASSET_FONT_CYAN_CELLS, ASSET_FONT_CYAN_KEY };
#endif
    static char rows[R4_HUD_ROWS][32];
    char     v[24];
    uint32_t bad = 0u;
    uint32_t s;

    memset(rows, 0, sizeof rows);
    memcpy(rows[0], "MYRIAD R4 3D", 13u);
    memcpy(rows[2], "FPS ", 5u);
    memcpy(rows[3], "TRIS ", 6u);
    memcpy(rows[4], "RAST ", 6u);
    memcpy(rows[5], "CPU  ", 6u);
    memcpy(rows[6], "PATH ", 6u);
    memcpy(rows[8], "SCORE ", 7u);
    memcpy(rows[9], "SHIELD ", 8u);
    memcpy(rows[11], "1 2 3 4=3D", 11u);
    strcpy(rows[12], "4:CORRIDOR/CUP");
    strcpy(rows[13], "TAB:SW/HW");
    strcpy(rows[14], "SHIFT:L1/L2");

    {
        core_out o;

        core_out_init(&o, v, (uint32_t)sizeof v);
        core_out_u32(&o, s_fps_valid ? s_fps : 0u);
        memcpy(rows[2] + 4u, s_fps_valid ? v : "--", s_fps_valid ? (size_t)o.n + 1u : 3u);

        core_out_init(&o, v, (uint32_t)sizeof v);
        core_out_u32(&o, s_records);
        memcpy(rows[3] + 5u, v, (size_t)o.n + 1u);

        core_out_init(&o, v, (uint32_t)sizeof v);
        if (s_hw != 0u && s_rast_cyc != 0u) {
            core_out_u32(&o, cycles_to_ms16(s_rast_cyc) >> 16);
            core_out_ch(&o, '.');
            core_out_u32(&o, ((cycles_to_ms16(s_rast_cyc) & 0xFFFFu) * 10u) / 65536u);
        } else {
            core_out_str(&o, "--");
        }
        memcpy(rows[4] + 5u, v, (size_t)o.n + 1u);

        core_out_init(&o, v, (uint32_t)sizeof v);
        {
            uint32_t ms16 = cycles_to_ms16(s_out.cyc_logic + s_out.cyc_geom +
                                           s_out.cyc_bin + s_out.cyc_hud);

            core_out_u32(&o, ms16 >> 16);
            core_out_ch(&o, '.');
            core_out_u32(&o, ((ms16 & 0xFFFFu) * 10u) / 65536u);
        }
        memcpy(rows[5] + 5u, v, (size_t)o.n + 1u);

        v[0] = s_hw ? 'H' : 'S';
        v[1] = 'W';
        v[2] = ' ';
        v[3] = 'L';
        v[4] = (s_ladder >= 2u) ? '2' : '1';
        v[5] = 0;
        memcpy(rows[6] + 5u, v, 6u);

        core_out_init(&o, v, (uint32_t)sizeof v);
        core_out_u32(&o, s_out.score);
        memcpy(rows[8] + 6u, v, (size_t)o.n + 1u);

        core_out_init(&o, v, (uint32_t)sizeof v);
        core_out_u32(&o, s_out.shield);
        memcpy(rows[9] + 7u, v, (size_t)o.n + 1u);
    }
#if GFX2D_R4_JUDGE
    /* A frame comparison cannot contain platform clocks or backend labels. */
    strcpy(rows[2], "FPS TEST");
    strcpy(rows[4], "RAST TEST");
    strcpy(rows[5], "CPU TEST");
    strcpy(rows[6], s_ladder == 2u ? "PATH REF L2" : "PATH REF L1");
#endif
    for (unsigned k=0;k<2;++k) {
        core_out o;
        core_out_init(&o,rows[24+k],sizeof rows[24+k]);
        core_out_str(&o,k?"HW FPS ":"SW FPS ");
        if(s_render_valid[k] && !GFX2D_R4_JUDGE) {
            core_out_u32(&o,s_render_fps[k]/1000u);core_out_ch(&o,'.');
            core_out_u32(&o,(s_render_fps[k]/100u)%10u);
        } else core_out_str(&o,"--");
    }
    if(s_scene)strcpy(rows[0],"CUP 396 TRIANGLES");

    for (s = 0u; s < 30u; s++) {
        gfx_list l;
        gfx_rect bar, line;

        if (s_scene && s>=7u && s<=22u) {list[s]=0u;continue;}

        /* 引擎 BAND 相位的 HUD 命令一律用条带局部 y（0..15，y+h<=16，
           dst 必须等于帧 dst，见 engine_cmd.v 的 local_bad 检查）；
           软件路径由 render_sw 按条带平移目的表面基址落地。 */
        gfx_list_begin(&l, (gfx_cmd *)(void *)((uint8_t *)list + (s + 1u) * 32768u),
                       1024u - 8u, tab);
        bar.x0 = 400;
        bar.y0 = 0;
        bar.x1 = 639;
        bar.y1 = 15;
        if (gfx_fill(&l, dst, bar, R4_HUD_BG, GFX_EQ_COPY, 0u) != 0)
            bad++;
        if (s == 0u) {
            line.x0 = 400;
            line.y0 = 0;
            line.x1 = 401;
            line.y1 = 1;
            if (gfx_fill(&l, dst, line, R4_HUD_LINE, GFX_EQ_COPY, 0u) != 0)
                bad++;
        }
#if R4_HUD_TEXT
        if (rows[s][0] != 0) {
            const stg_font *f = (s == 0u || s == 6u) ? &cyan : &white;
            uint32_t        drawn = 0u;

            bad += stg_text_draw(&l, f, dst, 408, 0, rows[s], &drawn);
        }
#endif  /* R4_HUD_TEXT */
        if (gfx_list_end(&l) != 0)
            bad++;
        list[s] = l.used - 1u;                  /* 计数不含 END（引擎按计数读） */
    }
    return bad;
}
#endif  /* !R4_HUD_NONE */

/* --- 背景与帧 CRC ----------------------------------------------------------- */

/* 离屏底色：深蓝纵向渐变（LOAD 每帧整屏读它）。 */
static void build_background(void)
{
    uint32_t *bg = (uint32_t *)((uint8_t *)board_region(REGION_OFFSCREEN).ptr+R4_BG_OFFSET);
    uint32_t  x, y;

    for (y = 0u; y < FB_HEIGHT; y++) {
        uint32_t *row = bg + (size_t)y * FB_ROW_PIXELS;
        const uint32_t c = R4_COLOR_BG + ((y * 24u) / FB_HEIGHT) * 0x00010102u;

        for (x = 0u; x < FB_WIDTH; x++)
            row[x] = c;
    }
}

uint32_t r4_frame_crc(const uint32_t *fb)
{
    crc32_ctx c;
    uint32_t  y;

    crc32_begin(&c);
    for (y = 0u; y < FB_HEIGHT; y++)
        crc32_update(&c, (const uint8_t *)fb + (size_t)y * FB_STRIDE_BYTES,
                     FB_WIDTH * 4u);
    return crc32_end(&c);
}

/* --- 进入 / 离开 ------------------------------------------------------------- */

int r4_mode_enter(gfx_surface_table *tab, const gfx_mem *mem, int hw, uint32_t ladder)
{
    (void)tab;
    (void)mem;
    uint32_t fail_before=s_fail;
    s_ladder = (ladder >= 2u) ? 2u : 1u;
    s_hw = hw ? 1u : 0u;
    memset(&s_out, 0, sizeof s_out);
    s_out.ladder = s_ladder;
    s_out.hw = s_hw;
    s_out.scene = s_scene;
    {
        static uint32_t last_scene=UINT32_MAX,last_ladder=UINT32_MAX;
        if(last_scene!=s_scene || last_ladder!=s_ladder)memset(s_render_valid,0,sizeof s_render_valid);
        last_scene=s_scene;last_ladder=s_ladder;
    }
    s_time_count=0u;s_window_late=0u;s_window_exact=0u;s_window_over=0u;
    s_render_sum=0;s_render_n=0;
    if (fx16_perspective(&s_proj, R4_FOV_BAM, R4_ASPECT, R4_NEAR_Z, R4_FAR_Z) != 0) {
        s_fail++;
        return -1;
    }
    if(s_replay_on && (s_replay.scene_id!=R4_SCENE_ID+s_scene ||
        s_replay.ladder!=s_ladder || s_replay.asset_version!=R4_ASSET_VERSION)) {
        ++s_fail;return -1;
    }
    if(board_region(REGION_SHARED).size<R4_DEPTH_OFF+32768u) {++s_fail;return -1;}
    if(board_region(REGION_OFFSCREEN).size<R4_BG_OFFSET+FB_BYTES) {++s_fail;return -1;}
    r4_world_init(&s_world, s_replay_on?s_replay.seed:R4_SEED);
    s_rec = (rast_record *)(void *)((uint8_t *)board_region(REGION_SHARED).ptr + R4_REC_OFF);
    s_sub = (uint8_t *)board_region(REGION_SHARED).ptr + R4_SUB_OFF;
    s_depth = (uint16_t *)(void *)((uint8_t *)board_region(REGION_SHARED).ptr + R4_DEPTH_OFF);

    build_background();
    memset(s_atlas, 0, sizeof s_atlas);
    if (s_ladder >= 2u && s_hooks.tex_load_atlas != NULL &&
        s_hooks.tex_load_atlas() != 0) {
        say("r4 err atlas 装载失败，降级 L1\r\n");
        s_ladder = 1u;
        s_out.ladder = 1u;
    }
    if (s_hooks.tex_hotset_dirty != NULL)
        s_hooks.tex_hotset_dirty();
    exec_sw_reset(&s_es);

    r4_timing_start();
    s_swaps0 = board_swap_count();
    s_cyc_acc = 0u;
    s_fps = 0u;
    s_fps_valid = 0;
    s_late = 0u;
    s_exact = 0u;
    s_records = 0u;
    s_rast_cyc = 0u;
#if GFX2D_R4_RECORD
    replay_r4_rec_begin(&s_tape, s_tape_buf, (uint32_t)sizeof s_tape_buf,
                        s_replay_on?s_replay.seed:R4_SEED, R4_SCENE_ID+s_scene, s_ladder, R4_ASSET_VERSION);
#endif
    say_u32("r4 enter ladder/hw ", s_ladder, s_hw);
    return (s_fail == fail_before) ? 0 : -1;
}

void r4_timing_start(void)
{
    s_anchor = board_frame_count();
    s_fps_start=board_cycles();s_swaps0=board_swap_count();
}

void r4_mode_leave(void)
{
    if(s_time_count) {
        static uint32_t sorted[2048];
        for(unsigned i=0;i<s_time_count;++i)sorted[i]=s_times[i];
        for(unsigned i=1;i<s_time_count;++i) {
            uint32_t v=sorted[i];unsigned j=i;
            while(j && sorted[j-1]>v){sorted[j]=sorted[j-1];--j;}sorted[j]=v;
        }
        char line[256];core_out o;core_out_init(&o,line,sizeof line);
        core_out_str(&o,"R4PERF");
        uint32_t values[]={s_time_count,sorted[(s_time_count-1)/2],
            sorted[(s_time_count*99u+99u)/100u-1u],sorted[s_time_count-1],
            s_window_late,s_window_exact,s_window_over,
            s_window_fields_end-s_window_field,s_window_swaps_end-s_window_swaps};
        for(unsigned i=0;i<sizeof values/sizeof values[0];++i){core_out_ch(&o,' ');core_out_u32(&o,values[i]);}
        core_out_str(&o,"\r\n");say(line);
    }
#if GFX2D_R4_RECORD
    {
        replay_r4_view v;
        char line[128];
        uint32_t bytes=replay_r4_rec_end(&s_tape);
        if(replay_r4_open(s_tape_buf,bytes,&v))++s_fail;
        else {
            replay_r4_text_header(v.seed,v.scene_id,v.ladder,v.asset_version,v.frames,line,sizeof line);
            board_puts(line);
            for(uint32_t i=0;i<v.frames;++i) {
                uint32_t keys=0;
                if(replay_r4_key_at(&v,i,&keys))++s_fail;
                replay_r4_text_key(i,keys,line,sizeof line);
                board_puts(line);
            }
        }
    }
#endif
    say_u32("r4 leave frames/late ", s_out.frames, s_late);
#ifdef __riscv
    {
        uint32_t field=0,min=0,zero=0,under=0;
        if(board_display_diag(&field,&min,&zero,&under))++s_fail;
        char line[128];core_out o;core_out_init(&o,line,sizeof line);core_out_str(&o,"R4HEALTH");
        uint32_t values[]={field,min,zero,under,board_display_response_errors()};
        for(unsigned i=0;i<5;++i){core_out_ch(&o,' ');core_out_u32(&o,values[i]);}
        core_out_str(&o,"\r\n");say(line);
        if(zero||under||values[4])++s_fail;
    }
#endif
    if (s_hooks.tex_hotset_restore != NULL) {
        if (s_hooks.tex_hotset_restore() != 0)
            say("r4 err 热集重装失败\r\n");
    }
}

/* --- 执行路径 ---------------------------------------------------------------- */

#ifdef __riscv
/* 把表面表整表登记进引擎（错误恢复与进入时共用）。 */
static void r4_register_surfaces(gfx_surface_table *tab)
{
    uint32_t i;

    for (i = 0u; i < GFX_SURF_N; i++) {
        const gfx_surf_raw *sr = &tab->s[i];

        if ((sr->ctrl & 0x80000000u) == 0u)
            continue;
        (void)gfx_engine_surf_set_space((int)i, sr->base, sr->dim & 0xFFFFu,
                                        sr->dim >> 16, sr->ctrl & 0x1Fu,
                                        (sr->ctrl >> 8) & 3u,
                                        (sr->ctrl >> 12) & 0xFu);
    }
}

static int submit_hw(gfx_surface_table *tab, int dst_slot, int fb_index,
                     uint32_t bytes, uint32_t count, uint32_t *crc_out)
{
    gfx_engine_raster p;
    uint32_t code = ~0u, index = 0u, tag = 0u, id = 0u, caps = 0u;
    uint32_t cycles = 0u, refs = 0u;
    const uint32_t shared = g_mem_regions[REGION_SHARED].offset;

    p.tri_base = shared + R4_REC_OFF;
    p.tri_alloc = 65536u;
    p.sub_base = shared + R4_SUB_OFF;
    p.sub_alloc = R4_SUB_CAP;
    p.sub_bytes = bytes;
    p.tri_count = count;
    p.ras_key = 0x00000000u;                    /* atlas 透明色 = 全 0 */
    p.texcfg = 0u;
    {
        const int sr = gfx_engine_submit_raster(g_mem_regions[REGION_CMDLIST_A].offset,
                                                R4_SLOT_BG, (uint32_t)dst_slot, &p);

        if (sr != GFX_ENG_OK) {
#ifdef __riscv
            say_u32("r4 err submit rc/count ", (uint32_t)sr, count);
#else
            say_u32("r4 err submit rc/count ", (uint32_t)sr, count);
#endif
            return -1;
        }
    }
    if (gfx_engine_wait(&code, &index, &tag) != 0 || code != 0u) {
        uint32_t stage = 0u;

        (void)gfx_engine_error_stage_read(&stage);
        say_u32("r4 err engine code/index ", code, index);
        say_u32("r4 err engine tag/stage ", tag, stage);
        if (code != 0u) {
            /* 引擎出错后复位并重登记表面表，下一帧才能干净提交。 */
            (void)gfx_engine_soft_reset();
            r4_register_surfaces(tab);
        }
        return -1;
    }
    if (gfx_engine_raster_info(&id, &caps, &cycles, &refs) != 0 ||
        refs != (bytes - 120u) / 2u)
        return -1;
    s_rast_cyc = cycles;
    *crc_out = 0u;
    (void)fb_index;
    return 0;
}
#endif

/* 软件路径：bg 整屏 BLIT -> rast_ref 逐带直写帧缓冲 -> HUD 逐带 exec。 */
static int render_sw(gfx_surface_table *tab, const gfx_mem *mem, int dst_slot,
                     int fb_index, uint32_t count, uint32_t *crc_out)
{
    static exec_sw_error err;
    exec_sw_stats st;
    uint32_t *fb = board_fb(fb_index);
    uint32_t  s;

    {
        gfx_list l;
        gfx_cmd  cmds[4];

        gfx_list_begin(&l, cmds, 4u, tab);
        if (gfx_blit(&l, R4_SLOT_BG, 0, 0, dst_slot, 0, 0,
                     (int)FB_WIDTH, (int)FB_HEIGHT, GFX_EQ_COPY, 0u, 0, 0u) != 0 ||
            gfx_list_end(&l) != 0)
            return -1;
        exec_sw_stats_clear(&st);
        if (exec_sw_run(&s_es, tab, mem, cmds, l.used, l.used, &st, &err) != GFX_OK)
            return -1;
    }
    for (s = 0u; s < 30u; s++) {
        rast_target t;
        uint32_t row, x;
        if(!GFX2D_R4_JUDGE)
            s_pending_keys |= board_keys() & (BOARD_KEY_SWAP|BOARD_KEY_SLOW|
                BOARD_KEY_MODE1|BOARD_KEY_MODE2|BOARD_KEY_MODE3);

        /* depth 与 color 同 stride（契约 §4）：按 1024 的实际行距清 z。 */
        for (row = 0u; row < 16u; row++)
            for (x = 0u; x < FB_ROW_PIXELS; x++)
                s_depth[row * FB_ROW_PIXELS + x] = 65535u;
        t.color = fb + (size_t)s * 16u * FB_ROW_PIXELS;
        t.depth = s_depth;
        t.stride = FB_ROW_PIXELS;
        t.strip = s;
        t.texture = s_atlas;
        t.tex_width = 64u;
        t.tex_height = 64u;
        t.tex_stride = 64u;
        t.key = 0x00000000u;
        if (rast_ref_run(s_rec, count, &t) != 0)
            return -1;
    }
    for (s = 0u; s < 30u; s++) {
        /* HUD 命令是条带局部 y：软件路径把目的表面基址平移到本条带首行，
           引擎的 BAND 相位则由硬件把局部 y 落进对应条带——两路同语义。 */
        gfx_surface_table t2 = *tab;
        uint32_t *hdr = (uint32_t *)board_region(REGION_CMDLIST_A).ptr;
        gfx_cmd  *cmds = (gfx_cmd *)(void *)((uint8_t *)hdr + (s + 1u) * 32768u);

        t2.s[dst_slot].base += s * 16u * FB_STRIDE_BYTES;
        exec_sw_stats_clear(&st);
        if (exec_sw_run(&s_es, &t2, mem, cmds, hdr[s], hdr[s], &st, &err) != GFX_OK)
            return -1;
    }
    *crc_out = 0u;
    return 0;
}

/* --- 一帧 ------------------------------------------------------------------- */

int r4_mode_frame(gfx_surface_table *tab, const gfx_mem *mem, int dst_slot,
                  uint32_t keys, r4_out *out)
{
    const int       fb_index = dst_slot - STG_SLOT_FB0;
    const uint32_t  t0 = board_cycles();
    uint32_t        t_seg, produced, bytes, crc = 0u, due, hud_bad;
    uint32_t       *list = (uint32_t *)board_region(REGION_CMDLIST_A).ptr;
    int             rc;

    if (fb_index < 0 || fb_index > 1) {
        s_fail++;
        return -1;
    }
    /* 1) 逻辑 tick（30Hz，确定性）。 */
    t_seg = board_cycles();
    if(s_replay_on && replay_r4_key_at(&s_replay,s_out.frames,&keys)) {++s_fail;return -1;}
    r4_world_tick(&s_world, keys, &s_fail);
    s_out.score=s_world.score;s_out.shield=s_world.shield;s_out.ticks=s_world.ticks;
    s_out.fail=s_fail;
    s_out.cyc_logic = board_cycles() - t_seg;
#if GFX2D_R4_RECORD
    (void)replay_r4_rec_put(&s_tape, keys);
#endif

    /* 2) 几何（发射 + setup）与分桶。 */
    t_seg = board_cycles();
    if(s_scene) {
        int n=r4_cup_emit(s_world.ticks,s_rec,R4_REC_MAX);
        produced=n<0?UINT32_MAX:(uint32_t)n;
    } else produced = r4_world_emit(&s_world, &s_proj, &s_vp, s_ladder, s_rec, R4_REC_MAX, NULL);
    s_out.cyc_geom = board_cycles() - t_seg;
    if (produced == 0u || produced > R4_RECORD_CAP) {
        s_fail++;
        say_u32("r4 err records ", produced, 0u);
        return -1;
    }
    t_seg = board_cycles();
    bytes = (uint32_t)rast_bin(s_rec, produced, s_sub, R4_SUB_CAP);
    s_out.cyc_bin = board_cycles() - t_seg;
    if (bytes < 120u) {
        s_fail++;
        say_u32("r4 err bin ", bytes, produced);
        return -1;
    }

    /* 3) HUD 条带表（计数头整块清零后重建；命令区按计数覆盖）。 */
    s_records=produced;
    t_seg = board_cycles();
    memset(list, 0, 32768u);
#if R4_HUD_NONE
    hud_bad = 0u;
#else
    hud_bad = hud_build_strips(tab, dst_slot, list);
#endif
    s_out.cyc_hud = board_cycles() - t_seg;
    s_fail += hud_bad;

    /* 4) 执行（硬件 RAST 或软件参考）；失败帧不呈现。 */
    t_seg = board_cycles();
#ifdef __riscv
    if (s_hw != 0u)
        rc = submit_hw(tab, dst_slot, fb_index, bytes, produced, &crc);
    else
#endif
        rc = render_sw(tab, mem, dst_slot, fb_index, produced, &crc);
    s_out.cyc_submit = board_cycles() - t_seg;
    if (rc != 0) {
        s_fail++;
        return -1;
    }
    s_records = produced;
    s_out.records = produced;
    s_out.refs = (bytes - 120u) / 2u;
    {
        uint32_t bb = 0u;

        for (uint32_t i = 0u; i < produced; i++) {
            const int32_t x0 = (int16_t)rast_get16(&s_rec[i], 72);
            const int32_t x1 = (int16_t)rast_get16(&s_rec[i], 74);
            const int32_t y0 = (int16_t)rast_get16(&s_rec[i], 76);
            const int32_t y1 = (int16_t)rast_get16(&s_rec[i], 78);

            bb += (uint32_t)(x1 - x0 + 1) * (uint32_t)(y1 - y0 + 1);
        }
        s_out.bbox_points = bb;
    }
    s_out.render_cycles = board_cycles() - t0;
#if R4_BOARD_CRC
#ifdef __riscv
    if(s_hw)board_dcache_invalidate_range(board_fb(fb_index),FB_BYTES);
#endif
#ifdef R4_INJECT_PIXEL
    board_fb(fb_index)[200u*FB_ROW_PIXELS+200u]^=1u;
#endif
    crc=r4_frame_crc(board_fb(fb_index));
    for(unsigned y=0;y<FB_HEIGHT;++y)for(unsigned x=FB_WIDTH;x<FB_ROW_PIXELS;++x)
        if(board_fb(fb_index)[y*FB_ROW_PIXELS+x]!=0u) {
            ++s_fail;say("r4 err framebuffer padding changed\r\n");y=FB_HEIGHT;break;
        }
    s_out.record_crc=crc32_ieee(s_rec,produced*RAST_RECORD_BYTES);
#endif
    s_out.crc = crc;

    /* 5) 两场节拍呈现：目标场 = F0 + 2*(f+1)，目标前一场发请求。 */
    due = s_anchor + 2u * (s_out.frames + 1u);
    while (!GFX2D_R4_JUDGE && !s_scene &&
           (int32_t)(board_frame_count() - (due - 1u)) < 0) {
        if (board_should_quit())
            break;
    }
    {
        uint32_t ticket = 0u;

        if (board_present_request(fb_index, &ticket) != 0 ||
            board_present_wait(ticket) != 0) {
            s_fail++;
            say_u32("r4 err present frame ", s_out.frames, 0u);
            return -1;
        }
    }
    {
        const uint32_t actual = board_frame_count();
        char     line[80];
        core_out o;

        if (actual == due)
            s_exact++;
        else if ((int32_t)(actual-due)>0)
            s_late++;
        if(s_out.frames==30u) {
            s_window_field=actual;s_window_swaps=board_swap_count();
        } else if(s_out.frames>30u && s_time_count<2048u) {
            s_times[s_time_count++]=s_out.render_cycles;
            s_window_exact+=(actual==due);
            s_window_late+=((int32_t)(actual-due)>0);
            s_window_over+=(s_out.render_cycles>BOARD_HZ/30u);
            s_window_fields_end=actual;s_window_swaps_end=board_swap_count();
        }
        core_out_init(&o, line, (uint32_t)sizeof line);
        core_out_str(&o, "R4CAD ");
        core_out_u32(&o, s_out.frames);
        core_out_ch(&o, ' ');
        core_out_u32(&o, s_out.render_cycles);
        core_out_ch(&o, ' ');
        core_out_u32(&o, due);
        core_out_ch(&o, ' ');
        core_out_u32(&o, actual);
        core_out_ch(&o, ' ');
        core_out_u32(&o, board_swap_count());
        core_out_ch(&o, ' ');
        core_out_ch(&o, ' ');
        core_out_u32(&o, s_rast_cyc);       /* 引擎 RAST 相位周期（定位慢段用） */
        core_out_ch(&o, ' ');
        core_out_hex32(&o, crc);            /* 整帧 CRC：板上/宿主逐帧对账用 */
        core_out_ch(&o, ' ');core_out_hex32(&o,s_out.record_crc);
        core_out_str(&o, "\r\n");
        if(GFX2D_R4_JUDGE)say(line);
    }

    /* FPS 窗口与出口。 */
    s_cyc_acc=(uint32_t)(board_cycles()-s_fps_start);
    if (s_cyc_acc >= BOARD_HZ) {
        s_fps=(uint32_t)(((uint64_t)(board_swap_count()-s_swaps0)*BOARD_HZ+s_cyc_acc/2u)/s_cyc_acc);
        s_swaps0=board_swap_count();s_fps_start=board_cycles();s_fps_valid=1;
    }
    s_render_sum+=s_out.render_cycles;++s_render_n;
    if(s_render_sum>=BOARD_HZ || s_render_n==30u) {
        s_render_fps[s_hw]=(uint32_t)(((uint64_t)s_render_n*BOARD_HZ*1000u)/s_render_sum);
        s_render_valid[s_hw]=1u;s_render_sum=0;s_render_n=0u;
        if(!GFX2D_R4_JUDGE) {
            char line[128];core_out o;core_out_init(&o,line,sizeof line);
            core_out_str(&o,"R4STAT");
            uint32_t values[]={s_fps,s_render_fps[s_hw],produced,s_ladder,s_hw,s_scene};
            for(unsigned i=0;i<6;++i){core_out_ch(&o,' ');core_out_u32(&o,values[i]);}
            core_out_str(&o,"\r\n");say(line);
        }
    }
    s_out.frames++;
    *out = s_out;
    return 0;
}

uint32_t r4_fail_count(void)
{
    return s_fail;
}

uint32_t r4_exact_count(void)
{
    return s_exact;
}

uint32_t r4_late_count(void)
{
    return s_late;
}

uint32_t *r4_atlas_shadow(void)
{
    return s_atlas;
}
