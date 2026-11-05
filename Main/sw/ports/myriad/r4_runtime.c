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
*         走廊/滑翔整帧一次提交；凝时依次绘制世界中间面、独立深度武器和HUD。
******************************************************************************/

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "board.h"

#include "core/crc32.h"
#include "core/format.h"
#include <math.h>
#include "core/logq.h"
#include "core/replay_r4.h"

#include "gfx/gfx.h"
#include "gfx/rast_ref.h"
#include "gfx/rast_setup.h"

#include "assets.h"
#include "stg_slots.h"
#include "game/stg_text.h"
#include "game/myriad_hud.h"

#include "r4.h"
#include "r4_scene.h"
#include "sh_scene.h"

#ifdef __riscv
#include "gfx_engine.h"
#include "gfx_regs.h"
#endif

/* Shared windows: 256KiB records + 128KiB bins + 32KiB strip depth.
   SH's exact weapon-pose cache is leased below the 2D arena at offset 4MiB. */
#define R4_REC_OFF      0x00000000u     /* 2048 x 128B */
#define R4_SUB_OFF      0x00040000u
#define R4_DEPTH_OFF    0x00060000u

#define R4_REC_MAX      (262144u / RAST_RECORD_BYTES)
#define R4_SUB_CAP      131072u
#define R4_SH_CACHE_OFF 0x00100000u
#define R4_SH_GUN_SUB_OFF 0x00068000u
#define R4_SH_GUN_SUB_CAP 65536u
#define R4_SH_WORLD_OFF 0x00200000u
#define R4_SH_WORLD_SLOT 3u
_Static_assert(R4_DEPTH_OFF+32768u<=R4_SH_GUN_SUB_OFF,"SH depth/bin overlap");
_Static_assert(R4_SH_GUN_SUB_OFF+R4_SH_GUN_SUB_CAP<=R4_SH_CACHE_OFF,"SH bin/cache overlap");
_Static_assert(R4_SH_CACHE_OFF+SH_CACHE_BYTES<=R4_SH_WORLD_OFF,"SH cache/world overlap");
_Static_assert(R4_SH_WORLD_OFF+FB_BYTES<=0x00400000u,"SH world/2D arena overlap");
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
static rast_viewport s_vp = { R4_VP_CORRIDOR };
static rast_frame s_raster_frame = {RAST_WIDTH,RAST_HEIGHT};
static board_frame s_frame;
static uint16_t *s_canvas_depth;
static unsigned s_canvas_depth_stride;
#ifndef __riscv
/* Detailed SH meshes are host-owned. Never overlap the target's fixed 64KiB list. */
static rast_record s_sh_records[SH_PC_RECORD_CAP];
#endif
static uint32_t s_record_capacity=R4_REC_MAX;
static uint32_t s_sh_previous_ms;
static uint32_t s_sh_world_count,s_sh_gun_bytes;
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
static uint32_t (*s_key_source)(void);   /* 键源注入：宿主框架（console）的
    inputd 是 board_keys 的唯一合法调用方，r4 每个条带的直读会抢走 UART
    解析器的字节（2026-10-28 板上实测：HOME 和弦在 r4 里永远拼不齐）。
    缺省 NULL = 直读 board_keys，裸机行为不变。 */
static uint32_t *s_hud_list;
static uint32_t s_hud_device;
static uint8_t s_hud_valid[2][30];
static uint32_t s_hud_count[2][30];
uint32_t r4_pending_keys(void) {uint32_t k=s_pending_keys;s_pending_keys=0;return k;}
void r4_set_key_source(uint32_t (*fn)(void)) {s_key_source=fn;}

int r4_set_replay(const uint8_t *data,uint32_t bytes)
{
    s_replay_on=0;
    if(replay_r4_open(data,bytes,&s_replay))return -1;
    s_replay_on=1;return 0;
}

/* 0=走廊、2=山脊；1 号茶杯场景 2026-11-02 删除，传 1 按走廊处理。 */
void r4_scene_select(uint32_t scene) {s_scene = scene == 3u ? 3u : (scene > 1u ? 2u : 0u);}
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
#if !R4_HUD_NONE && !GFX_PRODUCT_HUD
static uint32_t cycles_to_ms16(uint32_t cycles)
{
    uint64_t v = (uint64_t)cycles * 65536u / 100000u;

    return (v > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)v;
}

#endif  /* !R4_HUD_NONE */

/* --- HUD：右栏 160 像素，每逻辑行恰占一个 16 像素条带 ----------------------- */

#define R4_HUD_ROWS 30

#if !R4_HUD_NONE
static void hud_number(char *text,uint32_t value,unsigned width)
{
    char temp[12];unsigned n=0;
    do {temp[n++]=(char)('0'+value%10u);value/=10u;}while(value);
    while(n<width)temp[n++]='0';
    for(unsigned i=0;i<n;++i)text[i]=temp[n-1-i];
    text[n]=0;
}

