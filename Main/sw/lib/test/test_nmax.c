/******************************************************************************
* @file test_nmax.c: N_max 搜索状态机（SW8 2.3 节）的主机侧测试
*
* @brief  综合负载（SW8 第 3 节「搜索状态机」一行）：活动元素数改档之后按已知斜率爬到旋钮值
*         （每帧走 ceil(|差| / 180)，一次改档全程约 180 帧，长于 T_PROBE），帧代价 = A + B × 活动元素数，
*         e = max(1, ceil(代价 / F))。解析解 N* = floor((F - A) / B)：不掉帧的最大活动元素数。
*
*           N1  多组 (A, B, F)：结果 OK，0 <= N* - N_max < STEP_MIN，N_max 等于旋钮值
*           N2  每像素（每颗）代价加倍，N_max 不增
*           N3  N0 那一档就掉帧：BELOW_N0，不报 N_max，照样报帧时间，很快结束
*           N4  确认期第 1800 场（30 秒）注入一帧尖峰：确认失败、退 STEP_MIN、重新确认后 OK，
*               结果比没有尖峰时小 STEP_MIN，确认了两次
*           N5  活动元素数一直抖动（±5%）：每档涨潮都超时、照样测，搜索结束
*           N6  KNOB_MAX 也不掉帧：CAPPED，N_max 是下限
*           N7  每帧 50 场（软件执行器那样）：BELOW_N0，十几帧之内结束
*           N8  每次确认都有尖峰：重试用完 RETRY_EXHAUSTED，不报 N_max
*           N9  不合法的配置被拒；格式化的三种行
*
*         注错开关（见 core/nmax.h 的文件头），每个都必须让这里至少一行 FAIL：
*           NMAX_BISECT_SIDE  -> N1、N2
*           NMAX_NO_STEPBACK  -> N4
*           NMAX_NO_SETTLE    -> N1（N* = 1901 那一组：倍增到 2048 时负载要 146 帧才越过 N*，不等涨潮就会误判通过）
******************************************************************************/

#include <stdio.h>
#include <string.h>

#include "core/format.h"
#include "core/nmax.h"
#include "test_nmax.h"

#define RAMP_FRAMES     180u
#define FRAME_LIMIT     2000000u

static uint32_t g_fail;

static void say(const char *s)
{
    fputs(s, stdout);
    fflush(stdout);
}

static void ck(const char *tag, int ok)
{
    char     line[200];
    core_out o;

    core_out_init(&o, line, (uint32_t)sizeof line);
    core_out_str(&o, tag);
    core_out_str(&o, ok ? " ok\n" : " FAIL\n");
    say(line);
    if (!ok)
        g_fail++;
}

/* --- 综合负载 ------------------------------------------------------------ */

typedef struct {
    uint32_t A, B, F;           /* 帧代价 = A + B × 活动元素数；一场 F 周期 */
    uint32_t noise;             /* 1：活动元素数每帧 ±5% 交替 */
    uint32_t e_fixed;           /* 非 0：每帧就是这么多场（软件执行器那样） */
    uint32_t spike_field;       /* 非 0：确认期测到第几场时注入一帧 e = 2 */
    uint32_t spike_every;       /* 1：每次确认都注入；0：只在第一次确认注入 */
} syn_model;

typedef struct {
    uint32_t frames;
    uint32_t confirms;          /* 开始过几次确认 */
    uint32_t spikes;
    uint32_t n_star;
} syn_out;

static uint32_t n_star(const syn_model *m)
{
    return (m->F >= m->A) ? (m->F - m->A) / m->B : 0u;
}

static uint32_t run_syn(const syn_model *m, nmax_state *s, syn_out *out)
{
    nmax_cfg cfg;
    uint32_t active = 0u, target, step, spiked = 0u, confirm_seen = 0u;

    nmax_cfg_default(&cfg);
    out->frames = 0u;
    out->confirms = 0u;
    out->spikes = 0u;
    out->n_star = n_star(m);
    if (nmax_init(s, &cfg, m->F) != 0)
        return 0u;

    target = s->knob;
    step = (target + RAMP_FRAMES - 1u) / RAMP_FRAMES;
    while (s->status == NMAX_RUNNING && out->frames < FRAME_LIMIT) {
        uint32_t a, e, ev;
        uint64_t cost;

        /* 负载按斜率走向旋钮值 */
        if (active < target)
            active = (target - active > step) ? active + step : target;
        else if (active > target)
            active = (active - target > step) ? active - step : target;

        a = active;
        if (m->noise)
            a = (out->frames & 1u) ? active + active / 20u : active - active / 20u;

        cost = (uint64_t)m->A + (uint64_t)m->B * a;
        e = (cost <= m->F) ? 1u : (uint32_t)((cost + m->F - 1u) / m->F);
        if (m->e_fixed)
            e = m->e_fixed;

        /* 确认期的尖峰：测到 spike_field 场的那一帧多一场 */
        if (s->stage == NMAX_ST_CONFIRM && s->measuring && m->spike_field != 0u
            && s->cur_fields >= m->spike_field && (m->spike_every || out->spikes == 0u)
            && spiked != s->n_probe) {
            spiked = s->n_probe;
            e += 1u;
            out->spikes++;
        }

        ev = nmax_feed(s, e, a, (cost > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)cost);
        out->frames++;
        if (ev & NMAX_EV_KNOB) {
            uint32_t d;

            target = s->knob;
            d = (target > active) ? target - active : active - target;
            step = (d + RAMP_FRAMES - 1u) / RAMP_FRAMES;
            if (step == 0u)
                step = 1u;
            if (s->stage == NMAX_ST_CONFIRM && confirm_seen != s->n_probe) {
                confirm_seen = s->n_probe;
                out->confirms++;
            }
        }
    }
    return s->status;
}

