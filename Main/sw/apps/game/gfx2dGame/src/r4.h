/******************************************************************************
* @file r4.h: 模式 4「三维演示」的常量与框架接口
*
* @brief  轨道射击分级演示；当前接口与操作见 sw/README.md 的 R4 一节。
*         世界=Q16.16 定点，相机作周期平移，世界物体共用同一视图；
*         几何出口走 lib/gfx 的子视口接口（契约 §2.1）：走廊视口左 480x480，
*         HUD 全部走 2D 条带命令（右 160 像素）。逻辑固定 30Hz tick、显式种子，
*         不从墙钟取位置；软件慢时同一逻辑帧仍产生同一场景。
*
*         阶梯：L1=平涂（TEX_EN=0,VCOLOR_EN=1），L2=TEX 仿射（含色键 billboard）。
*         两者同一位流（B7）上切换，不重综合。
*
*         内存：模式 4 独占 SHARED 内三个固定窗口（渲染槽 0x600000/256KiB、
*         子表 SHARED+0x40000/128KiB、软件 z 条带 SHARED+0x60000/32KiB）；凝时
*         凝时另用SHARED+0x68000/64KiB武器子表、0x100000/1MiB记录缓存、
*         0x200000/2MiB世界中间表面（槽3），世界/武器独立深度；与 renderer 的
*         实例/分桶 arena（0x400000 起）与背景命令表
*         （0x800000 起）互不重叠；mode_enter 前框架已做 frontend_game_reset
*         （在途非零即拒绝进入）。
******************************************************************************/
#ifndef GFX2D_R4_H
#define GFX2D_R4_H

#include <stdint.h>

/* Functional captures use a deterministic HUD; timing runs use live values. */
#ifndef GFX2D_R4_JUDGE
#define GFX2D_R4_JUDGE 0
#endif
#ifndef GFX2D_R4_HW
#define GFX2D_R4_HW 1
#endif
#ifndef GFX2D_R4_LADDER
#define GFX2D_R4_LADDER 2
#endif
#ifndef GFX2D_R4_SCENE
#define GFX2D_R4_SCENE 0
#endif

#include "core/logq.h"
#include "gfx/gfx.h"
#include "gfx/rast_setup.h"

/* --- 表面槽（boot_surfaces_r4 登记；15 是 stg_slots 空着的槽） ------------- */
#define R4_SLOT_BG          2           /* 独立三维背景表面 */
#define R4_BG_OFFSET        (6u * 1024u * 1024u) /* OFFSCREEN tail; first 3x2MiB belong to 2D */

/* --- 视口（契约 §2.1 子视口） -------------------------------------------- */
#define R4_WORLD_WIDTH      480u
#define R4_VP_CORRIDOR      0, 0, 480, 480      /* square world; compact 160px HUD */
#define R4_VP_CENTER        160, 120, 320, 240  /* cup3d 对照（D 批） */

/* --- 逻辑时钟与世界尺度 --------------------------------------------------- */
#define R4_TICK_HZ          30u
#define R4_FIELDS_PER_TICK  2u              /* 60Hz 场上的两场节拍 */
#define R4_SEED             0x52344631u     /* 显式种子（xs32） */

#define R4_ONE              65536           /* Q16.16 的 1.0 = 1 米 */
/* 走廊半宽 2.4m、半高 1.8m；五个深度区间、八面倒角截面。 */
#define R4_HALF_W           (12 * R4_ONE / 5)
#define R4_HALF_H           (9 * R4_ONE / 5)
#define R4_SEG_LEN          (4 * R4_ONE)
#define R4_SEG_COUNT        8
#define R4_NEAR_PAD         (R4_ONE / 2)    /* 近墙延伸至视口外，恒在近面之后 */

/* 门框：开口半宽约 1.67m，霓虹框半厚约 0.056m，每 10m 一道。 */
#define R4_GATE_OPEN        (5 * R4_ONE / 3)
#define R4_GATE_THICK       (R4_ONE / 18)
#define R4_GATE_SPACING     (10u * R4_ONE)
#define R4_GATE_FIRST       (12u * R4_ONE)

/* 自机（视空间固定深度）/ 弹。 */
#define R4_SHIP_Z           (11 * R4_ONE / 6)
#define R4_SHIP_HALF        (R4_ONE / 6)
#define R4_BULLET_HALF      (R4_ONE / 10)

/* 投影：fov 70.3125°（12800 BAM）、aspect 1:1、near 0.25m、far 64m。
   几何发射与投影矩阵共用这一组，改一处两边都要改。 */
#define R4_NEAR_Z           (R4_ONE / 4)
#define R4_FAR_Z            (64u * R4_ONE)
#define R4_FOV_BAM          12800u
#define R4_ASPECT           R4_ONE

/* 命中/擦弹判定（世界横向/纵向距离）。 */
#define R4_HIT_DIST         (R4_ONE / 5)
#define R4_GRAZE_DIST       (R4_ONE / 2)

/* 走廊检查上限；完整物体发射，容量不足显式失败，不截断记录。 */
#define R4_RECORD_BUDGET    256u
#define R4_RECORD_CAP       512u            /* 含完整396面茶杯与裁剪拆分 */

