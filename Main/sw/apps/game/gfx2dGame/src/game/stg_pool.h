/******************************************************************************
* @file stg_pool.h: 实体池 —— 敌弹、自机弹、敌机、粒子
*
* @brief  SW7 2.2 节。四个池用同一种 20 字节实体，布局先用 AoS；
*         实体的字段只在 stg_pool.c 里读写，外面经函数取值，以后换 SoA 只改那一个文件。
*
*         **回收是 Existential Processing**（Fabian《Data-Oriented Design》）：
*         「存活」就是「在表里」。更新、出界判定与稳定压缩在同一遍里做完，
*         读指针从 0 走到 n，写指针只在实体存活时前进；碰撞与炸弹只打「已标记移除」，
*         下一遍压缩时移除。存活实体的相对顺序不变（绘制顺序稳定），没有空闲链表。
*
*         口径（SW7 2.2 节与 4.5 节第 3 条）：
*           - x, y 是实体**中心**，Q.8；vx, vy 每帧加一次。
*           - 画图时目的左上角取 (x >> 8) - w/2，见 stg_draw_origin()。
*           - 出界：中心落在游戏区四周外扩 STG_MARGIN_PX 之外，半开区间，见 stg_bounds_field()。
*           - 池满时拒绝，溢出计数加一（进 SW8 的 CSV）。
*
*         没有文件级的可变状态：池的描述与存储都在调用方给的结构体里（SW7 2.8 节）。
*         更新循环只做加法与比较，不调函数指针、没有除法（设计文档 4.4 节的实现约束）。
*
*         碰撞（SW7 2.4 节，B 批只有自机判定点对敌弹）见 stg_pool_collide()。
*
*         注错开关（主机测试 apps/game/gfx2dGame/test/ 用，见 check_sw7_game.sh）：
*           INJECT_STG_POOL_STALL  移除之后下一个存活实体写进去了，写指针却停一步
*           INJECT_STG_POOL_SWAP   移除改成与末尾交换（不稳定）
*           INJECT_COLLIDE_LE      命中判成 d² <= (r_p + r_b)²（恰好相切也算命中）
*           INJECT_COLLIDE_NO_CULL 去掉粗筛，坐标差大时平方溢出
******************************************************************************/
#ifndef STG_POOL_H
#define STG_POOL_H

#include <stdint.h>

#include "core/fixmath.h"

/* 容量。**存储上限，不是 60 FPS 下的 N_max**（SW7 2.2 节）。 */
#define STG_CAP_EBULLET     16384u
#define STG_CAP_PBULLET     256u
#define STG_CAP_ENEMY       64u
#define STG_CAP_PARTICLE    2048u

/* flags 的位。其余位保留为 0。 */
#define STG_F_KIND_MASK     0x000Fu     /* [3:0] 弹种：查判定半径与图像 */
#define STG_F_GRAZED        0x0010u     /* [4]   已擦弹，每颗只计一次 */
#define STG_F_NOCANCEL      0x0020u     /* [5]   不可消除（炸弹不消它） */
#define STG_F_REMOVE        0x8000u     /* [15]  已标记移除，下一遍压缩时移除 */

/* 游戏区与出界外扩。外扩取最大弹图的半宽：米弹 20x20，10 像素（SW7 2.7 节）。 */
#define STG_FIELD_W         400
#define STG_FIELD_H         480
#define STG_MARGIN_PX       10

typedef struct {
    int32_t  x, y;          /* 中心，Q.8 */
    int32_t  vx, vy;        /* 每帧的位移，Q.8 */
    uint16_t sprite;        /* 图集帧号 */
    uint16_t flags;         /* STG_F_* */
} stg_ent;

/* 与 E8 的实体同宽：四个 int32 加两个 uint16，4 字节对齐、无填充。 */
_Static_assert(sizeof(stg_ent) == 20u, "stg_ent 必须是 20 字节");

/* 出界边界，Q.8。存活条件 x_lo <= x < x_hi 且 y_lo <= y < y_hi。 */
typedef struct {
    int32_t x_lo, x_hi;
    int32_t y_lo, y_hi;
} stg_bounds;

