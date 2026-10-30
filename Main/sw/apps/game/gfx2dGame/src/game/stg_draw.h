/******************************************************************************
* @file stg_draw.h: 游戏的组帧与 HUD
*
* @brief  stg_game_compose 把一帧的游戏状态画成一张命令表：
*           SET_CLIP 游戏区 -> 天空底图（两笔 200 宽列切片 BLIT）-> 15 条横线 + 13 条竖线（STG_BG_FILLS 条 FILL）->
*           远景/城市剪影带（各两笔横向循环视差）-> 60 个星点（震动只偏移装饰层，天空不动保证覆盖）->
*           敌机（槽位顺序，COPY 加色键）-> Boss（ALPHA）-> 自机（COPY 加色键）-> 低速时的判定点（COPY 加色键）->
*           自机弹（ADD）-> 敌弹（ALPHA，按弹种）-> 粒子（ADD）-> 擦弹特效（ADD 四粒火花，计 fx_graze）->
*           标题图（带提示行）或 CLEAR（带得分，只在那两段）->
*           受击闪白（CONST FILL，c = 128；扣残机帧闪红，只在那一帧）-> SET_CLIP 全屏 -> HUD -> END。
*         整帧 640x480 的每个像素都被写到（天空底图与 HUD 底色各铺一半），所以整帧 CRC 与上一帧无关。
*         完全落在裁剪框外的精灵不发出。
*
*         活动元素（K1）只数游戏区里发出去的精灵，按敌弹、自机弹、敌机（含 Boss）、粒子、自机（含判定点）五类；
*         背景、特效、标题图、CLEAR 字样、闪白与 HUD 都不算。另按方程记下成功追加的 FILL 与 BLIT 条数，
*         判据入口拿普查从命令表另数一遍来对（2.6 节「游戏的判据入口」）。
*
*         HUD（x = 400~639，布局表见 2.6 节）：底色 FILL、左边 2 像素青色分隔线、顶部标题图（ALPHA），
*         标签用青色字库、数值用白色字库，大号数字用 digits.png；Boss 符卡进行中在段名下画血条。
*         数值取本帧组帧时已经算好的量，所以同一状态画出同一个 HUD；
*         实时量（SW FPS、HW FPS）由调用方给，无效时显示 --。
*
*         表面槽由入口在 stg_draw_cfg 里给，本文件不认识槽号，也不含 assets.h。
******************************************************************************/
#ifndef STG_DRAW_H
#define STG_DRAW_H

#include <stdint.h>

#include "gfx/gfx.h"

#include "stg_field.h"
#include "stg_game.h"
#include "stg_text.h"

/* 背景与 HUD 的颜色（2.5、2.6 节，效果图的值；2026-10-25 审美升级改新色板） */
#define STG_HUD_BG          0x00050912u
#define STG_HUD_LINE        0x0035F2FFu     /* 左边 2 像素分隔线 */
#define STG_HUD_RULE        0x00224A5Cu     /* 段名上面的细线 */
#define STG_FLASH_ARGB      0x00FFFFFFu     /* 受击闪白（残机还在时的自动炸弹不闪） */
#define STG_FLASH_RED       0x00FF3030u     /* 扣残机那一帧的红闪 */
#define STG_FLASH_C         128u
#define STG_BG_BRIGHT       0x00144058u     /* 网格亮线（在天空底图上） */
#define STG_BG_DIM          0x000A2438u     /* 网格暗线 */
#define STG_HP_HI           0x008DFF6Au     /* Boss 血条：>1/2 绿 */
#define STG_HP_MID          0x00FFB23Eu     /* >1/4 琥珀，其余品红 */
#define STG_HP_LOW          0x00FF4FD8u
#define STG_HP_FRAME        0x00182848u     /* 血条外框 */

/* 背景的命令形状（test_stg_view.c 的 V2 按这些常量断言）：
   前 STG_BG_SKY_BLITS 笔是天空列切片 BLIT(COPY)，接着 STG_BG_FILLS 条 FILL（15 横线 + 13 竖线），再是剪影带与星点 BLIT。 */