/* 逻辑上限。 */
#define R4_EBULLET_MAX      12
#define R4_PBULLET_MAX      12
#define R4_SHIELD_INIT      3u

/* 素材版本：atlas 生成脚本（tools/assets/gen_r4_atlas.py）与此同步才合法。 */
#define R4_ASSET_VERSION    3u
/* 场景身份：进录像头，回放判据拒绝身份不符的录像。 */
#define R4_SCENE_ID         0x52344334u     /* 视觉升级；与 cup 的 +1 身份不冲突 */
#define R4_FLIGHT_SCENE_ID  0x52464c33u     /* RFL3: damped control and world-anchored mesh. */
static inline uint32_t r4_scene_identity(uint32_t scene)
{return scene==2u?R4_FLIGHT_SCENE_ID:R4_SCENE_ID+scene;}
#ifdef R4_FLIGHT_TEST
void r4_flight_bg_test(uint32_t *row,unsigned width,unsigned zone,unsigned y,int reference);
#endif

/* 颜色（L1 平涂；ARGB，高字节不写）。 */
#define R4_COLOR_BG         0x00040810u
#define R4_COLOR_WALL       0x00304a68u
#define R4_COLOR_FLOOR      0x00283850u
#define R4_COLOR_CEIL       0x00203248u
#define R4_COLOR_GATE       0x00c06828u
#define R4_COLOR_SHIP       0x0040c888u
#define R4_COLOR_EBULLET    0x00ff5050u
#define R4_COLOR_PBULLET    0x00e8f0ffu

/* --- 状态与出口 ----------------------------------------------------------- */

typedef struct {
    uint32_t fail;          /* 自查失败累计（组帧拒绝、引擎错误码、present 失败） */
    uint32_t ticks;         /* 已推进的逻辑 tick 数 */
    uint32_t frames;        /* 已呈现的逻辑帧数 */
    uint32_t records;       /* 上一帧的裁剪后记录数 */
    uint32_t refs;          /* 上一帧的条带引用数 */
    uint32_t bbox_points;   /* 上一帧的 bbox 候选点数 */
    uint32_t score;
    uint32_t shield;
    uint32_t ladder;        /* 1=L1 2=L2 */
    uint32_t hw;            /* 1=硬件 RAST，0=软件 rast_ref */
    uint32_t scene;         /* 0=走廊，1=原396面茶杯 */
    /* 上一帧的五段耗时（周期数）。 */
    uint32_t cyc_logic, cyc_geom, cyc_bin, cyc_hud, cyc_submit;
    uint32_t rast_cycles;   /* 上一帧引擎 RAST 相位周期（hw 路径有效） */
    uint32_t render_cycles; /* 上一帧整帧渲染时间（含等待前） */
    uint32_t crc, record_crc, hud_crc;
} r4_out;

/* 进入/离开/一帧。tab/mem 由框架提供（表面表已按 R4 槽登记）。
   keys 是本逻辑 tick 的按键位图；dst_slot 是目的面槽号（STG_SLOT_FB0/1）。
   返回 0 成功；失败已累计内部 fail，调用方不得 present。 */
int      r4_mode_enter(gfx_surface_table *tab, const gfx_mem *mem, int hw, uint32_t ladder);
void     r4_timing_start(void);
void     r4_scene_select(uint32_t scene);
int      r4_set_replay(const uint8_t *data, uint32_t bytes);
uint32_t r4_pending_keys(void);
/* 键源注入：条带循环里的按键采样默认直读 board_keys()（裸机 sole reader）。
   宿主框架（console 的 inputd 任务独占 board_keys）必须注入自己的按住位图
   读取函数，否则两处读取会互相抢走 UART 解析器的字节。 */
void     r4_set_key_source(uint32_t (*fn)(void));
void     r4_mode_leave(void);
int      r4_mode_frame(gfx_surface_table *tab, const gfx_mem *mem, int dst_slot,
                       uint32_t keys, r4_out *out);

/* 框架在 main.c 提供的三个钩子：装载 atlas（进影子+TEX 窗口）、
   热集失效（3D 覆盖 TEX 后 s_tex_ready 必须归零）、热集重装（离开时）。 */
void     r4_set_hooks(int (*load_atlas)(void), void (*hotset_dirty)(void),
                      int (*hotset_restore)(void));
void     r4_set_log(logq *q);

/* 自查失败与节拍计数（框架的 say_enter/统计用）。 */
uint32_t r4_fail_count(void);
int r4_backend_hardware(void);
uint32_t r4_current_scene(void);
uint32_t r4_current_ladder(void);
uint32_t r4_exact_count(void);
uint32_t r4_late_count(void);

/* atlas 影子（16KiB）：软件路径 rast_ref 的纹理源，main.c 的装载钩子写入。 */
uint32_t *r4_atlas_shadow(void);

/* 模式 4 的整帧 CRC（逐行取 2560 字节可见区，与 stg_modes_frame_crc 同口径）。 */
uint32_t r4_frame_crc(const uint32_t *fb);

#endif /* GFX2D_R4_H */
