/******************************************************************************
* @file nmax.h: N_max 搜索的状态机
*
* @brief  SW8 2.3 节（口径与 4.9 节的五处定法）。纯状态机：每帧喂一次
*         「这一帧经过了几场 e（不小于 1）、活动元素数、帧时间」，吐出事件；
*         不认识弹幕场、不碰板子，所以同一份代码在 PC 的模拟时钟与板上的真实周期下都能跑。
*
*         流程（Bentley-Yao 的无界搜索）：
*           倍增   从 N0 起每档翻倍，不超过 KNOB_MAX；第一次掉帧后进二分。N0 就掉帧 -> BELOW_N0；
*                  KNOB_MAX 也通过 -> 直接确认 KNOB_MAX，通过时标 CAPPED（N_max 是下限）。
*           二分   mid = lo + (hi - lo) / 2，通过抬 lo、掉帧压 hi，直到 hi - lo <= STEP_MIN。
*           确认   在 lo 上跑 T_CONFIRM；掉帧就退 STEP_MIN 再确认，最多重试 CONFIRM_RETRY 次，
*                  退到 N0 以下 -> BELOW_N0，重试用完 -> RETRY_EXHAUSTED。
*         每一档都先等涨潮：改档之后至少 SETTLE_MIN 帧（负载到达新水平要的时间），且最近 SETTLE_FRAMES 帧的窗口里
*         前后两半的平均值之差不超过 SETTLE_ABS、最大减最小不超过 SETTLE_ABS 或不超过最大值的 1%；
*         过了 SETTLE_MAX 场还不稳就照样测、标 timeout（超时可以越过 SETTLE_MIN，软件后端靠它结束）。
*         再测满时长（T_PROBE 或 T_CONFIRM 场），
*         不在第一次掉帧时提前结束。**测量期与超时按场计**，掉帧数是每帧 e - 1 之和；
*         SETTLE_MIN 与 SETTLE_FRAMES 按帧计：它们说的是负载本身怎么演化（弹一帧走一步），与一帧花了几场无关。
*
*         结果：OK 与 CAPPED 报 N_max = 确认期活动元素数的最小值，另报旋钮值与确认期帧时间的最大值、99 分位；
*         BELOW_N0、RETRY_EXHAUSTED 不报 N_max（字段为 0），只报最后一档的帧时间。
*
*         没有文件级的可变状态，状态全在调用方给的 nmax_state 里；没有浮点、没有 malloc。
*
*         注错开关（lib/test/test_nmax.c 用，见 check_sw8_stats.sh）：
*           INJECT_NMAX_BISECT_SIDE  二分时通过的一侧写反（通过压 hi、掉帧抬 lo）
*           INJECT_NMAX_NO_STEPBACK  确认失败不退步，在同一档重新确认
*           INJECT_NMAX_NO_SETTLE    不等涨潮，改档之后立刻开始测
******************************************************************************/
#ifndef GFX2D_CORE_NMAX_H
#define GFX2D_CORE_NMAX_H

#include <stdint.h>

#include "core/framehist.h"

#define NMAX_SETTLE_WIN_MAX 64u         /* SETTLE_FRAMES 的上限（窗口的存储） */

typedef struct {
    uint32_t n0;                /* 第一档（颗） */
    uint32_t step_min;          /* 二分的终止宽度与确认失败的退步（颗） */
    uint32_t t_probe;           /* 每档测量的场数 */
    uint32_t t_confirm;         /* 确认期的场数 */
    uint32_t settle_min;        /* 改档之后至少等几帧才看窗口 */
    uint32_t settle_frames;     /* 涨潮窗口的帧数，1..NMAX_SETTLE_WIN_MAX */
    uint32_t settle_abs;        /* 窗口「最大减最小」的绝对容差 */
    uint32_t settle_max;        /* 涨潮超时（场） */
    uint32_t confirm_retry;     /* 确认失败之后最多再确认几次 */
    uint32_t knob_max;          /* 旋钮上限（颗） */
} nmax_cfg;

/* 阶段 */
#define NMAX_ST_DOUBLE      0u
#define NMAX_ST_BISECT      1u
#define NMAX_ST_CONFIRM     2u
#define NMAX_ST_DONE        3u

/* 状态 */
#define NMAX_RUNNING        0u
#define NMAX_OK             1u
#define NMAX_BELOW_N0       2u
#define NMAX_CAPPED         3u
#define NMAX_RETRY_EXHAUSTED 4u

