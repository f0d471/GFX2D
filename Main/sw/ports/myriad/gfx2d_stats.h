/******************************************************************************
* @file gfx2d_stats.h: 模式框架的统计埋点（每秒一行 CSV）
*
* @brief  统计行的列口径。三种行都带前缀，与其它日志行分得开：
*           csvh,t,mode,exec,fps,drop,n_active,n_bullet,overflow,cmd,pixel,key_skip,ft_max,
*                cyc_logic,cyc_build,cyc_exec,cyc_auto,cmd_eq0..cmd_eq5,cmd_other,pix_eq0..pix_eq5
*           csv,<每 60 场一行>
*           csvsum,frames,fields,drops,ft_max,ft_p99,rows,lines_dropped
*         每 60 场（board_frame_count 的增量）一行，t 是行号。fps 是这 60 场里的呈现次数，drop = 场数增量 - 呈现次数；
*         n_active、n_bullet、cmd、pixel、key_skip、按方程的两组列与四个 cyc_* 取这一秒的抽样帧（这一秒的第一帧）：
*         软件行的执行器统计来自那一帧；硬件行的 cmd/pixel 来自同一表的抽样普查，key_skip 填 0 表示无逐帧硬件值，
*         应读追加的 hw_key_cum 累计列。普查的耗时不进帧时间。overflow 是这一秒敌弹池溢出计数的增量，
*         ft_max 是这一秒逐帧帧时间的最大值。切换模式时这一秒的窗口清零，下一行从进入新模式的那一帧重新计；
*         汇总行的帧时间直方图（F = BOARD_HZ / 60）覆盖全程。
*
*         每出一行自查：cmd_by_eq 之和加 cmd_other 等于 cmd，pix_by_eq 之和等于 pixel（执行器的 pixel_total）；
*         不符打一行 `err csv` 并记 fail。所有行进日志队列（lib/core/logq），由框架按 board_tx_room() 取出。
*
*         注错开关（check_sw8_stats.sh 用）：
*           INJECT_CSV_DROP_EQ  打印时漏掉方程 1（ALPHA）那两列（写 0）；应用自己按内部值自查，只有脚本的解析看得见
******************************************************************************/
#ifndef GFX2D_STATS_H
#define GFX2D_STATS_H

#include <stdint.h>

#include "core/framehist.h"
#include "core/logq.h"
#include "gfx/census.h"
#include "gfx/gfx.h"

#define STATS_ROW_FIELDS    60u

/* 模式名 */
#define STATS_MODE_DEMO     1u
#define STATS_MODE_GAME     2u
#define STATS_MODE_PRESS    3u
#define STATS_MODE_R4       4u

/* 抽样帧的量 */
typedef struct {
    uint32_t      n_active;
    uint32_t      n_bullet;
    exec_sw_stats xs;
    gfx_census    cc;
    uint32_t      hw_perf[9];     /* 硬件寄存器累计值，与普查像素分开 */
    int           hw_real;
    uint32_t      cyc_logic, cyc_build, cyc_exec, cyc_auto;
} stats_sample;

typedef struct {
    logq        *q;
    fhist        hist;          /* 全程的帧时间 */
    uint32_t     frames;        /* 全程帧数 */
    uint32_t     field0, swap0; /* 全程的起点 */
    uint32_t     rows;          /* 已出的 csv 行数，也是下一行的 t */
    uint32_t     fail;          /* 自查不符的次数 */
    /* 这一秒的窗口 */
    uint32_t     mode, exec_hw;
    uint32_t     win_field0, win_swap0, win_ovf0, win_ft_max;
    uint64_t     win_cpu_sum;
    uint32_t     win_frames, hw_busy_prev;
    int          have_hw_busy_prev;
    int          have_sample;
    stats_sample sample;
} stats_state;

/* 开始：全程计数清零、写表头行。 */
void stats_init(stats_state *s, logq *q, uint32_t mode, uint32_t exec_hw, uint32_t ovf_now);

/* 切模式或切后端：这一秒的窗口清零（下一行从现在起计）。 */
void stats_window(stats_state *s, uint32_t mode, uint32_t exec_hw, uint32_t ovf_now);

/* 这一帧要不要抽样（这一秒还没有抽样帧时要）。 */
int  stats_want_sample(const stats_state *s);

/* 抽样：普查一张命令表（由调用方给的表面表与记录），连同执行器统计与其它量记下。 */
void stats_sample_list(stats_state *s, const gfx_surface_table *t, const gfx_cmd *list, uint32_t used,
                       const stats_sample *other);
void stats_sample_counted(stats_state *s, const gfx_census *cc, const stats_sample *other);

/* 一帧结束：帧时间进窗口与直方图；满 60 场就出一行（ovf_now 是敌弹池溢出计数的当前值）。 */
void stats_frame(stats_state *s, uint32_t ft_cyc, uint32_t cpu_cyc,
                 uint32_t ovf_now, const uint32_t *hw_perf_end);

/* 运行结束：写汇总行。 */
void stats_end(stats_state *s);

#endif /* GFX2D_STATS_H */