static void say_run(const char *tag, const syn_model *m, const nmax_state *s, const syn_out *o)
{
    char     line[220];
    core_out w;

    core_out_init(&w, line, (uint32_t)sizeof line);
    core_out_str(&w, tag);
    core_out_str(&w, " A=");
    core_out_u32(&w, m->A);
    core_out_str(&w, " B=");
    core_out_u32(&w, m->B);
    core_out_str(&w, " N*=");
    core_out_u32(&w, o->n_star);
    core_out_str(&w, " -> ");
    core_out_str(&w, nmax_status_name(s->status));
    core_out_str(&w, " n_max ");
    core_out_u32(&w, s->n_max);
    core_out_str(&w, " knob ");
    core_out_u32(&w, s->res_knob);
    core_out_str(&w, " probes ");
    core_out_u32(&w, s->n_probe);
    core_out_str(&w, " frames ");
    core_out_u32(&w, o->frames);
    core_out_str(&w, "\n");
    say(line);
}

static nmax_state s_st;

/* --- N1 解析解 ----------------------------------------------------------- */

static void n1_closed_form(void)
{
    static const syn_model ms[] = {
        { 213424u,  612u, 1666667u, 0u, 0u, 0u, 0u },   /* SW8 2.3 节的 16x16 模型 */
        { 213424u,  228u, 1666667u, 0u, 0u, 0u, 0u },   /* 8x8 */
        { 100000u,  824u, 1666667u, 0u, 0u, 0u, 0u },   /* N* = 1901：倍增到 2048 时负载 146 帧才越过 */
        {      0u, 1000u, 1666667u, 0u, 0u, 0u, 0u },
        { 500000u,  250u, 1666667u, 0u, 0u, 0u, 0u },
        { 900000u, 9000u, 1666667u, 0u, 0u, 0u, 0u },   /* N* = 85：倍增只走一步 */
    };
    uint32_t i, bad = 0u;

    for (i = 0u; i < sizeof ms / sizeof ms[0]; i++) {
        syn_out o;

        (void)run_syn(&ms[i], &s_st, &o);
        say_run("N1", &ms[i], &s_st, &o);
        if (s_st.status != NMAX_OK || s_st.n_max > o.n_star
            || o.n_star - s_st.n_max >= s_st.cfg.step_min || s_st.n_max != s_st.res_knob)
            bad++;
    }
    ck("N1 six models: OK, 0 <= N* - N_max < STEP_MIN, N_max == knob", bad == 0u);
}

/* --- N2 单调 ------------------------------------------------------------- */

static void n2_monotonic(void)
{
    uint32_t b, prev = 0xFFFFFFFFu, bad = 0u;

    for (b = 150u; b <= 4800u; b *= 2u) {
        syn_model m = { 200000u, 0u, 1666667u, 0u, 0u, 0u, 0u };
        syn_out   o;

        m.B = b;
        (void)run_syn(&m, &s_st, &o);
        say_run("N2", &m, &s_st, &o);
        if (s_st.status != NMAX_OK || s_st.n_max > prev || o.n_star - s_st.n_max >= s_st.cfg.step_min)
            bad++;
        prev = s_st.n_max;
    }
    ck("N2 B = 150..4800 doubling: N_max non-increasing, each within STEP_MIN of N*", bad == 0u);
}

/* --- N3 下界 ------------------------------------------------------------- */

static void n3_below(void)
{
    syn_model m = { 2000000u, 100u, 1666667u, 0u, 0u, 0u, 0u };
    syn_out   o;
    char      line[200];

    (void)run_syn(&m, &s_st, &o);
    say_run("N3", &m, &s_st, &o);
    (void)nmax_format_result(&s_st, line, (uint32_t)sizeof line);
    say(line);
    say("\n");
    ck("N3 cost above one field at N0: BELOW_N0 after 1 probe, n_max not reported, ft_max > F",
       s_st.status == NMAX_BELOW_N0 && s_st.n_probe == 1u && s_st.n_max == 0u && s_st.res_knob == 0u
       && s_st.res_ft_max > m.F && strstr(line, "n_max - knob -") != 0);
}

/* --- N4 尖峰 ------------------------------------------------------------- */