/* nmax_feed 的事件位 */
#define NMAX_EV_KNOB        1u          /* 新的一档开始：调用方把 knob 设进压力钩子 */
#define NMAX_EV_PROBE       2u          /* 一档测完：last 是它的记录 */
#define NMAX_EV_DONE        4u          /* 结束：看 status */

/* 一档测完的记录（轨迹的一行）。 */
typedef struct {
    uint32_t index;             /* 第几档，从 0 起 */
    uint32_t stage;             /* NMAX_ST_* */
    uint32_t knob;
    uint32_t settle_frames;     /* 涨潮用了几帧 */
    uint32_t settle_timeout;    /* 1：涨潮超时之后照样测的 */
    uint32_t fields;            /* 测量期的场数 */
    uint32_t frames;            /* 测量期的帧数 */
    uint32_t drops;             /* 测量期的掉帧数 */
    uint32_t act_min;           /* 测量期活动元素数的最小值 */
    uint32_t ft_max, ft_p99;    /* 测量期帧时间（周期） */
    uint32_t pass;              /* 1：没有掉帧 */
} nmax_probe_rec;

typedef struct {
    nmax_cfg       cfg;
    uint32_t       field_cyc;           /* 一场的周期数 F */
    uint32_t       stage;
    uint32_t       status;
    uint32_t       knob;                /* 当前这一档 */
    uint32_t       lo, hi;              /* lo：已通过的最高档；hi：已掉帧的最低档；0 表示还没有 */
    uint32_t       retry;               /* 确认失败的次数 */
    uint32_t       n_probe;             /* 已开始的档数 */
    /* 当前这一档 */
    uint32_t       measuring;           /* 0 涨潮，1 测量 */
    uint32_t       settle_fields, settle_frames, settle_timeout;
    uint32_t       cur_fields, cur_frames, drops, act_min;
    uint32_t       win[NMAX_SETTLE_WIN_MAX];
    uint32_t       win_n, win_pos;
    fhist          hist;
    nmax_probe_rec last;
    /* 结果 */
    uint32_t       n_max;               /* OK、CAPPED 时有效，其余为 0 */
    uint32_t       res_knob;            /* 同上 */
    uint32_t       res_ft_max, res_ft_p99;  /* 确认期（或最后一档）的帧时间 */
    uint32_t       settle_timeouts;     /* 涨潮超时的档数 */
} nmax_state;

/* B 批的取值（SW8 2.3 节）：N0 64、STEP_MIN 16、T_PROBE 120 场、T_CONFIRM 3600 场、
   SETTLE_MIN 125 帧（定寿命负载的寿命）、SETTLE_FRAMES 30、SETTLE_ABS 2、SETTLE_MAX 600 场、
   CONFIRM_RETRY 3、KNOB_MAX 16000。 */
void        nmax_cfg_default(nmax_cfg *c);

/* 开始：第一档是 N0（调用方随后把 s->knob 设进压力钩子）。
   配置不合法（任一时长或步长为 0、SETTLE_FRAMES 越界、KNOB_MAX < N0）或 field_cyc 为 0 时返回 -1。 */
int         nmax_init(nmax_state *s, const nmax_cfg *cfg, uint32_t field_cyc);

/* 喂一帧（这一帧是在 s->knob 下跑的）。e 为 0 时按 1 算。返回 NMAX_EV_* 的组合；
   结束之后再喂返回 0。 */
uint32_t    nmax_feed(nmax_state *s, uint32_t e, uint32_t active, uint32_t frame_cyc);

const char *nmax_status_name(uint32_t status);
const char *nmax_stage_name(uint32_t stage);

/* 三种日志行，写进 buf（含结尾 \0，不含换行），返回字符数：
     nmax cfg N0=.. STEP_MIN=.. T_PROBE=.. T_CONFIRM=.. SETTLE_MIN=.. SETTLE_FRAMES=.. SETTLE_ABS=.. SETTLE_MAX=.. CONFIRM_RETRY=.. KNOB_MAX=.. F=..
     nmax probe <序号> <阶段> knob <k> settle <帧>[ timeout] fields <场> frames <帧> drops <d> act_min <m> ft_max <c> ft_p99 <c> pass|fail
     nmax result <状态> n_max <n|-> knob <k|-> ft_max <c> ft_p99 <c> probes <档数> timeouts <超时档数> */
uint32_t    nmax_format_cfg(const nmax_state *s, char *buf, uint32_t cap);
uint32_t    nmax_format_probe(const nmax_probe_rec *r, char *buf, uint32_t cap);
uint32_t    nmax_format_result(const nmax_state *s, char *buf, uint32_t cap);

#endif /* GFX2D_CORE_NMAX_H */