static uint32_t hud_build_strips(gfx_surface_table *tab,int dst,uint32_t *list)
{
    const stg_font white={STG_SLOT_FONT,ASSET_FONT8X16_CELL_W,ASSET_FONT8X16_CELL_H,
        ASSET_FONT8X16_COLS,0x20u,ASSET_FONT8X16_CELLS,0};
    const stg_font cyan={STG_SLOT_FONT_CYAN,ASSET_FONT8X16_CELL_W,ASSET_FONT8X16_CELL_H,
        ASSET_FONT_CYAN_COLS,0x20u,ASSET_FONT_CYAN_CELLS,ASSET_FONT_CYAN_KEY};
#if GFX_PRODUCT_HUD
    const stg_font amber={STG_SLOT_FONT_AMBER,ASSET_FONT8X16_CELL_W,ASSET_FONT8X16_CELL_H,
        ASSET_FONT_AMBER_COLS,0x20u,ASSET_FONT_AMBER_CELLS,ASSET_FONT_AMBER_KEY};
#endif
    static char rows[R4_HUD_ROWS][32],values[R4_HUD_ROWS][48];  /* 48：场景2 的 tagline 38 字符 */
    static char previous_rows[2][R4_HUD_ROWS][32],previous_values[2][R4_HUD_ROWS][48];
    static char previous_active[2][12];
    static uint32_t previous_icons[2][2];
    unsigned fb=(unsigned)(dst-STG_SLOT_FB0);
    char active[12];uint32_t bullets=0,objects=1,bad=0;
    memset(rows,0,sizeof rows);memset(values,0,sizeof values);
    for(unsigned i=0;i<R4_EBULLET_MAX;++i)bullets+=s_world.ebul[i].alive!=0;
    objects+=bullets+(s_ladder>=2 && s_world.enemy.alive);
    for(unsigned i=0;i<R4_PBULLET_MAX;++i)objects+=s_world.pbul[i].alive!=0;
    strcpy(rows[5],s_scene==2u?"RIDGE FLIGHT":s_ladder>=2?"3D FLIGHT / L2":"3D FLIGHT / L1");
    strcpy(rows[6],"SCORE");hud_number(rows[7],s_world.score,9);
    strcpy(rows[9],"HI-SCORE");hud_number(rows[10],s_world.hiscore,9);
    strcpy(rows[12],"PLAYER");strcpy(rows[14],"BOMB");
    strcpy(rows[15],"GRAZE");hud_number(values[15],s_world.graze,0);
    strcpy(rows[18],s_scene==2u?(s_frame.width==FB_WIDTH?"SAME 320x240":"ADAPTIVE VIEW"):
        GFX2D_R4_JUDGE?"FIXED INPUT":s_hw?"FPGA RASTER":"CPU RASTER");
    strcpy(rows[19],s_ladder>=2?"L2 MATERIALS":"L1 SOLID");
    if (s_scene == 2u) {
        unsigned i;
        for (i = 11u; i < R4_HUD_ROWS; ++i) { rows[i][0] = '\0'; values[i][0] = '\0'; }
    }
    if (s_scene == 2u) {
        unsigned speed = 4u + ((s_world.combo >> 1) > 3u ? 3u : (s_world.combo >> 1));
        strcpy(rows[8], "RINGS"); hud_number(values[8], s_world.rings_passed, 0);
        strcpy(rows[14], "COMBO"); hud_number(values[14], s_world.combo, 0);
        strcpy(rows[18], "SPEED"); hud_number(values[18], speed, 0);
        strcpy(rows[20], "ZONE"); hud_number(values[20], s_world.gate_seq / 8u + 1u, 0);
    } else {
        strcpy(rows[20],"CONTACTS");hud_number(active,objects,2);
        strcpy(rows[24],"SHOTS");hud_number(values[24],bullets,0);
    }
    strcpy(rows[25],"TRIS");hud_number(values[25],s_records,0);
    strcpy(rows[27],"SW FPS");strcpy(rows[28],"HW FPS");
    for(unsigned i=0;i<2;++i) {
        core_out o;core_out_init(&o,values[27+i],sizeof values[0]);
        if(s_render_valid[i] && !GFX2D_R4_JUDGE) {
            core_out_u32(&o,s_render_fps[i]/1000);core_out_ch(&o,'.');
            core_out_u32(&o,(s_render_fps[i]/100)%10);
        } else core_out_str(&o,"--");
    }
#if GFX_PRODUCT_HUD
    /* Same rows, glyph sizes and right edge as the product's danmaku HUD. */
    memset(rows,0,sizeof rows);memset(values,0,sizeof values);
    strcpy(rows[1],"MYRIAD");
    strcpy(rows[4],s_scene==2u?"RIDGE FLIGHT":s_ladder>=2?"3D FLIGHT / L2":"3D FLIGHT / L1");
    strcpy(rows[6],"SCORE");hud_number(rows[7],s_world.score,9);
    strcpy(rows[9],"HI-SCORE");hud_number(rows[10],s_world.hiscore,9);
    strcpy(rows[12],"PLAYER");strcpy(rows[14],"BOMB");
    strcpy(rows[16],"GRAZE");hud_number(values[16],s_world.graze,0);
    strcpy(rows[20],s_ladder>=2?"L2 MATERIALS":"L1 SOLID");
    strcpy(rows[21],s_scene==2u?"ADAPTIVE VIEW":s_hw?"FPGA RASTER":"CPU RASTER");
    strcpy(rows[24],s_scene==2u?"RINGS":"OBJECTS");hud_number(values[24],objects,0);
    strcpy(rows[25],"TRIS");hud_number(values[25],s_records,0);
    strcpy(rows[27],"SW FPS");strcpy(rows[28],"HW FPS");
    for(unsigned i=0;i<2;++i) {
        core_out o;core_out_init(&o,values[27+i],sizeof values[0]);
        if(s_render_valid[i] && !GFX2D_R4_JUDGE) {
            core_out_u32(&o,s_render_fps[i]/1000);core_out_ch(&o,'.');
            core_out_u32(&o,(s_render_fps[i]/100)%10);
        } else core_out_str(&o,"--");
    }
    strcpy(rows[29],"ESC MENU  C SCENE");
    if (s_scene == 2u) {
        /* 山脊滑翔版右栏：SCORE/HI/COMBO/GRAZE/RINGS/BOMB 全随门环退场，
           留 PLAYER（撞山掉盾）+ SPEED + TRIS + FPS。
           排版三层：左上 zone 名（0 行）、右上行程（同 0 行）、
           左下 tagline（29 行，与右栏操作提示并存）。 */
        unsigned i2;
        static const unsigned dead[6] = {6u, 7u, 9u, 10u, 14u, 16u};
        for (i2 = 0u; i2 < 6u; ++i2) { rows[dead[i2]][0] = '\0'; values[dead[i2]][0] = '\0'; }
        for (i2 = 15u; i2 < 27u; ++i2) { rows[i2][0] = '\0'; values[i2][0] = '\0'; }
        strcpy(rows[18], "SPEED"); hud_number(values[18], 4u, 0);
        strcpy(rows[24], "TRIS"); hud_number(values[24], s_records, 0);
        strcpy(rows[0], ridge_zone_names[r4_scene2_zone(&s_world)]);
        hud_number(values[0], s_world.d >> 16, 0);
        strcat(values[0], "m");
        strcpy(values[29], ridge_zone_tags[r4_scene2_zone(&s_world)]);
    }
#else
    {
        core_out o;core_out_init(&o,rows[29],sizeof rows[0]);
        if(GFX2D_R4_JUDGE)core_out_str(&o,"PRESENT/RAST TEST");
        else {
            core_out_str(&o,"FPS ");
            if(s_fps_valid)core_out_u32(&o,s_fps);else core_out_str(&o,"--");
            core_out_str(&o,"  RAST ");
            if(s_hw) {
                uint32_t ms=cycles_to_ms16(s_rast_cyc);
                core_out_u32(&o,ms>>16);core_out_ch(&o,'.');
                core_out_u32(&o,((ms&65535u)*10u)>>16);
            } else core_out_str(&o,"--");
        }
    }
#endif
    for(unsigned s=0;s<30;++s) {
        gfx_list l;gfx_rect bar={(int)R4_WORLD_WIDTH,0,639,15};
        if(!GFX_PRODUCT_HUD && s<4){list[s]=0;continue;}
        /* Each destination owns one persistent arena. A row is reused only
           when all its visible inputs match, after the previous frame drained. */
        int same=s_hud_valid[fb][s] && !strcmp(rows[s],previous_rows[fb][s]) &&
            !strcmp(values[s],previous_values[fb][s]);
        if(s==12)same&=previous_icons[fb][0]==s_world.shield;
        if(s==14)same&=previous_icons[fb][1]==s_world.bombs;
        if(!GFX_PRODUCT_HUD && (s==21 || s==22))same&=!strcmp(active,previous_active[fb]);
        if(same){list[s]=s_hud_count[fb][s];continue;}
        gfx_list_begin(&l,(gfx_cmd *)(void *)((uint8_t *)list+(s+1)*32768u),1016,tab);
        bad+=gfx_fill(&l,dst,bar,R4_HUD_BG,GFX_EQ_COPY,0)!=0;
        bar.x1=(int)R4_WORLD_WIDTH+1;bad+=gfx_fill(&l,dst,bar,R4_HUD_LINE,GFX_EQ_COPY,0)!=0;
        if(s==(GFX_PRODUCT_HUD?18u:17u)) {
            gfx_rect line={492,0,627,1};
            bad+=gfx_fill(&l,dst,line,0x00224a5cu,GFX_EQ_COPY,0)!=0;
        }
        if(GFX_PRODUCT_HUD && (s==5 || s==11)) {
            /* 标题下与分数块下的青色分隔线 */
            gfx_rect line={492,0,627,1};
            bad+=gfx_fill(&l,dst,line,0x0035f2ff,GFX_EQ_COPY,0)!=0;
        }
        if(GFX_PRODUCT_HUD && (s==0 || s==29)) {
            /* HUD 顶/底横向描边 */
            gfx_rect edge={482,(s==0)?0u:14u,627,(s==0)?1u:15u};
            bad+=gfx_fill(&l,dst,edge,0x0035f2ff,GFX_EQ_COPY,0)!=0;
        }
        if(GFX_PRODUCT_HUD && (s<=2 || s>=27)) {
            /* 右缘竖向括号:上三行与下三行各一段 */
            gfx_rect tick={626,0,627,15};
            bad+=gfx_fill(&l,dst,tick,0x0035f2ff,GFX_EQ_COPY,0)!=0;
        }
        if(s==12 || (s==14 && s_scene!=2u))for(unsigned i=0;i<3;++i) {
            uint32_t count=s==12?s_world.shield:s_world.bombs;
            gfx_rect icon={564+(int)i*22,3,577+(int)i*22,14};
            bad+=gfx_fill(&l,dst,icon,i<count?(s==12?0x0041ffb0:0x00c9a6ff):0x0012314a,
                GFX_EQ_COPY,0)!=0;
        }
#if R4_HUD_TEXT
        uint32_t drawn=0;
#if GFX_PRODUCT_HUD
        int hud_world=(s_scene==2u && (s==0u || s==29u));
        char fit[18];myriad_hud_fit(fit,rows[s],values[s][0]?7u:17u);
        if(fit[0] && !hud_world)bad+=stg_text_draw(&l,(s==21)?&white:&cyan,
            dst,MYRIAD_HUD_TEXT_X,0,fit,&drawn);
        if(values[s][0] && !hud_world)bad+=stg_text_draw(&l,(s==7 || s==10)?&amber:&white,
            dst,myriad_hud_value_x(values[s]),0,values[s],&drawn);
        if(hud_world) {
            /* 参考版式：世界区左上 zone 名+细线、右上 RINGS n/6、左下 tagline。
               各配一块小深板垫字：白云/亮雪地上白字也要可读。 */
            if(s==0u) {
                uint32_t len=0;while(rows[0][len])++len;
                {
                    gfx_rect plate={6,0,(int)(10+(int)len*8),16};
                    bad+=gfx_fill(&l,dst,plate,0x00182833,GFX_EQ_COPY,0)!=0;
                }
                bad+=stg_text_draw(&l,&white,dst,10,0,rows[0],&drawn);
                {
                    gfx_rect rule={10,14,(int)(10+(int)drawn*8),14};
                    bad+=gfx_fill(&l,dst,rule,0x0090b8d0,GFX_EQ_COPY,0)!=0;
                }
                if(values[0][0]) {
                    uint32_t len2=0;while(values[0][len2])++len2;
                    gfx_rect plate={0,0,0,0};
                    (void)plate;
                    bad+=gfx_fill(&l,dst,(gfx_rect){(int)(R4_WORLD_WIDTH-12-(int)len2*8),0,
                        (int)(R4_WORLD_WIDTH-6),16},0x00182833,GFX_EQ_COPY,0)!=0;
                    bad+=stg_text_draw(&l,&white,dst,(int)(R4_WORLD_WIDTH-10-(int)len2*8),0,values[0],&drawn);
                }
            } else {
                uint32_t len=0;while(values[29][len])++len;
                {
                    gfx_rect plate={6,0,(int)(10+(int)len*8),16};
                    bad+=gfx_fill(&l,dst,plate,0x00182833,GFX_EQ_COPY,0)!=0;
                }
                bad+=stg_text_draw(&l,&white,dst,10,0,values[29],&drawn);
            }
        }
#else
        if(rows[s][0])bad+=stg_text_draw(&l,(s==7 || s==10 || s==18 || s==19)?&white:&cyan,
            dst,492,0,rows[s],&drawn);
        if(values[s][0])bad+=stg_text_draw(&l,&white,dst,584,0,values[s],&drawn);
#endif
#endif
        if(!GFX_PRODUCT_HUD && (s==21 || s==22))for(unsigned i=0;active[i];++i)
            bad+=gfx_blit(&l,STG_SLOT_DIGITS,(active[i]-'0')*16,(s-21)*16,dst,
                492+(int)i*16,0,16,16,GFX_EQ_COPY,0,1,0)!=0;
        bad+=gfx_list_end(&l)!=0;list[s]=l.used-1;
        strcpy(previous_rows[fb][s],rows[s]);strcpy(previous_values[fb][s],values[s]);
        s_hud_count[fb][s]=list[s];s_hud_valid[fb][s]=1;
    }
    previous_icons[fb][0]=s_world.shield;previous_icons[fb][1]=s_world.bombs;
    strcpy(previous_active[fb],active);
    return bad;
}
#ifdef __riscv
/* SH's PC overlay cannot be written by the CPU while the GPU owns the frame.
 * Build the target HUD into the existing per-strip 2D phase instead. */
