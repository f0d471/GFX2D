/******************************************************************************
* @file census.h: 按方程的普查
*
* @brief  sw 第 07 章 3.2 节。扫一遍命令表，按六个方程分别累计条数与裁剪后的面积；
*         NOP 与 SET_CLIP 记进单独的「无方程」一格。只做矩形求交，不碰像素。
*         普查与执行后端无关，软件执行器与硬件引擎跑的是同一张表，两边的按方程计数天然可比。
*
*         口径与执行器一致（对账：同一串提交上，各方程面积之和 == exec_sw_stats.pixel_total，
*         各方程条数之和加 cmd_other == exec_sw_stats.cmd）：
*           - 裁剪是目的矩形、640x480 游戏区、SET_CLIP 框、目的表面宽高四者求交；
*           - SET_CLIP 在设置时就截到游戏区以内，新框替换旧框；
*           - 裁剪框跨提交持续（spec 第 01 章），所以状态 gfx_census_state 由调用方持有，
*             与 exec_sw_state 一一对应；
*           - 零面积或全被裁掉的 FILL/BLIT 仍计一条、面积记 0；END 不计，之后的记录不读。
*
*         **普查假定命令表合法，不重跑十步检查**（spec 第 02 章规范条款 §5）：它统计的是
*         构造层已经校验过、执行者将要执行的表。只为内存安全留一道闸——遇到不认识的
*         操作码或方程号大于 5 的记录就停下、返回 -1，不拿它去索引计数数组。
*
*         同一个文件里还有模拟时钟（sw 第 07 章 3.4 节）：帧时间由代价模型给出，输入就是普查的结果，
*         PC 上的 N_max 搜索拿它代替墙钟（SW1-SW9 总览约束 5：PC 的墙钟不作性能结论）。
*
*         注错开关（主机测试 lib/test/test_census.c 用，见 check_sw8_stats.sh）：
*           INJECT_CENSUS_NO_SETCLIP  SET_CLIP 只计数、不改裁剪框
*           INJECT_CENSUS_NO_SURF     求交漏掉目的表面宽高那一项
*           INJECT_COST_NO_B          代价漏掉「每条命令」那一项
******************************************************************************/
#ifndef GFX2D_GFX_CENSUS_H
#define GFX2D_GFX_CENSUS_H

#include "gfx/gfx.h"

#define GFX_CENSUS_EQ_N     6u      /* COPY ALPHA ADD CONST MUL ALPHA_STRAIGHT */

/* 跨提交的状态：只有裁剪框。 */
typedef struct {
    gfx_rect clip;
} gfx_census_state;

/* 累加的计数，gfx_census_run() 只加不清。 */
typedef struct {
    uint32_t cmd_by_eq[GFX_CENSUS_EQ_N];
    uint32_t pix_by_eq[GFX_CENSUS_EQ_N];
    uint32_t cmd_other;                 /* NOP 与 SET_CLIP */
} gfx_census;

/* 裁剪框回到整个游戏区，与 exec_sw_reset() 同义。 */
void     gfx_census_reset(gfx_census_state *cs);

void     gfx_census_clear(gfx_census *c);

/* 普查一张表：最多读 list_count 条，遇到 END 结束。计数累加进 c。
   正常返回 0；遇到不认识的操作码或方程号越界时停在那一条、返回 -1（已计的保留）。 */
int      gfx_census_run(gfx_census_state *cs, const gfx_surface_table *t,
                        const gfx_cmd *list, uint32_t list_count, gfx_census *c);

int gfx_census_run_instances(gfx_census_state *cs,const gfx_surface_table *t,
                              const gfx_cmd *list,uint32_t count,
                              const gfx_instance_view *views,uint32_t view_count,gfx_census *c);

/* 两个合计：与执行器的 cmd、pixel_total 对账用。 */
uint32_t gfx_census_cmd_total(const gfx_census *c);
uint32_t gfx_census_pix_total(const gfx_census *c);

/* --- 模拟时钟 ------------------------------------------------------------ */

/* 代价模型：一帧的周期数 = a + b × 命令数 + Σ c[方程] × 该方程的像素数。
   命令数是 gfx_census_cmd_total()，与执行器的 cmd 同口径（含 NOP 与 SET_CLIP）。 */
typedef struct {
    uint32_t a;                         /* 每帧固定 */
    uint32_t b;                         /* 每条命令 */
    uint32_t c[GFX_CENSUS_EQ_N];        /* 每个方程每像素 */
} gfx_cost_model;

/* 64 位累加：b 与每个 c 不超过 2^28、命令数与每方程像素数不超过 2^32 时，
   八项之和小于 2^63，不会溢出。 */
uint64_t gfx_cost_cycles(const gfx_cost_model *m, const gfx_census *c);

/* 代价严格大于一场才算掉帧，返回 1；恰好等于一场不算。 */
int      gfx_cost_drops(uint64_t cost, uint32_t field_cyc);

#endif /* GFX2D_GFX_CENSUS_H */
