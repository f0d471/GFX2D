/******************************************************************************
* @file stg_slots.h: 游戏的表面槽（sw 第 06 章 3.12 节）
*
* @brief  游戏、压力测试与 B 批的 STG_FIELD 入口共用这一套：帧缓冲 0、1，SW5 字库留在 5
*         （与 SW6 同一个槽，SW8「字库固定一个槽」），其余 12 张霓虹素材占 2~4、6~14，槽 15 空着。
*         基础 Demo 用 sw6_cfg.h 那一套。只有入口认识槽号，src/game/ 下的代码只认弹种表里的源图编号。
******************************************************************************/
#ifndef STG_SLOTS_H
#define STG_SLOTS_H

#define STG_SLOT_FB0        0
#define STG_SLOT_FB1        1
#define STG_SLOT_BUL16      2       /* bul16.png   16x16 圆弹，4 色 */
#define STG_SLOT_BUL8       3       /* bul8.png    8x8 小圆弹，4 色 */
#define STG_SLOT_RICE       4       /* rice20.png  20x20 米弹，4 色 x 16 方向 */
#define STG_SLOT_FONT       5       /* SW5 的 FONT8X16（白色） */
#define STG_SLOT_PSHOT      6       /* pshot.png   自机弹 */
#define STG_SLOT_SPARK      7       /* spark.png   火花 */
#define STG_SLOT_SHIPS      8       /* ships.png   自机、DART、CROSS */
#define STG_SLOT_BOSS       9       /* boss.png    Boss 12 帧 */
#define STG_SLOT_STARS      10      /* stars.png   星点 */
#define STG_SLOT_TITLE      11      /* title.png   标题 */
#define STG_SLOT_DIGITS     12      /* digits.png  大号数字 */
#define STG_SLOT_ICONS      13      /* icons.png   图标 */
#define STG_SLOT_FONT_CYAN  14      /* font_cyan.png 青色字库 */
#define STG_SLOT_N_ASSETS   13      /* 登记的素材张数（2~14） */

#endif /* STG_SLOTS_H */