static uint32_t sh_hud_build(gfx_surface_table *tab,int dst,uint32_t *list)
{
    const stg_font font={STG_SLOT_FONT,ASSET_FONT8X16_CELL_W,ASSET_FONT8X16_CELL_H,
        ASSET_FONT8X16_COLS,0x20u,ASSET_FONT8X16_CELLS,0};
    static char previous[2][30][80];static unsigned previous_bar[2];
    uint32_t state[4],bad=0;sh_hud_read(state);
    unsigned fb=(unsigned)(dst-STG_SLOT_FB0),bar=state[3]*132u/65536u;
    for(unsigned strip=0;strip<30;++strip) {
        if(strip>2 && strip!=14 && strip!=15 && strip<28){list[strip]=0;continue;}
        char text[80]="";
        if(strip==0)snprintf(text,sizeof text,"HALTTIME  KILLS %u   AMMO %u  %s",(unsigned)state[0],(unsigned)state[1],state[2]?"RELOADING":"READY");
        if(strip==1)strcpy(text,"INVINCIBLE / ENDLESS  -  AUTO RELOAD");
        if(strip==2)snprintf(text,sizeof text,"TIME %u.%02u",(unsigned)(state[3]/65536u),(unsigned)((state[3]*100u/65536u)%100u));
        if(strip==28)strcpy(text,"UP/DOWN MOVE  LEFT/RIGHT TURN  SPACE FIRE");
        if(strip==29)strcpy(text,"6 HALTTIME    ESC MENU    STOP TO SLOW TIME");
        if(s_hud_valid[fb][strip] && !strcmp(previous[fb][strip],text) &&
           (strip!=2 || previous_bar[fb]==bar)){list[strip]=s_hud_count[fb][strip];continue;}
        gfx_list l;gfx_list_begin(&l,(gfx_cmd *)(void *)((uint8_t *)list+(strip+1u)*32768u),1016,tab);
        if(strip<3 || strip>=28) {
            bad+=gfx_fill(&l,dst,(gfx_rect){0,0,639,15},0x00242a32u,GFX_EQ_COPY,0)!=0;
            bad+=stg_text_draw(&l,&font,dst,12,0,text,NULL)!=0;
        }
        if(strip==2) {
            bad+=gfx_fill(&l,dst,(gfx_rect){160,5,291,9},0x00545c68u,GFX_EQ_COPY,0)!=0;
            if(bar)bad+=gfx_fill(&l,dst,(gfx_rect){160,5,159+(int)bar,9},0x00e04028u,GFX_EQ_COPY,0)!=0;
        }
        if(strip==14 || strip==15) {
            int offset=strip==14?14:0;
            bad+=gfx_fill(&l,dst,(gfx_rect){319,offset,321,strip==14?15:6},0x00242a32u,GFX_EQ_COPY,0)!=0;
            if(strip==15)bad+=gfx_fill(&l,dst,(gfx_rect){314,0,326,2},0x00242a32u,GFX_EQ_COPY,0)!=0;
            bad+=gfx_fill(&l,dst,(gfx_rect){320,offset,320,strip==14?15:5},0x00ffffffu,GFX_EQ_COPY,0)!=0;
            if(strip==15)bad+=gfx_fill(&l,dst,(gfx_rect){315,1,325,1},0x00ffffffu,GFX_EQ_COPY,0)!=0;
        }
        bad+=gfx_list_end(&l)!=0;list[strip]=l.used-1u;
        strcpy(previous[fb][strip],text);s_hud_valid[fb][strip]=1;s_hud_count[fb][strip]=list[strip];
    }
    previous_bar[fb]=bar;return bad;
}
#endif
#endif  /* !R4_HUD_NONE */

