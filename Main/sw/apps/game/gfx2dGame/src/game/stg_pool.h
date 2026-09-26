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
*         碰撞（SW7 2.4 节）：自机判定点对敌弹见 stg_pool_collide()，自机弹对敌机见 stg_pool_hit_targets()；
*         炸弹消弹见 stg_pool_cancel()，粒子寿命见 stg_pool_age()（C 批，2.2、2.4 节）；
*         按池发出（2.6 节）见 stg_pool_draw()。这些逐颗的循环里没有除法与取模、
*         没有函数指针调用，check_sw7_game.sh 用 objdump 查板上构建。
*
*         每个池记一个弹种位图 kinds（C 批，2.2 节）：一遍更新时把存活实体的弹种或进去，追加时也或进去；
*         组帧只遍历位图里有的弹种。
*
*         注错开关（主机测试 apps/game/gfx2dGame/test/ 用，见 check_sw7_game.sh）：
*           INJECT_STG_POOL_STALL  移除之后下一个存活实体写进去了，写指针却停一步
*           INJECT_STG_POOL_SWAP   移除改成与末尾交换（不稳定）
*           INJECT_COLLIDE_LE      命中判成 d² <= (r_p + r_b)²（恰好相切也算命中）
*           INJECT_COLLIDE_NO_CULL 去掉粗筛，坐标差大时平方溢出
*           INJECT_STG_LOOP_DIV    更新循环里加一次除法（只给 objdump 那条判据当反例，结果不对）
*           INJECT_PHIT_LE         自机弹命中判成 d² <= T²（恰好相切也算命中）
*           INJECT_PHIT_LOOP_DIV   自机弹对敌机的循环里加一次除法（objdump 反例）
*           INJECT_AGE_LOOP_DIV    粒子寿命的循环里加一次除法（objdump 反例）
******************************************************************************/
#ifndef STG_POOL_H
#define STG_POOL_H

#include <stdint.h>

#include "core/fixmath.h"
#include "gfx/gfx.h"

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
    uint32_t  kinds;        /* 弹种位图：第 k 位为 1 表示池里可能有弹种 k（只多不少） */
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

/* --- 碰撞：自机弹对敌机（SW7 2.4 节，C 批） --------------------------------- */

/* 目标圆。坐标与半径都是 Q.8。 */
typedef struct {
    int32_t x, y;           /* 中心 */
    int32_t r;              /* 判定半径 */
    int32_t alive;          /* 0：不参与（空槽、符卡间隙里的 Boss） */
} stg_target;

/* 逐颗自机弹（已标记移除的跳过），按目标顺序找第一个命中的目标：
     粗筛阈值 T = r_kind[弹种] + r_t，|dx| >= T 或 |dy| >= T 跳过；d² < T² 为命中（严格小于）；
     命中：hits[t] 加一，这颗弹打移除标记，不再看后面的目标（一颗弹只打一个目标）。
   hits 由调用方清零，这里只加。返回本次命中的弹数。坐标差须在 ±2^18 以内，T 小于 64 像素。 */
uint32_t stg_pool_hit_targets(stg_pool *p, const int32_t *r_kind, const stg_target *tg,
                              uint32_t n, uint32_t *hits);

/* 炸弹消弹：给所有没标记移除、可消除（flags[5] 为 0）的实体打移除标记，
   并在 sparks 池里原位各生成一个火花（速度 (svx, svy)、帧号 = 寿命 life、弹种 spark_kind）；
   sparks 满了就不生成，那一次计进 sparks 的溢出计数。返回消掉的个数。 */
uint32_t stg_pool_cancel(stg_pool *p, stg_pool *sparks, int32_t svx, int32_t svy,
                         uint16_t life, uint16_t spark_kind);

/* 粒子寿命：帧号当剩余寿命，每遍减一，减到 0 打移除标记（下一遍压缩时移除）；已标记的跳过。 */
void     stg_pool_age(stg_pool *p);

/* --- 按池发出（SW7 2.6 节） ------------------------------------------------ */

/* 一个弹种的画图几何：图宽高（像素）与源图每行 2^cols_log2 格。 */
typedef struct {
    int32_t  w, h;
    uint32_t cols_log2;
} stg_draw_geom;

/* 从 *cursor 起扫池，把「弹种为 kind、没有标记移除、目的矩形与 clip 相交」的实体依次写进 out[0..cap)：
     目的左上角  (x >> 8) - w/2，(y >> 8) - h/2（2.2 节）
     源左上角    (sprite & (2^cols_log2 - 1)) * w，(sprite >> cols_log2) * h（2.3 节）
   写满 cap 条或扫到池尾就停，返回写了几条；*cursor 移到下一个没扫过的下标，扫完时等于存活数。
   调用方反复调用直到 *cursor == 存活数，每次把写出来的交给 gfx_blit_run()，缓冲只要 cap 条。
   clip 是闭区间（gfx_rect），与执行器的裁剪同口径；只判相交，不裁。
   w 或 h 不大于 0 时没有像素，什么也不发，*cursor 直接移到池尾。 */
uint32_t stg_pool_draw(const stg_pool *p, uint32_t kind, const stg_draw_geom *g,
                       const gfx_rect *clip, uint32_t *cursor, gfx_blit_pos *out, uint32_t cap);

uint32_t stg_pool_count(const stg_pool *p);
uint32_t stg_pool_overflow(const stg_pool *p);
uint32_t stg_pool_kinds(const stg_pool *p);

/* 取第 i 个实体的副本。成功返回 0，i 越界返回 -1（out 不动）。 */
int      stg_pool_get(const stg_pool *p, uint32_t i, stg_ent *out);

#endif /* STG_POOL_H */