typedef struct {
    stg_ent  *ent;          /* 存储，由调用方给 */
    uint32_t  cap;
    uint32_t  n;            /* 存活数：ent[0..n) */
    uint32_t  overflow;     /* 池满被拒的发射次数，累计 */
} stg_pool;

/* 一遍更新里移除了多少：出界的与被标记的分开数。 */
typedef struct {
    uint32_t oob;
    uint32_t marked;
} stg_step_stats;

/* 四个池连同各自的存储。由游戏的状态结构体持有，切模式时整体重新初始化。 */
typedef struct {
    stg_pool ebullet;
    stg_pool pbullet;
    stg_pool enemy;
    stg_pool particle;
    stg_ent  ebullet_mem[STG_CAP_EBULLET];
    stg_ent  pbullet_mem[STG_CAP_PBULLET];
    stg_ent  enemy_mem[STG_CAP_ENEMY];
    stg_ent  particle_mem[STG_CAP_PARTICLE];
} stg_pools;

/* 画图时的目的左上角（像素）：中心取整像素再减半个图宽。size/2 是整数除法。 */
static inline int32_t stg_draw_origin(int32_t center_q8, int32_t size_px)
{
    return fx_px(center_q8) - size_px / 2;
}

void     stg_pool_init(stg_pool *p, stg_ent *storage, uint32_t cap);
void     stg_pools_init(stg_pools *s);

/* 游戏区四周各外扩 STG_MARGIN_PX 的默认边界。 */
void     stg_bounds_field(stg_bounds *b);

/* 发射：追加到末尾。成功返回 0；池满返回 -1，溢出计数加一，池不变。
   flags 里的 STG_F_REMOVE 不收：新实体总是存活的。 */
int      stg_pool_add(stg_pool *p, int32_t x, int32_t y, int32_t vx, int32_t vy,
                      uint16_t sprite, uint16_t flags);

/* 一遍：先看已标记移除，再做两次加法，再判出界，其余稳定压缩到前面。
   st 可以传 NULL；不为 NULL 时本遍的两个计数写进去（覆盖，不累加）。 */
void     stg_pool_step(stg_pool *p, const stg_bounds *b, stg_step_stats *st);

/* 给第 i 个实体打「已标记移除」，下一遍移除。i 越界时什么也不做。 */
void     stg_pool_mark(stg_pool *p, uint32_t i);

/* --- 碰撞：自机判定点对敌弹（SW7 2.4 节） --------------------------------- */

/* 自机探针。坐标与半径都是 Q.8。 */
typedef struct {
    int32_t px, py;         /* 自机中心 */
    int32_t r_hit;          /* 自机判定半径 */
    int32_t r_graze;        /* 擦弹半径 */
    int     hit_enabled;    /* 受击开关：0 时不计命中、不打标记，擦弹照算 */
} stg_probe;

typedef struct {
    uint32_t hits;
    uint32_t grazes;
} stg_hit_stats;

/* 逐颗（2.4 节 B 批定法）：
     1. 已标记移除的跳过；
     2. 粗筛阈值 T = r_graze + r_kind[弹种]，|dx| >= T 或 |dy| >= T 就跳过；
     3. d² = dx² + dy²；受击开关开着且 d² < (r_hit + r_b)² 为命中：命中数加一、打移除标记，不再算擦弹；
     4. 否则还没擦过且 d² < T² 为擦弹：擦弹数加一、打擦弹位。
   r_kind 是 16 项的判定半径表（Q.8），下标是 flags[3:0]。坐标差须在 ±2^18 以内
   （池里的弹都按出界边界筛过，发射器与自机在游戏区内），平方只在通过粗筛之后做。
   r_hit 不得大于 r_graze：粗筛的阈值按擦弹半径取，判定半径更大时粗筛会漏掉命中。
   st 写本次的两个计数（覆盖，不累加）。 */
void     stg_pool_collide(stg_pool *p, const int32_t *r_kind, const stg_probe *pr,
                          stg_hit_stats *st);

uint32_t stg_pool_count(const stg_pool *p);
uint32_t stg_pool_overflow(const stg_pool *p);

/* 取第 i 个实体的副本。成功返回 0，i 越界返回 -1（out 不动）。 */
int      stg_pool_get(const stg_pool *p, uint32_t i, stg_ent *out);

#endif /* STG_POOL_H */