/* --- 背景与帧 CRC ----------------------------------------------------------- */

/* 离屏底色：深蓝纵向渐变（LOAD 每帧整屏读它）。场景 2 换成清晨天空。 */
static uint32_t bg_mix(uint32_t a, uint32_t b, uint32_t t)
{
    uint32_t r, g, bl;
    if (t > 256u) t = 256u;
    r = ((((a >> 16) & 255u) * (256u - t)) + (((b >> 16) & 255u) * t)) >> 8;
    g = ((((a >> 8) & 255u) * (256u - t)) + (((b >> 8) & 255u) * t)) >> 8;
    bl = (((a & 255u) * (256u - t)) + ((b & 255u) * t)) >> 8;
    return (r << 16) | (g << 8) | bl;
}

/* --- 场景 2 的天空美术包：渐变/剪影/中央远峰/霾带，随 zone 擦除式换装 --------
   换装不一次重烘整屏（那是一帧 20ms 级的卡顿），而是每帧 32 行自上而下推进
   ——30Hz 下约0.5s 的"天色刷下来"转场。 */

static const uint32_t bg_sky_stops[4][4] = {
    {0x4f9bcd, 0x9cc8ec, 0xffe2bc, 0xffdca8},   /* 晨光 */
    {0x5498c8, 0xa8cce0, 0xf6e0b4, 0xf2d8a4},   /* 金原 */
    {0x5a90b8, 0xa4c2d4, 0xf0d4ac, 0xeccaa0},   /* 红岩 */
    {0x8fb4d4, 0xc8dce8, 0xe8f0f4, 0xeef4f6},   /* 雪线 */
};
static const uint32_t bg_sils[4][3] = {
    {0xc4b2ac, 0x9e94a4, 0x74808c},
    {0xd8c0a0, 0xb09878, 0x8a7a60},
    {0xd8a880, 0xb08060, 0x88604a},
    {0xe4ecf2, 0xc4d2de, 0xa4b8c8},
};
static const uint32_t bg_peaks[4][2] = {
    {0x9c90ac, 0x84789c},
    {0xa89484, 0x907c6c},
    {0xb08868, 0x987050},
    {0xdce6ee, 0xc2d2e0},
};
static uint32_t s_bg_zone = 0u, s_bg_zone_pend = 0u, s_bg_row = FB_HEIGHT;
static uint32_t s_sky_rows[4][FB_HEIGHT][4];
static int s_sky_rows_ready;

static int bg_haze_row(unsigned y)
{return y==96u || y==97u || y==112u || y==113u || y==128u || y==129u;}

static void bg_sky_cache(void)
{
    if(s_sky_rows_ready)return;
    static const uint8_t bayer[16]={0,8,2,10,12,4,14,6,3,11,1,9,15,7,13,5};
    static const uint32_t bounds[3]={400u,520u,1000u};
    for(unsigned zone=0;zone<4;++zone)for(unsigned y=0;y<FB_HEIGHT;++y) {
        unsigned ty=y*1000u/FB_HEIGHT,seg=0;
        while(ty>=bounds[seg])++seg;
        unsigned base=seg?bounds[seg-1u]:0u,span=bounds[seg]-base;
        for(unsigned x=0;x<4;++x) {
            int u=(int)((ty-base)*256u/span)+(int)bayer[(y&3u)*4u+x]-8;
            if(u<0)u=0;
            if(u>256)u=256;
            uint32_t c=bg_mix(bg_sky_stops[zone][seg],bg_sky_stops[zone][seg+1u],(unsigned)u);
            s_sky_rows[zone][y][x]=bg_haze_row(y)?bg_mix(c,0xffffffu,40u):c;
        }
    }
    s_sky_rows_ready=1;
}

/* 三层剪影的逐列脊线（视图无关）：zone 变才重算一次，宽屏/离屏两路共用。 */
static int16_t s_sil_y0[3][1792];
static uint32_t s_sil_zone = UINT32_MAX;
static int s_sil_top=FB_HEIGHT,s_sil_floor;

static void bg_sil_cache(uint32_t zone)
{
    /* Current silhouette geometry is identical across palettes. Colours are
       applied by the row painter; re-evaluating its sines at each zone is waste. */
    if (s_sil_zone != UINT32_MAX) return;
    for (uint32_t x = 0u; x < 1792u; ++x) {
        static const int base_y[3] = {252, 262, 272};
        static const int amp[3] = {52, 38, 24};
        static const int phase[3] = {0, 8000, 21000};
        for (unsigned k = 0u; k < 3u; ++k) {
            int a = (int)(x * 68u + (uint32_t)phase[k]) & 0xFFFF;
            int b = (int)(x * 26u + (uint32_t)phase[k] * 2u) & 0xFFFF;
            int s = fx_sin((uint16_t)a) / 2 + fx_sin((uint16_t)b) / 4;
            s_sil_y0[k][x] = (int16_t)(base_y[k] - (amp[k] * s) / 32768);
            if(s_sil_y0[k][x]<s_sil_top)s_sil_top=s_sil_y0[k][x];
            if(k==2u && s_sil_y0[k][x]>s_sil_floor)s_sil_floor=s_sil_y0[k][x];
        }
    }
    s_sil_zone = zone;
}