static void n4_spike(void)
{
    syn_model m = { 213424u, 612u, 1666667u, 0u, 0u, 0u, 0u };
    syn_out   o0, o1;
    uint32_t  base;

    (void)run_syn(&m, &s_st, &o0);
    base = s_st.n_max;
    m.spike_field = 1800u;
    (void)run_syn(&m, &s_st, &o1);
    say_run("N4", &m, &s_st, &o1);
    ck("N4 one spike at confirm field 1800: confirm fails, steps back STEP_MIN, reconfirms OK",
       s_st.status == NMAX_OK && o1.spikes == 1u && o1.confirms == 2u && o0.confirms == 1u
       && s_st.n_max + s_st.cfg.step_min == base && s_st.retry == 1u);
}

/* --- N5 抖动 ------------------------------------------------------------- */

static void n5_noise(void)
{
    syn_model m = { 213424u, 612u, 1666667u, 1u, 0u, 0u, 0u };
    syn_out   o;

    (void)run_syn(&m, &s_st, &o);
    say_run("N5", &m, &s_st, &o);
    ck("N5 active jitters +-5%: every probe settles by timeout, search still ends",
       s_st.status != NMAX_RUNNING && s_st.settle_timeouts == s_st.n_probe && s_st.n_probe > 0u);
}

/* --- N6 上限 ------------------------------------------------------------- */

static void n6_cap(void)
{
    syn_model m = { 0u, 1u, 1666667u, 0u, 0u, 0u, 0u };
    syn_out   o;

    (void)run_syn(&m, &s_st, &o);
    say_run("N6", &m, &s_st, &o);
    ck("N6 KNOB_MAX passes: CAPPED, N_max == KNOB_MAX (a lower bound)",
       s_st.status == NMAX_CAPPED && s_st.n_max == s_st.cfg.knob_max && s_st.res_knob == s_st.cfg.knob_max);
}

/* --- N7 软件执行器那样 --------------------------------------------------- */

static void n7_slow(void)
{
    syn_model m = { 0u, 1u, 1666667u, 0u, 50u, 0u, 0u };
    syn_out   o;

    (void)run_syn(&m, &s_st, &o);
    say_run("N7", &m, &s_st, &o);
    ck("N7 every frame 50 fields: BELOW_N0 within 20 frames (settle timeout 12 frames + probe 3 frames)",
       s_st.status == NMAX_BELOW_N0 && o.frames <= 20u && s_st.settle_timeouts == 1u);
}

/* --- N8 重试用完 --------------------------------------------------------- */

static void n8_exhausted(void)
{
    syn_model m = { 213424u, 612u, 1666667u, 0u, 0u, 1800u, 1u };
    syn_out   o;

    (void)run_syn(&m, &s_st, &o);
    say_run("N8", &m, &s_st, &o);
    ck("N8 spike in every confirm: RETRY_EXHAUSTED after 1 + CONFIRM_RETRY confirms, n_max not reported",
       s_st.status == NMAX_RETRY_EXHAUSTED && o.confirms == 1u + s_st.cfg.confirm_retry
       && s_st.n_max == 0u && s_st.res_knob == 0u);
}

/* --- N9 配置与格式 ------------------------------------------------------- */

static void n9_cfg(void)
{
    nmax_cfg c;
    int      bad = 0;
    char     line[240];

    nmax_cfg_default(&c);
    c.settle_frames = 0u;
    bad += (nmax_init(&s_st, &c, 1000u) == -1);
    nmax_cfg_default(&c);
    c.settle_frames = NMAX_SETTLE_WIN_MAX + 1u;
    bad += (nmax_init(&s_st, &c, 1000u) == -1);
    nmax_cfg_default(&c);
    c.knob_max = c.n0 - 1u;
    bad += (nmax_init(&s_st, &c, 1000u) == -1);
    nmax_cfg_default(&c);
    bad += (nmax_init(&s_st, &c, 0u) == -1);
    ck("N9 bad configs rejected (settle window 0 / 65, KNOB_MAX < N0, F = 0)", bad == 4);

    nmax_cfg_default(&c);
    (void)nmax_init(&s_st, &c, 1666667u);
    (void)nmax_format_cfg(&s_st, line, (uint32_t)sizeof line);
    say(line);
    say("\n");
    ck("N9 cfg line lists the ten constants and F",
       strcmp(line, "nmax cfg N0=64 STEP_MIN=16 T_PROBE=120 T_CONFIRM=3600 SETTLE_MIN=125 SETTLE_FRAMES=30"
                    " SETTLE_ABS=2 SETTLE_MAX=600 CONFIRM_RETRY=3 KNOB_MAX=16000 F=1666667") == 0);
}

uint32_t test_nmax_run(void)
{
    g_fail = 0u;
    n1_closed_form();
    n2_monotonic();
    n3_below();
    n4_spike();
    n5_noise();
    n6_cap();
    n7_slow();
    n8_exhausted();
    n9_cfg();
    return g_fail;
}
