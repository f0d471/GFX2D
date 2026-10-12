/******************************************************************************
* @file entity.h: 弹幕实体的两种数据布局与一帧更新
*
* @brief  CPU 每弹幕开销与缓存对照的被测对象，**纯算法**：不依赖任何平台，
*         板上的 `sw/apps/probe/entityBench` 与主机侧的期望值生成器编的是同一份源码。
*         这是「板上与主机侧逐字段对账」判据成立的前提——只有在两边真的
*         跑同一个循环时才说明问题。
*
*         一个弹幕的字段与口径：
*
*           x, y     int32，Q12.4 定点（1 像素 = 16）
*           vx, vy   int32，Q12.4 定点
*           sprite   uint16，图集索引
*           flags    uint16，存活、计时等
*           命令     32 字节，一条 BLIT 记录
*
*         两种布局：
*           AoS  结构体数组，一个实体 20 字节连续（4 字节对齐、无填充）
*           SoA  并列数组，六个数组各自连续
*
*         更新循环只做加法，不调函数指针、不查表：设计文档 4.4 节的实现约束。
*         出屏回收用「位置落在 ±2 倍游戏区之外则标记死、按固定序列补入一个实体」，
*         不做实体池整理。补入序列由 `eb_rng` 决定，
*         同一份源码在板上与主机侧给出同一串数。
*
*         命令记录遵守 BLIT ABI；独立的构造层对照测试负责验证字节一致性。
*         这里不调用构造层，是因为要量的是「填一条 32 字节记录」本身的成本，
*         而构造层每次追加都会走一遍 `gfx_cmd_check()`。
*         板上与主机侧摘要只能验证本实现跨平台一致，不能替代 ABI 对照。
******************************************************************************/
#ifndef GFX2D_CORE_ENTITY_H
#define GFX2D_CORE_ENTITY_H

#include <stdint.h>

/* 游戏区（本层不能依赖 gfx；独立 ABI 测试断言与 gfx_abi.h 一致）。 */
#define EB_SCREEN_W     640
#define EB_SCREEN_H     480

/* Q12.4：1 像素 = 16。 */
#define EB_SHIFT        4
#define EB_ONE          (1 << EB_SHIFT)

/* 出屏边界（像素）：位置落在游戏区两侧各 2 倍宽度之外算死。 */
#define EB_DEAD_X_LO    (-2 * EB_SCREEN_W * EB_ONE)
#define EB_DEAD_X_HI    ( 3 * EB_SCREEN_W * EB_ONE)
#define EB_DEAD_Y_LO    (-2 * EB_SCREEN_H * EB_ONE)
#define EB_DEAD_Y_HI    ( 3 * EB_SCREEN_H * EB_ONE)

/* 补入点的起点与每帧的初始速度范围，Q12.4。 */
#define EB_SPAWN_X      (EB_SCREEN_W / 2 * EB_ONE)
#define EB_SPAWN_Y      (EB_SCREEN_H / 2 * EB_ONE)
#define EB_V_RANGE      (5 * EB_ONE)        /* 每帧 -5.0 ~ +5.0 像素/帧 */

/* 一个命令记录 8 个字、32 字节（gfx_abi.h 的 gfx_cmd）。 */
#define EB_CMD_WORDS    8u

/* 表面槽位与图集索引：取值固定，只为让命令记录的内容确定。 */
#define EB_SLOT_DST     0u
#define EB_SLOT_SRC     1u

/* --- AoS ----------------------------------------------------------------- */

typedef struct {
    int32_t  x, y, vx, vy;      /* Q12.4 */
    uint16_t sprite;
    uint16_t flags;
} eb_ent;

/* 四个 int32 + 两个 uint16 = 20 字节；20 已是 4 的倍数。 */
#define EB_ENT_BYTES    20u
_Static_assert(sizeof(eb_ent) == EB_ENT_BYTES, "AoS 实体布局必须为 20 字节");

/* --- SoA ----------------------------------------------------------------- */