static void bg_paint_row_mode(uint32_t *row,uint32_t w,uint32_t zone,uint32_t y,int reference)
{
    /* Below the nearest silhouette every previous layer is overwritten. Avoid
       four complete DDR write passes during the lower part of a zone wipe. */
    if(!reference && (int)y>=s_sil_floor) {
        const uint32_t c=bg_sils[zone][2];unsigned x=0;
        for(;x+4u<=w;x+=4u){row[x]=c;row[x+1u]=c;row[x+2u]=c;row[x+3u]=c;}
        for(;x<w;++x)row[x]=c;
        return;
    }
#ifdef R4_FLIGHT_TEST
    if(reference) {
    static const uint8_t bayer[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
    static const uint32_t bounds[3] = {400u, 520u, 1000u};
    uint32_t ty = y * 1000u / FB_HEIGHT;
    unsigned seg = 0u;
    while (ty >= bounds[seg]) ++seg;
    {
        uint32_t a = bg_sky_stops[zone][seg], b = bg_sky_stops[zone][seg + 1u];
        uint32_t base = seg ? bounds[seg - 1u] : 0u;
        uint32_t span = bounds[seg] - base;
        for (uint32_t x = 0u; x < w; x++) {
            int32_t u = (int32_t)(((ty - base) * 256u) / span) +
                (int32_t)bayer[(y & 3u) * 4u + (x & 3u)] - 8;
            if (u < 0) u = 0;
            if (u > 256) u = 256;
            row[x] = bg_mix(a, b, (uint32_t)u);
        }
    }
    } else
#else
    (void)reference;
#endif
    {
        bg_sky_cache();
        const uint32_t *c=s_sky_rows[zone][y];unsigned x=0;
        for(;x+4u<=w;x+=4u){row[x]=c[0];row[x+1u]=c[1];row[x+2u]=c[2];row[x+3u]=c[3];}
        for(;x<w;++x)row[x]=c[x&3u];
    }
    /* 太阳：左上天域的两级光晕+亮核。 */
    {
        int dy = (int)y - 96;
        if (dy >= -46 && dy <= 46) {
            uint32_t sun[3]={0xfff6e2,bg_mix(bg_sky_stops[zone][1],0xfff4dc,140u),
                bg_mix(bg_sky_stops[zone][1],0xfff4dc,60u)};
            if(!reference && bg_haze_row(y))for(unsigned i=0;i<3;++i)sun[i]=bg_mix(sun[i],0xffffffu,40u);
            for (int x = 118 - 46; x <= 118 + 46 && x < (int)w; ++x) {
                if (x < 0) continue;
                {
                    int dx = x - 118;
                    int d2 = dx * dx + dy * dy;
                    if (d2 <= 22 * 22) row[x] = sun[0];
                    else if (d2 <= 32 * 32) row[x] = sun[1];
                    else if (d2 <= 46 * 46) row[x] = sun[2];
                }
            }
        }
    }
    /* 中央远峰：顶点 (240,150)，两张立面（向阳/背阳），底边融进剪影。 */
    if (y >= 150u && y < 285u) {
        int32_t hw = (int32_t)y - 150;
        if (hw > 135) hw = 135;
        for (int32_t x = 240 - hw; x < 240; ++x) row[x] = bg_peaks[zone][0];
        for (int32_t x = 240; x < 240 + hw && x < (int)w; ++x) row[x] = bg_peaks[zone][1];
    }
    /* 三层剪影（查表）。 */
    if(reference || (int)y>=s_sil_top)for (uint32_t x = 0u; x < w; ++x) {
        if ((int32_t)y >= s_sil_y0[0][x]) row[x] = bg_sils[zone][0];
        if ((int32_t)y >= s_sil_y0[1][x]) row[x] = bg_sils[zone][1];
        if ((int32_t)y >= s_sil_y0[2][x]) row[x] = bg_sils[zone][2];
    }
    /* 高空霾带：三条 2px 浅色横带（参考图的空气感）。 */
    if (reference && (y == 96u || y == 97u || y == 112u || y == 113u || y == 128u || y == 129u))
        for (uint32_t x = 0u; x < w; ++x)
            row[x] = bg_mix(row[x], 0xffffffu, 40u);
}

static void bg_paint_row(uint32_t *row,uint32_t w,uint32_t zone,uint32_t y)
{bg_paint_row_mode(row,w,zone,y,0);}

#ifdef R4_FLIGHT_TEST
void r4_flight_bg_test(uint32_t *row,unsigned width,unsigned zone,unsigned y,int reference)
{bg_sil_cache(zone);bg_paint_row_mode(row,width,zone,y,reference);}
#endif

static void bg_bake_rows(uint32_t zone, uint32_t y0, uint32_t y1)
{
    uint32_t *bg = (uint32_t *)((uint8_t *)board_region(REGION_OFFSCREEN).ptr+R4_BG_OFFSET);
    if (zone > 3u) zone = 0u;
    bg_sil_cache(zone);
    for (uint32_t y = y0; y < y1 && y < FB_HEIGHT; ++y) {
        uint32_t *row = bg + (size_t)y * FB_ROW_PIXELS;
        if (s_scene == 2u) {
            bg_paint_row(row, FB_WIDTH, zone, y);   /* 无 HUD：世界吃满 640 */
            continue;
        }
        bg_paint_row(row, R4_WORLD_WIDTH, zone, y);
        row[R4_WORLD_WIDTH] = R4_HUD_LINE;
        for (uint32_t x = R4_WORLD_WIDTH + 1u; x < FB_WIDTH; x++) row[x] = R4_HUD_BG;
    }
}

static int build_background(const gfx_surface_table *tab,const gfx_mem *mem)
{
    if (s_scene == 2u) {
        s_bg_zone = s_bg_zone_pend = 0u;
        s_bg_row = FB_HEIGHT;
        bg_bake_rows(0u, 0u, FB_HEIGHT);
        return 0;
    }
    if (s_scene == 3u) {                     /* 凝时:素色暗背景,画面全靠 rast 房间 */
        uint32_t *bg = (uint32_t *)((uint8_t *)board_region(REGION_OFFSCREEN).ptr+R4_BG_OFFSET);
        for (uint32_t y = 0u; y < FB_HEIGHT; y++) {
            uint32_t *row = bg + (size_t)y * FB_ROW_PIXELS;
            const uint32_t c = 0x002a2d33u + ((y * 6u) / FB_HEIGHT) * 0x00010101u;
            for (uint32_t x = 0u; x < FB_WIDTH; x++) row[x] = c;
        }
        return 0;
    }
    {
        uint32_t *bg = (uint32_t *)((uint8_t *)board_region(REGION_OFFSCREEN).ptr+R4_BG_OFFSET);
        uint32_t  x, y;


    for (y = 0u; y < FB_HEIGHT; y++) {
        uint32_t *row = bg + (size_t)y * FB_ROW_PIXELS;
        const uint32_t c = R4_COLOR_BG + ((y * 24u) / FB_HEIGHT) * 0x00010102u;

        for (x = 0u; x < FB_WIDTH; x++)
            row[x] = x<R4_WORLD_WIDTH?c:R4_HUD_BG;
        row[R4_WORLD_WIDTH]=R4_HUD_LINE;
    }
    if(GFX_PRODUCT_HUD)return 0;
    /* Area-filter the existing premultiplied 240x80 title to 144x48 once at
       entry. It remains in DDR BG; no new Flash assets or per-frame rescale. */
    const gfx_surf_raw *title=&tab->s[STG_SLOT_TITLE];
    if(!gfx_surf_valid(title) || gfx_surf_width(title)<240 || gfx_surf_height(title)<80)return -1;
    for(y=0;y<48;++y)for(x=0;x<144;++x) {
        unsigned sx=x*5u/3u,sy=y*5u/3u,channels[4]={0};
        for(unsigned dy=0;dy<2;++dy)for(unsigned dx=0;dx<2;++dx) {
            uint32_t address=gfx_addr_of(title,(int32_t)(sx+dx),(int32_t)(sy+dy));
            if(address<mem->dev_base || address-mem->dev_base>mem->bytes-4u)return -1;
            uint32_t pixel=*(const uint32_t *)(const void *)(mem->host+address-mem->dev_base);
            for(unsigned k=0;k<4;++k)channels[k]+=(pixel>>(k*8u))&255u;
        }
        uint32_t result=0,alpha=(channels[3]+2u)/4u;
        uint32_t previous=bg[(y+8u)*FB_ROW_PIXELS+R4_WORLD_WIDTH+8u+x];
        for(unsigned k=0;k<3;++k) {
            unsigned value=(channels[k]+2u)/4u+(((previous>>(k*8u))&255u)*(255u-alpha)+127u)/255u;
            result|=(value>255u?255u:value)<<(k*8u);
        }
        bg[(y+8u)*FB_ROW_PIXELS+R4_WORLD_WIDTH+8u+x]=result;
    }
    return 0;
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
    uint32_t fail_before=s_fail;
    s_ladder = (ladder >= 2u) ? 2u : 1u;
#ifdef __riscv
    s_hw = hw ? 1u : 0u;
#else
    (void)hw;
    s_hw = 0u; /* PC uses rast_ref; never label that path as FPGA rendering. */
#endif
    memset(&s_out, 0, sizeof s_out);
    memset(s_hud_valid,0,sizeof s_hud_valid);
    s_out.ladder = s_ladder;
    s_out.hw = s_hw;
    s_out.scene = s_scene;
    if (s_scene == 2u) {
        /* 山脊滑翔无 HUD（用户定案）：世界吃满整幅 640x480，投影切 4:3。 */
        s_vp=(rast_viewport){0,0,FB_WIDTH,FB_HEIGHT};
    } else if (s_scene == 3u) {
        s_vp=(rast_viewport){0,0,FB_WIDTH,FB_HEIGHT};   /* 凝时:同样无 HUD 满幅 */
    } else {
        s_vp=(rast_viewport){R4_VP_CORRIDOR};
    }
    s_raster_frame=(rast_frame){RAST_WIDTH,RAST_HEIGHT};
    {
        static uint32_t last_scene=UINT32_MAX,last_ladder=UINT32_MAX;
        if(last_scene!=s_scene || last_ladder!=s_ladder)memset(s_render_valid,0,sizeof s_render_valid);
        last_scene=s_scene;last_ladder=s_ladder;
    }
    s_time_count=0u;s_window_late=0u;s_window_exact=0u;s_window_over=0u;
    s_render_sum=0;s_render_n=0;
    if (fx16_perspective(&s_proj, R4_FOV_BAM,
                         s_scene==0u?R4_ASPECT:fx16_div(4*FX16_ONE,3*FX16_ONE),
                         R4_NEAR_Z, R4_FAR_Z) != 0) {
        s_fail++;
        return -1;
    }
    if(s_scene==0u)s_proj.v[6] = -R4_ONE/8; /* 480 高的世界：灭点 (中心,210)。 */
    if(s_replay_on && (s_replay.scene_id!=r4_scene_identity(s_scene) ||
        s_replay.ladder!=s_ladder || s_replay.asset_version!=R4_ASSET_VERSION)) {
        ++s_fail;return -1;
    }
    if(board_region(REGION_SHARED).size<R4_DEPTH_OFF+32768u) {++s_fail;return -1;}
    if(board_region(REGION_OFFSCREEN).size<R4_BG_OFFSET+FB_BYTES) {++s_fail;return -1;}
    r4_world_init(&s_world, s_replay_on?s_replay.seed:R4_SEED);
    s_world.ladder=s_ladder;
    s_world.scene=s_scene;
    if(s_scene==2u)r4_scene2_fix_gates(&s_world);
    if(s_scene==3u)sh_profile_clock(board_cycles);
    if(s_scene==3u) {
        if(board_region(REGION_SHARED).size<R4_SH_WORLD_OFF+FB_BYTES){++s_fail;return -1;}
        sh_world_cache_bind((uint8_t *)board_region(REGION_SHARED).ptr+R4_SH_CACHE_OFF,SH_CACHE_BYTES);
        /* A private intermediate surface permits two independent Z passes without
           source/destination aliasing or touching the display's front buffer. */
        gfx_surface surface={0};
        surface.width=FB_WIDTH;surface.height=FB_HEIGHT;surface.stride_log2=FB_STRIDE_LOG2;
        surface.space=GFX_SPACE_DDR;surface.format=GFX_FMT_XRGB8888;
        surface.base=g_mem_regions[REGION_SHARED].offset+R4_SH_WORLD_OFF;
        if(gfx_surface_set(tab,R4_SH_WORLD_SLOT,&surface)!=GFX_OK){++s_fail;return -1;}
#ifdef __riscv
        if(s_hw && gfx_engine_surf_set_space(R4_SH_WORLD_SLOT,surface.base,FB_WIDTH,FB_HEIGHT,
            FB_STRIDE_LOG2,GFX_SPACE_DDR,GFX_FMT_XRGB8888)!=GFX_ENG_OK){++s_fail;return -1;}
#endif
    }
    if(s_scene==3u)sh_world_init(s_replay_on?s_replay.seed:R4_SEED);
    s_rec = (rast_record *)(void *)((uint8_t *)board_region(REGION_SHARED).ptr + R4_REC_OFF);
    s_record_capacity=R4_REC_MAX;
#ifndef __riscv
    if(s_scene==3u && !s_hw){s_rec=s_sh_records;s_record_capacity=SH_PC_RECORD_CAP;}
#endif
    s_sub = (uint8_t *)board_region(REGION_SHARED).ptr + R4_SUB_OFF;
    s_depth = (uint16_t *)(void *)((uint8_t *)board_region(REGION_SHARED).ptr + R4_DEPTH_OFF);
    if(s_scene==0u && r4_world_prewarm(&s_proj,&s_vp,s_ladder,s_rec,R4_REC_MAX)) {
        ++s_fail;return -1;
    }
    if(s_scene==2u && r4_flight_prewarm(&s_world,&s_proj,&s_vp)) {++s_fail;return -1;}

    if(build_background(tab,mem)){++s_fail;return -1;}
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

    /* The 32 KiB header has 30 strip counts and zero reserved padding.
       Neither the engine nor the HUD cache writes the padding. Clear it once
       for each destination on entry, before reusing legacy command arenas. */
    memset(board_region(REGION_CMDLIST_A).ptr,0,32768u);
    memset(board_region(REGION_CMDLIST_B).ptr,0,32768u);

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
                        s_replay_on?s_replay.seed:R4_SEED, r4_scene_identity(s_scene), s_ladder, R4_ASSET_VERSION);
#endif
    say_u32("r4 enter ladder/hw ", s_ladder, s_hw);
    return (s_fail == fail_before) ? 0 : -1;
}

void r4_timing_start(void)
{
    s_sh_previous_ms=(uint32_t)board_time_ms();
    s_anchor = board_frame_count();
    s_fps_start=board_cycles();s_swaps0=board_swap_count();
}

void r4_mode_leave(void)
{
    if(s_scene==3u)sh_world_cache_bind(NULL,0);
    if(s_scene==2u)r4_flight_cache_reset();
#ifndef __riscv
    free(s_canvas_depth);
#endif
    s_canvas_depth=NULL;s_canvas_depth_stride=0;
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

static int submit_hw_part(gfx_surface_table *tab,int dst_slot,uint32_t bg,uint32_t list,
                          uint32_t first,uint32_t sub_off,uint32_t sub_cap,
                          uint32_t bytes,uint32_t count,uint32_t *cycles_out)
{
    gfx_engine_raster p;
    uint32_t code = ~0u, index = 0u, tag = 0u, id = 0u, caps = 0u;
    uint32_t cycles = 0u, refs = 0u;
    const uint32_t shared = g_mem_regions[REGION_SHARED].offset;

    p.tri_base = shared + R4_REC_OFF + first*RAST_RECORD_BYTES;
    p.tri_alloc = count * RAST_RECORD_BYTES;
    p.sub_base = shared + sub_off;
    p.sub_alloc = sub_cap;
    p.sub_bytes = bytes;
    p.tri_count = count;
    p.ras_key = 0x00000000u;                    /* atlas 透明色 = 全 0 */
    p.texcfg = 0u;
    {
        const int sr = gfx_engine_submit_raster(list,bg,(uint32_t)dst_slot,&p);

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
    *cycles_out=cycles;
    return 0;
}

static int submit_hw(gfx_surface_table *tab,int dst_slot,int fb_index,
                     uint32_t bytes,uint32_t count,uint32_t *crc_out)
{
    uint32_t world_cycles=0,gun_cycles=0;
    if(s_scene==3u && !s_sh_world_count) {
        if(submit_hw_part(tab,dst_slot,R4_SLOT_BG,s_hud_device,0,R4_SH_GUN_SUB_OFF,
            R4_SH_GUN_SUB_CAP,s_sh_gun_bytes,count,&gun_cycles))return -1;
    } else if(s_scene==3u && s_sh_world_count==count) {
        if(submit_hw_part(tab,dst_slot,R4_SLOT_BG,s_hud_device,0,R4_SUB_OFF,R4_SUB_CAP,
            bytes,count,&world_cycles))return -1;
    } else if(s_scene==3u) {
        const unsigned empty_region=fb_index?REGION_CMDLIST_A:REGION_CMDLIST_B;
        memset(board_region(empty_region).ptr,0,R4_HUD_ROWS*sizeof(uint32_t));
        if(submit_hw_part(tab,R4_SH_WORLD_SLOT,R4_SLOT_BG,g_mem_regions[empty_region].offset,
             0,R4_SUB_OFF,R4_SUB_CAP,bytes,s_sh_world_count,&world_cycles) ||
           submit_hw_part(tab,dst_slot,R4_SH_WORLD_SLOT,s_hud_device,s_sh_world_count,
             R4_SH_GUN_SUB_OFF,R4_SH_GUN_SUB_CAP,s_sh_gun_bytes,count-s_sh_world_count,&gun_cycles))
            return -1;
    } else if(submit_hw_part(tab,dst_slot,R4_SLOT_BG,s_hud_device,0,R4_SUB_OFF,R4_SUB_CAP,
                             bytes,count,&world_cycles))return -1;
    s_rast_cyc=world_cycles+gun_cycles;s_out.rast_cycles=s_rast_cyc;
    *crc_out = 0u;
    (void)fb_index;
    return 0;
}
#endif

/* 软件路径：bg 整屏 BLIT -> rast_ref 逐带直写帧缓冲 -> HUD 逐带 exec。 */
static int frame_layout(int index)
{
    if(board_frame_acquire(index,!GFX2D_R4_JUDGE && !R4_BOARD_CRC && !s_replay_on,&s_frame))return -1;
    if(s_frame.width<FB_WIDTH || s_frame.width>RAST_FRAME_MAX_WIDTH ||
        s_frame.height!=FB_HEIGHT || s_frame.stride<s_frame.width)return -1;
    if(s_frame.width!=FB_WIDTH && s_canvas_depth_stride!=s_frame.stride) {
#ifdef __riscv
        return -1; /* The fixed hardware path never owns a PC heap canvas. */
#else
        uint16_t *depth=(uint16_t *)realloc(s_canvas_depth,
                                          (size_t)s_frame.stride*RAST_STRIP_H*sizeof(uint16_t));
        if(!depth)return -1;
        s_canvas_depth=depth;s_canvas_depth_stride=s_frame.stride;
#endif
    }
    if(s_raster_frame.width!=s_frame.width) {
        s_raster_frame.width=s_frame.width;
        /* 山脊滑翔无 HUD：世界吃满整幅宽；走廊仍留右侧 160 HUD 带。 */
        s_vp.w=(uint16_t)(s_scene==0u?s_frame.width-160u:s_frame.width);
        fx16_t aspect=fx16_div((fx16_t)s_vp.w*FX16_ONE,(fx16_t)s_vp.h*FX16_ONE);
        if(fx16_perspective(&s_proj,R4_FOV_BAM,aspect,R4_NEAR_Z,R4_FAR_Z))return -1;
        if(s_scene==0u)s_proj.v[6]=-R4_ONE/8;
        memset(s_render_valid,0,sizeof s_render_valid);s_render_sum=0;s_render_n=0;
    }
    return 0;
}

/* --- 场景 2 的门环：改走 rast 记录（r4_scene.c ridge_gate_emit） -------------
   基线版用逐像素叠加只活在软件路径（硬件光栅看不见环），且不吃 z 测试会被
   近山盖脸。现在环=普通记录：硬件/软件同一份几何同一份像素，CRC 契约恢复。 */

static int render_sw(gfx_surface_table *tab, const gfx_mem *mem, int dst_slot,
                     int fb_index, uint32_t count, uint32_t *crc_out)
{
    static exec_sw_error err;
    exec_sw_stats st;
    uint32_t *fb = board_fb(fb_index);
    uint32_t  s;
    const int wide=s_frame.width!=FB_WIDTH;

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
    if(wide)for(unsigned y=0;y<s_frame.height;++y) {
        uint32_t c=(s_scene==3u)
            ?0x002a2d33u+((y*6u)/FB_HEIGHT)*0x00010101u   /* 凝时:暗灰背景,房间自含 */
            :R4_COLOR_BG+((y*24u)/FB_HEIGHT)*0x00010102u;
        uint32_t *row=s_frame.pixels+(size_t)y*s_frame.stride;
        if(s_scene==2u) {
            /* 无 HUD：世界吃满整幅宽，同一行画函数直接画进宽帧（跟随换装锋面）。 */
            bg_sil_cache(s_bg_zone);
            bg_paint_row(row,s_frame.width,(y<s_bg_row)?s_bg_zone_pend:s_bg_zone,y);
        } else {
            for(unsigned x=0;x<s_frame.width;++x)row[x]=c;
        }
    }
    for (s = 0u; s < 30u; s++) {
        rast_target t;
        uint32_t row, x;
        if(!GFX2D_R4_JUDGE)
            s_pending_keys |= (s_key_source ? s_key_source() : board_keys())
                & (BOARD_KEY_SWAP|BOARD_KEY_SLOW|
                BOARD_KEY_MODE1|BOARD_KEY_MODE2|BOARD_KEY_MODE3);

        /* depth 与 color 同 stride（契约 §4）：按 1024 的实际行距清 z。 */
        uint16_t *depth=wide?s_canvas_depth:s_depth;
        for (row = 0u; row < 16u; row++)
            for (x = 0u; x < s_frame.stride; x++)
                depth[row * s_frame.stride + x] = 65535u;
        t.color = s_frame.pixels + (size_t)s * 16u * s_frame.stride;
        t.depth = depth;
        t.stride = s_frame.stride;
        t.strip = s;
        t.texture = s_atlas;
        t.tex_width = 64u;
        t.tex_height = 64u;
        t.tex_stride = 64u;
        t.key = 0x00000000u;
        const uint32_t weapon=s_scene==3u?sh_world_weapon_start():count;
        if(weapon>count)return -1;
        if (rast_ref_run_frame(s_rec, weapon, &t,&s_raster_frame) != 0)
            return -1;
        if(weapon<count) {
            /* FPS viewmodel has its own depth range. Walls must not hide it. */
            for(row=0;row<16u;++row)for(x=0;x<s_frame.stride;++x)
                depth[row*s_frame.stride+x]=65535u;
            if(rast_ref_run_frame(s_rec+weapon,count-weapon,&t,&s_raster_frame)!=0)return -1;
        }
    }
    if(s_scene==0u
#ifdef __riscv
       || s_scene==3u
#endif
    )for (s = 0u; s < 30u; s++) {
        /* HUD 命令是条带局部 y：软件路径把目的表面基址平移到本条带首行，
           引擎的 BAND 相位则由硬件把局部 y 落进对应条带——两路同语义。
           山脊滑翔无 HUD（用户定案），整段跳过。 */
        gfx_surface_table t2 = *tab;
        uint32_t *hdr = s_hud_list;
        gfx_cmd  *cmds = (gfx_cmd *)(void *)((uint8_t *)hdr + (s + 1u) * 32768u);

        t2.s[dst_slot].base += s * 16u * FB_STRIDE_BYTES;
        exec_sw_stats_clear(&st);
        if (exec_sw_run(&s_es, &t2, mem, cmds, hdr[s], hdr[s], &st, &err) != GFX_OK)
            return -1;
    }
    if(wide && s_scene==0u)for(unsigned y=0;y<FB_HEIGHT;++y)
        memcpy(s_frame.pixels+(size_t)y*s_frame.stride+s_frame.width-160u,
               fb+(size_t)y*FB_ROW_PIXELS+R4_WORLD_WIDTH,160u*sizeof(uint32_t));
#ifndef __riscv
    if(s_scene==3u)sh_world_overlay(s_frame.pixels,s_frame.width,FB_HEIGHT,s_frame.stride);
#endif
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
    uint32_t       *list;
    int             rc;

    if (fb_index < 0 || fb_index > 1) {
        s_fail++;
        return -1;
    }
    if(frame_layout(fb_index)) {++s_fail;return -1;}
    unsigned cmd_region=fb_index?REGION_CMDLIST_B:REGION_CMDLIST_A;
    list=(uint32_t *)board_region(cmd_region).ptr;
    s_hud_list=list;s_hud_device=g_mem_regions[cmd_region].offset;
    /* 1) 逻辑 tick（30Hz，确定性）。 */
    t_seg = board_cycles();
    if(s_replay_on && replay_r4_key_at(&s_replay,s_out.frames,&keys)) {++s_fail;return -1;}
    if(s_scene==3u) {
#if defined(__riscv) && !GFX2D_R4_JUDGE
        uint32_t now=(uint32_t)board_time_ms();
        sh_world_advance(keys,now-s_sh_previous_ms,&s_fail);s_sh_previous_ms=now;
#else
        sh_world_tick(keys,&s_fail);
#endif
    } else
    r4_world_tick(&s_world, keys, &s_fail);
    if(s_scene==2u) {
        /* User-selected 32 rows per 30Hz frame: approximately 0.5s wipe. */
        uint32_t zone=r4_scene2_zone(&s_world);
        if(zone!=s_bg_zone_pend){s_bg_zone_pend=zone;s_bg_row=0u;}
        if(s_bg_zone!=s_bg_zone_pend) {
            uint32_t y1=s_bg_row+32u;if(y1>FB_HEIGHT)y1=FB_HEIGHT;
            bg_bake_rows(s_bg_zone_pend,s_bg_row,y1);
            s_bg_row=y1;
            if(s_bg_row>=FB_HEIGHT)s_bg_zone=s_bg_zone_pend;
        }
    }
    if(s_scene==3u){s_out.score=sh_score();s_out.shield=sh_shield();s_out.ticks=sh_ticks();}
    else{s_out.score=s_world.score;s_out.shield=s_world.shield;s_out.ticks=s_world.ticks;}
    s_out.fail=s_fail;
    s_out.cyc_logic = board_cycles() - t_seg;
#if GFX2D_R4_RECORD
    (void)replay_r4_rec_put(&s_tape, keys);
#endif

    /* 2) 几何（发射 + setup）与分桶。 */
    t_seg = board_cycles();
    produced = (s_scene==3u)
        ? sh_world_emit(&s_proj, &s_vp, s_rec, s_record_capacity)
        : r4_world_emit(&s_world, &s_proj, &s_vp, s_ladder, s_rec, R4_REC_MAX, NULL);
    s_out.cyc_geom = board_cycles() - t_seg;
    if (produced == 0u || produced > s_record_capacity) {
        s_fail++;
        say_u32("r4 err records ", produced, 0u);
        return -1;
    }
    t_seg = board_cycles();
    s_sh_world_count=s_scene==3u?sh_world_weapon_start():produced;
    const int bin_bytes=rast_bin_frame(s_rec,s_sh_world_count,s_sub,R4_SUB_CAP,&s_raster_frame);
    bytes=bin_bytes>0?(uint32_t)bin_bytes:0u;
    s_sh_gun_bytes=0;
    if(s_scene==3u && s_sh_world_count<produced) {
        uint8_t *gun_sub=(uint8_t *)board_region(REGION_SHARED).ptr+R4_SH_GUN_SUB_OFF;
        int gun_bytes=rast_bin_frame(s_rec+s_sh_world_count,produced-s_sh_world_count,
                                    gun_sub,R4_SH_GUN_SUB_CAP,&s_raster_frame);
        if(gun_bytes<120){++s_fail;return -1;}
        s_sh_gun_bytes=(uint32_t)gun_bytes;
    } else if(s_scene==3u && s_sh_world_count>produced){++s_fail;return -1;}
    s_out.cyc_bin = board_cycles() - t_seg;
    if (bytes < 120u) {
        s_fail++;
        say_u32("r4 err bin ", bytes, produced);
        return -1;
    }

    /* 3) HUD 条带表：重建 30 个计数；保留进入时已清零的头部填充。 */
    s_records=produced;
    t_seg = board_cycles();
    memset(list, 0, R4_HUD_ROWS*sizeof *list);
#if R4_HUD_NONE
    hud_bad = 0u;
#else
    hud_bad = (s_scene==0u)?hud_build_strips(tab, dst_slot, list):0u;
#ifdef __riscv
    if(s_scene==3u)hud_bad=sh_hud_build(tab,dst_slot,list);
#endif
    /* 山脊无HUD；凝时目标端使用条带HUD。 */
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
    s_out.refs = (bytes - 120u) / 2u+(s_sh_gun_bytes?(s_sh_gun_bytes-120u)/2u:0u);
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
    if(s_scene==2u && !GFX2D_R4_JUDGE && s_out.render_cycles>BOARD_HZ/30u) {
        char line[160];core_out o;core_out_init(&o,line,sizeof line);core_out_str(&o,"R4SLOW");
        r4_flight_cache_stats cache=r4_flight_cache_status();
        const uint32_t values[]={s_out.frames,s_out.cyc_logic,s_out.cyc_geom,s_out.cyc_bin,
            s_out.cyc_hud,s_out.cyc_submit,s_world.d>>16,cache.columns};
        for(unsigned i=0;i<8;++i){core_out_ch(&o,' ');core_out_u32(&o,values[i]);}
        core_out_str(&o,"\r\n");say(line);
    }
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
    while (!GFX2D_R4_JUDGE && (s_scene==0u || s_scene==2u || s_scene==3u) &&
           (int32_t)(board_frame_count() - (due - 1u)) < 0) {
        if (board_should_quit())
            break;
        board_wait_yield();
    }
    {
        uint32_t ticket = 0u;

        if (board_present_frame_request(fb_index,&s_frame, &ticket) != 0 ||
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
        /* SH physics follows elapsed time. Keep the actual late-frame evidence,
           then recover the next 30Hz deadline instead of replaying stale slots. */
        if(s_scene==3u && (int32_t)(actual-due)>0)s_anchor+=actual-due;
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
            if(s_scene==2u) {
                r4_flight_cache_stats cache=r4_flight_cache_status();
                core_out_init(&o,line,sizeof line);core_out_str(&o,"R4FCACHE ");
                const uint32_t values[]={cache.cold,cache.late,cache.columns,cache.hits,cache.fallback};
                for(unsigned i=0;i<5;++i) {core_out_u32(&o,values[i]);core_out_ch(&o,i==4?'\r':' ');}
                core_out_ch(&o,'\n');say(line);
            }
            core_out_init(&o,line,sizeof line);core_out_str(&o,"R4TIME");
            uint32_t times[]={s_out.cyc_logic,s_out.cyc_geom,s_out.cyc_bin,
                s_out.cyc_hud,s_out.cyc_submit,s_out.rast_cycles,s_out.bbox_points,s_out.refs};
            for(unsigned i=0;i<8;++i){core_out_ch(&o,' ');core_out_u32(&o,times[i]);}
            core_out_str(&o,"\r\n");say(line);
#if SH_PROFILE
            if(s_scene==3u) {
                uint32_t profile[12];sh_profile_read(profile);
                core_out_init(&o,line,sizeof line);core_out_str(&o,"SHCPU");
                for(unsigned i=0;i<12;++i){core_out_ch(&o,' ');core_out_u32(&o,profile[i]);}
                core_out_str(&o,"\r\n");say(line);
            }
#endif
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

int r4_backend_hardware(void) { return (int)s_hw; }
uint32_t r4_current_scene(void) { return s_scene; }
uint32_t r4_current_ladder(void) { return s_ladder; }

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
