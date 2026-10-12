/******************************************************************************
* @file sw6_cfg.h: 基础 Demo 的常量、表面槽分配与编译期开关
*
* @brief  基础 Demo 要集中管理的数都在本文件，代码里不再出现魔数：
*         方块池与 N／k 的上下限、HUD 矩形与字库几何、表面槽分配、离屏槽步长、
*         以及四个编译期开关（判据模式、只跑一段、帧数上限、起始 N）。
*
*         编译期开关由 mk 的 EXTRA_CFLAGS 传进来，进编译选项标记文件，换开关必然重编；
*         板上没有 argv，所以入口不用运行时参数。
******************************************************************************/
#ifndef SW6_CFG_H
#define SW6_CFG_H

#include <stdint.h>

/* --- 场景 A：方块随机移动 ------------------------------------------------- */
#define SW6_POOL_MAX     5000u      /* 池容量，方块数上限 */
#define SW6_N_DEFAULT    100u       /* 起始活动数 */
#define SW6_N_STEP       100u       /* 上下键各增减多少 */
#define SW6_N_MIN        100u
#define SW6_N_MAX        SW6_POOL_MAX

/* 四个固定出屏样例的编号（池里的前四个），分别跨上、下、左、右边 */
#define SW6_SAMPLE_N     4u

/* --- 场景 B：大图切换 ----------------------------------------------------- */
#define SW6_SCENES       3u
#define SW6_K_DEFAULT    30u
#define SW6_K_MIN        1u
#define SW6_K_MAX        120u

/* 演示模式 A／B 自动轮换的帧数 */
#ifndef SW6_STAGE_FRAMES
#define SW6_STAGE_FRAMES 600u
#endif

/* 演示模式实时输入的种子；判据模式用的是录像头里的种子 */
#define SW6_SEED_DEMO    0x5E6D0003u

/* --- 表面槽 --------------------------------------------------------------- */
/* 0、1 是两张帧缓冲；2~9 是素材包 8 项，按 assets.h 的条目号顺序；
   10~12 是三张离屏图。16 个槽用到 13 个。 */
#define SW6_SLOT_FB0       0
#define SW6_SLOT_FB1       1
#define SW6_SLOT_ASSET0    2
#define SW6_SLOT_BG_GRAD   (SW6_SLOT_ASSET0 + 0)
#define SW6_SLOT_CHECKER   (SW6_SLOT_ASSET0 + 1)
#define SW6_SLOT_ATLAS     (SW6_SLOT_ASSET0 + 2)
#define SW6_SLOT_FONT      (SW6_SLOT_ASSET0 + 3)
#define SW6_SLOT_BULLET_PM (SW6_SLOT_ASSET0 + 4)
#define SW6_SLOT_BULLET_ST (SW6_SLOT_ASSET0 + 5)
#define SW6_SLOT_KEY_DEMO  (SW6_SLOT_ASSET0 + 6)
#define SW6_SLOT_SOLID8    (SW6_SLOT_ASSET0 + 7)
#define SW6_SLOT_OFF0      10
#define SW6_SLOT_OFF(i)    (SW6_SLOT_OFF0 + (int)(i))

/* 离屏层里每张整屏图占一个 2 MiB 槽，第四槽留给调试抓帧 */
#define SW6_OFF_SLOT_STEP  (2u * 1024u * 1024u)

/* --- HUD ------------------------------------------------------------------ */
#define SW6_HUD_X       400u
#define SW6_HUD_Y       0u
#define SW6_HUD_W       240u
#define SW6_HUD_H       160u
#define SW6_HUD_COLS    (SW6_HUD_W / SW6_FONT_CELL_W)   /* 30 列 */
#define SW6_HUD_LINES   9u

#define SW6_HUD_BG      0x00000000u     /* 底色纯黑：字形判据才只有两种像素 */
#define SW6_FONT_COLS   16u
#define SW6_FONT_CELL_W 8u
#define SW6_FONT_CELL_H 16u
#define SW6_FONT_FIRST  0x20u
#define SW6_FONT_LAST   0x7Eu
#define SW6_FONT_KEY    0x00000000u

/* --- 打底色（每帧第一条覆盖命令用的背景）--------------------------------- */
#define SW6_BG_COLOR    0x00101820u

/* --- 编译期开关 ----------------------------------------------------------- */
#if defined(SW6_JUDGE) && (SW6_JUDGE)
#define SW6_MODE_JUDGE 1
#else
#define SW6_MODE_JUDGE 0
#endif

#ifdef SW6_ONLY
#define SW6_ONLY_STAGE ((int)(SW6_ONLY))
#else
#define SW6_ONLY_STAGE 0
#endif

/* 帧数上限。这里**不能**带 `(uint32_t)` 转型：`#if SW6_FRAMES_MAX != 0` 要在预处理阶段
   求值，而 `#if` 不认类型转换（`-DSW6_FRAMES=120` 时第一版就是这么编不过的）。 */
#ifdef SW6_FRAMES
#define SW6_FRAMES_MAX SW6_FRAMES
#else
#define SW6_FRAMES_MAX 0
#endif

#ifdef SW6_N
#define SW6_N_START ((uint32_t)(SW6_N))
#else
#define SW6_N_START SW6_N_DEFAULT
#endif

/* 反例注错：判据模式下 HUD 照样显示实时量，判据的两遍逐行比对（A 段 246 帧、B 段 367 帧各一遍）必须因此见红 */
#ifdef INJECT_SW6_LIVE_HUD
#define SW6_HUD_FORCE_LIVE 1
#else
#define SW6_HUD_FORCE_LIVE 0
#endif
#define SW6_HUD_LIVE (!SW6_MODE_JUDGE || SW6_HUD_FORCE_LIVE)

#endif /* SW6_CFG_H */