/* SoA 的六个并列数组由调用方一次性分配成连续内存，按下面的顺序排：
     x[n] y[n] vx[n] vy[n] sprite[n] flags[n]
   这样一次分配就够，且各数组的首地址可由 n 算出。
   不把六个指针放进一个结构体，是因为「布局」本身也是被测的一部分：
   多一层指针间接对 4KB 直连缓存是额外的开销。 */
#define EB_SOA_BYTES(n)  ((n) * (4u * 4u + 2u + 2u))

/* 把一块至少 4 字节对齐的 SoA 内存切成六个数组。n 必须与分配时一致。 */
void eb_soa_split(void *mem, uint32_t n,
                  int32_t **x, int32_t **y, int32_t **vx, int32_t **vy,
                  uint16_t **sprite, uint16_t **flags);

/* --- 确定性的随机序列 ----------------------------------------------------- */

/* xorshift32。板上与主机侧给出同一串数（同一份源码、同一初始种子）。 */
uint32_t eb_rng(uint32_t *state);
void     eb_seed(eb_ent *e, uint32_t *rng);
void     eb_seed_soa(uint32_t i, int32_t *x, int32_t *y, int32_t *vx, int32_t *vy,
                     uint16_t *sprite, uint16_t *flags, uint32_t *rng);

/* --- 一帧 ---------------------------------------------------------------- */

/* 补入次数由调用方传进来的计数器累计（`respawn` 可以为 NULL）。
   不靠返回值：两条信息（补入了几次、提交了几条）都要能对上，
   而且 SoA 的提交路径里「死了几个」与「提交了几条」不是同一个量，
   用同一个计数器统一累计，板上与主机侧才不会各算一套。 */

/* 纯更新：x += vx; y += vy; 出屏则标记死并按序列补入一个。
   只碰 x/y/vx/vy/sprite/flags 六项，不写命令表。 */
void eb_frame_aos(eb_ent *e, uint32_t n, uint32_t *rng, uint32_t *respawn);

/* 更新并提交：纯更新之后，把当帧每个活动实体的位置写成一条 BLIT 记录。
   返回写进 cmd 的场景命令数（不含 END）；cmd 至少要有 n 条空间。
   要送给执行器/硬件时，调用方须另补 END 并按 ABI 验证整表。 */
uint32_t eb_frame_aos_cmd(eb_ent *e, uint32_t n, uint32_t *rng, uint32_t *cmd,
                          uint32_t *respawn);

/* SoA 版本。六个数组由 eb_soa_split 给出。 */
void eb_frame_soa(int32_t *x, int32_t *y, int32_t *vx, int32_t *vy,
                  uint16_t *sprite, uint16_t *flags,
                  uint32_t n, uint32_t *rng, uint32_t *respawn);

uint32_t eb_frame_soa_cmd(int32_t *x, int32_t *y, int32_t *vx, int32_t *vy,
                          uint16_t *sprite, uint16_t *flags,
                          uint32_t n, uint32_t *rng, uint32_t *cmd,
                          uint32_t *respawn);

/* --- 计时外的摘要 --------------------------------------------------------- */

/* 实体六个字段的逐字节 CRC32（AoS）。**由独立的编译单元实现**，
   这样编译器不能因为「没人读」而删掉更新循环里的写入。 */
uint32_t eb_digest_aos(const eb_ent *e, uint32_t n);

/* SoA 按实体顺序、按 x/y/vx/vy/sprite/flags 顺序算 CRC32，
   与 AoS 的逻辑字段摘要相同，A/B 同输入时可以直接相互对照。 */
uint32_t eb_digest_soa(const int32_t *x, const int32_t *y,
                       const int32_t *vx, const int32_t *vy,
                       const uint16_t *sprite, const uint16_t *flags, uint32_t n);

/* 命令表前 words 个字的逐字节 CRC32；是否包含 END 由调用方决定。 */
uint32_t eb_digest_cmd(const uint32_t *cmd, uint32_t words);

#endif /* GFX2D_CORE_ENTITY_H */