#define STG_BG_FILLS        (STG_BG_HLINES + STG_BG_VLINES)
#define STG_BG_SLICE_W      256             /* HW/TEX 条带流水线的源宽上限：w x 16 x 4 <= 16KiB */
#define STG_BG_SKY_SLICE_W  200             /* 天空底图的列切片宽（两笔铺满 400） */
#define STG_BG_SKY_BLITS    2u              /* 天空底图的 BLIT 笔数（V2 断言用） */
#define STG_BG_BAND_W       512             /* bg_layers.png 里两条剪影带的宽（stride = 512） */
#define STG_BG_FAR_H        56              /* 远景带高，sheet y = 480 */
#define STG_BG_CITY_H       72              /* 城市带高，sheet y = 536 */
#define STG_BG_FAR_Y        (STG_FIELD_H - STG_BG_FAR_H - STG_BG_CITY_H + 16)
#define STG_BG_CITY_Y       (STG_FIELD_H - STG_BG_CITY_H)
#define STG_BG_FAR_SPEED    24u             /* Q.8 像素/帧，横向 */
#define STG_BG_CITY_SPEED   80u

/* 精灵的尺寸（ships.png、boss.png、icons.png、stars.png、title.png、digits.png 的格子） */
#define STG_SHIP_PX         32
#define STG_SHIP_COLS       4u
#define STG_BOSS_PX         96
#define STG_BOSS_COLS       2u
#define STG_BOSS_FRAMES     6u              /* 每 8 帧换一帧，6 帧转过 60 度 */
#define STG_ICON_PX         16
#define STG_ICON_PLAYER     0u
#define STG_ICON_BOMB       1u
#define STG_ICON_HITBOX     2u
#define STG_STAR_PX         2
#define STG_TITLE_W         240
#define STG_TITLE_H         90
#define STG_DIGIT_W         16
#define STG_DIGIT_H         32
#define STG_DIGIT_DASH      10u             /* digits.png 第 10 格是 '-' */

typedef struct {
    stg_compose_cfg pool;       /* 目的面与弹种表五张源图的槽（按池发出用）；player_* 不用 */
    int             ships_slot;
    int             boss_slot;
    int             stars_slot;
    int             title_slot;
    int             digits_slot;
    int             icons_slot;
    int             bglayers_slot;  /* 背景合图（天空+远景+城市） */
    stg_font        font;       /* 白色字库（数值） */
    stg_font        font_cyan;  /* 青色字库（标签） */
} stg_draw_cfg;

/* HUD 的实时量。valid 为 0 时显示 --（判据模式、第一个完整的一秒之前、PC 上的 HW FPS）。 */
typedef struct {
    uint32_t    sw_fps;
    int         sw_fps_valid;
    uint32_t    hw_fps;
    int         hw_fps_valid;
    const char *seg_text;       /* 不为 NULL 时代替段名那一行（SW8 模式 3 显示搜索的档与结果） */
} stg_hud_live;

/* 一帧的计数：活动元素按类，另按方程记成功追加的 FILL 与 BLIT。
   fx_graze 是擦弹特效的火花条数（ADD），与自机弹/粒子同属 ADD 恒等式（V2）。 */
typedef struct {
    stg_active act;
    uint32_t   boss;            /* 画出的 Boss（0 或 1，也计在 act.enemy 里） */
    uint32_t   title;           /* 画出的标题图（游戏区与 HUD 各一张） */
    uint32_t   flash;           /* 画出的闪白 */
    uint32_t   fx_graze;        /* 擦弹特效（ADD 的火花） */
    uint32_t   by_eq[GFX_EQ_MAX + 1u];
    uint32_t   rejected;        /* 被构造层拒绝的追加（应为 0） */
} stg_frame_stats;

/* Render coordinates only. The simulation remains in its original 400x480
 * coordinate system; scenery fills [0,hud_x), entities translate by field_x. */
typedef struct { unsigned width;int field_x,hud_x; } stg_draw_view;
int stg_game_compose_view(const stg_game *g,gfx_list *list,const stg_draw_cfg *cfg,
                         const stg_hud_live *live,stg_frame_stats *stats,const stg_draw_view *view);

/* 画一帧（含两条 SET_CLIP 与 END）。l 已经 END 或没绑表面表时返回 -1，st 清零；否则返回 0。 */
int  stg_game_compose(const stg_game *g, gfx_list *l, const stg_draw_cfg *cfg,
                      const stg_hud_live *live, stg_frame_stats *st);
/* 背景与 HUD 的目的面由调用方给出；static_base 只跳过已缓存的两块底色。 */
int stg_game_compose_layers(const stg_game *g,gfx_list *dynamic,gfx_list *background,
                            int bg_dst,int static_base,const stg_draw_cfg *cfg,
                            const stg_hud_live *live,stg_frame_stats *st);

#endif /* STG_DRAW_H */
