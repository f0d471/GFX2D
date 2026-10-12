/******************************************************************************
* @file nmax.c: N_max 搜索的状态机
*
* @brief  口径见 nmax.h。每帧的工作量是涨潮窗口的一遍扫描（最多 64 项）
*         或直方图的一次累加，与活动元素数无关。
******************************************************************************/

#include "nmax.h"

#include "core/format.h"

void nmax_cfg_default(nmax_cfg *c)
{
    c->n0            = 64u;
    c->step_min      = 16u;
    c->t_probe       = 120u;
    c->t_confirm     = 3600u;
    c->settle_min    = 125u;
    c->settle_frames = 30u;
    c->settle_abs    = 2u;
    c->settle_max    = 600u;
    c->confirm_retry = 3u;
    c->knob_max      = 16000u;
}

/* 新的一档：旋钮换成 knob，重新等涨潮。 */
static uint32_t start_probe(nmax_state *s, uint32_t stage, uint32_t knob)
{
    s->stage          = stage;
    s->knob           = knob;
    s->measuring      = 0u;
    s->settle_fields  = 0u;
    s->settle_frames  = 0u;
    s->settle_timeout = 0u;
    s->win_n          = 0u;
    s->win_pos        = 0u;
    s->n_probe++;
    return NMAX_EV_KNOB;
}

static uint32_t finish(nmax_state *s, uint32_t status)
{
    s->status = status;
    s->stage  = NMAX_ST_DONE;
    if (status == NMAX_OK || status == NMAX_CAPPED) {
        s->n_max    = s->last.act_min;
        s->res_knob = s->last.knob;
    } else {
        s->n_max    = 0u;          /* 失败的路径不报像结果的数 */
        s->res_knob = 0u;
    }
    s->res_ft_max = s->last.ft_max;
    s->res_ft_p99 = s->last.ft_p99;
    return NMAX_EV_DONE;
}

int nmax_init(nmax_state *s, const nmax_cfg *cfg, uint32_t field_cyc)
{
    if (cfg->n0 == 0u || cfg->step_min == 0u || cfg->t_probe == 0u || cfg->t_confirm == 0u
        || cfg->settle_frames == 0u || cfg->settle_frames > NMAX_SETTLE_WIN_MAX
        || cfg->settle_max == 0u || cfg->knob_max < cfg->n0 || field_cyc == 0u)
        return -1;

    s->cfg             = *cfg;
    s->field_cyc       = field_cyc;
    s->status          = NMAX_RUNNING;
    s->lo              = 0u;
    s->hi              = 0u;
    s->retry           = 0u;
    s->n_probe         = 0u;
    s->n_max           = 0u;
    s->res_knob        = 0u;
    s->res_ft_max      = 0u;
    s->res_ft_p99      = 0u;
    s->settle_timeouts = 0u;
    s->last.index      = 0u;
    s->last.pass       = 0u;
    fhist_init(&s->hist, field_cyc);
    (void)start_probe(s, NMAX_ST_DOUBLE, cfg->n0);
    return 0;
}

/* 改档之后过了 SETTLE_MIN 帧、窗口满了，两条都成立才算稳定：
     不在涨潮或退潮：最早的一半与最近的一半，平均值之差不超过 SETTLE_ABS
                     （整数写成两半之和的差不超过 SETTLE_ABS × 半窗帧数）；
     不在剧烈抖动：  「最大减最小」不超过 SETTLE_ABS 或不超过最大值的 1%。
   只看幅度时，一条缓慢的线性涨落在 30 帧里变化不到 1%，会在途中就放行（已知取舍）。
   窗口是环形的，win_pos 指向最早的那一项（窗口满时）。 */
static int settled(const nmax_state *s)
{
    const uint32_t w = s->cfg.settle_frames, half = w / 2u;
    uint32_t       i, k, mn, mx;
    uint64_t       sum_old = 0u, sum_new = 0u, drift;

    if (s->win_n < w || s->settle_frames < s->cfg.settle_min)
        return 0;
    mn = s->win[0];
    mx = s->win[0];
    for (i = 1u; i < w; i++) {
        if (s->win[i] < mn)
            mn = s->win[i];
        if (s->win[i] > mx)
            mx = s->win[i];
    }
    for (k = 0u; k < half; k++) {
        uint32_t io = s->win_pos + k;
        uint32_t in = s->win_pos + (w - half) + k;

        if (io >= w)
            io -= w;
        if (in >= w)
            in -= w;
        sum_old += s->win[io];
        sum_new += s->win[in];
    }
    drift = (sum_new > sum_old) ? sum_new - sum_old : sum_old - sum_new;
    if (drift > (uint64_t)s->cfg.settle_abs * half)
        return 0;
    return (mx - mn) <= s->cfg.settle_abs || (uint64_t)(mx - mn) * 100u <= (uint64_t)mx;
}

/* 测量期结束：记录这一档，决定下一步。 */
static uint32_t probe_done(nmax_state *s)
{
    const uint32_t pass = (s->drops == 0u);
    const uint32_t step = s->cfg.step_min;
    uint32_t       ev = NMAX_EV_PROBE;

    s->last.index          = s->n_probe - 1u;
    s->last.stage          = s->stage;
    s->last.knob           = s->knob;
    s->last.settle_frames  = s->settle_frames;
    s->last.settle_timeout = s->settle_timeout;
    s->last.fields         = s->cur_fields;
    s->last.frames         = s->cur_frames;
    s->last.drops          = s->drops;
    s->last.act_min        = s->act_min;
    s->last.ft_max         = fhist_max(&s->hist);
    s->last.ft_p99         = fhist_quantile(&s->hist, 99u, 100u);
    s->last.pass           = pass;

    switch (s->stage) {
    case NMAX_ST_DOUBLE:
        if (pass) {
            s->lo = s->knob;
            if (s->knob >= s->cfg.knob_max)
                return ev | start_probe(s, NMAX_ST_CONFIRM, s->lo);
            return ev | start_probe(s, NMAX_ST_DOUBLE,
                                    (s->knob > s->cfg.knob_max / 2u) ? s->cfg.knob_max : s->knob * 2u);
        }
        if (s->lo == 0u)
            return ev | finish(s, NMAX_BELOW_N0);
        s->hi = s->knob;
        break;

    case NMAX_ST_BISECT:
#ifdef INJECT_NMAX_BISECT_SIDE
        /* 注错：通过的一侧写反 */
        if (pass)
            s->hi = s->knob;
        else
            s->lo = s->knob;
#else
        if (pass)
            s->lo = s->knob;
        else
            s->hi = s->knob;
#endif
        break;

    default: /* NMAX_ST_CONFIRM */
        if (pass)
            return ev | finish(s, (s->knob >= s->cfg.knob_max) ? NMAX_CAPPED : NMAX_OK);
        s->retry++;
        if (s->retry > s->cfg.confirm_retry)
            return ev | finish(s, NMAX_RETRY_EXHAUSTED);
#ifdef INJECT_NMAX_NO_STEPBACK
        /* 注错：不退步，在同一档重新确认 */
        return ev | start_probe(s, NMAX_ST_CONFIRM, s->knob);
#else
        if (s->knob < s->cfg.n0 + step)
            return ev | finish(s, NMAX_BELOW_N0);
        s->hi = s->knob;
        s->lo = s->knob - step;
        return ev | start_probe(s, NMAX_ST_CONFIRM, s->lo);
#endif
    }

    /* 倍增掉帧之后与二分：区间够窄就确认 lo，否则测中点 */
    if (s->hi - s->lo <= step)
        return ev | start_probe(s, NMAX_ST_CONFIRM, s->lo);
    return ev | start_probe(s, NMAX_ST_BISECT, s->lo + (s->hi - s->lo) / 2u);
}

uint32_t nmax_feed(nmax_state *s, uint32_t e, uint32_t active, uint32_t frame_cyc)
{
    uint32_t dur;

    if (s->status != NMAX_RUNNING)
        return 0u;
    if (e == 0u)
        e = 1u;

    if (!s->measuring) {
        int ok, timeout;

        s->settle_fields += e;
        s->settle_frames++;
        s->win[s->win_pos] = active;
        s->win_pos = (s->win_pos + 1u < s->cfg.settle_frames) ? s->win_pos + 1u : 0u;
        if (s->win_n < s->cfg.settle_frames)
            s->win_n++;

        ok = settled(s);
#ifdef INJECT_NMAX_NO_SETTLE
        ok = 1;                     /* 注错：不等涨潮 */
#endif
        timeout = !ok && s->settle_fields >= s->cfg.settle_max;
        if (ok || timeout) {
            s->measuring      = 1u;
            s->settle_timeout = (uint32_t)timeout;
            s->settle_timeouts += (uint32_t)timeout;
            s->cur_fields     = 0u;
            s->cur_frames     = 0u;
            s->drops          = 0u;
            s->act_min        = 0xFFFFFFFFu;
            fhist_clear(&s->hist);
        }
        return 0u;                  /* 涨潮的帧不计掉帧 */
    }

    s->cur_fields += e;
    s->cur_frames++;
    s->drops += e - 1u;
    if (active < s->act_min)
        s->act_min = active;
    fhist_add(&s->hist, frame_cyc);

    dur = (s->stage == NMAX_ST_CONFIRM) ? s->cfg.t_confirm : s->cfg.t_probe;
    if (s->cur_fields < dur)
        return 0u;
    return probe_done(s);
}

const char *nmax_status_name(uint32_t status)
{
    switch (status) {
    case NMAX_RUNNING:          return "RUNNING";
    case NMAX_OK:               return "OK";
    case NMAX_BELOW_N0:         return "BELOW_N0";
    case NMAX_CAPPED:           return "CAPPED";
    case NMAX_RETRY_EXHAUSTED:  return "RETRY_EXHAUSTED";
    default:                    return "?";
    }
}

const char *nmax_stage_name(uint32_t stage)
{
    switch (stage) {
    case NMAX_ST_DOUBLE:        return "double";
    case NMAX_ST_BISECT:        return "bisect";
    case NMAX_ST_CONFIRM:       return "confirm";
    case NMAX_ST_DONE:          return "done";
    default:                    return "?";
    }
}

static void kv(core_out *o, const char *k, uint32_t v)
{
    core_out_ch(o, ' ');
    core_out_str(o, k);
    core_out_u32(o, v);
}

uint32_t nmax_format_cfg(const nmax_state *s, char *buf, uint32_t cap)
{
    core_out o;

    core_out_init(&o, buf, cap);
    core_out_str(&o, "nmax cfg");
    kv(&o, "N0=", s->cfg.n0);
    kv(&o, "STEP_MIN=", s->cfg.step_min);
    kv(&o, "T_PROBE=", s->cfg.t_probe);
    kv(&o, "T_CONFIRM=", s->cfg.t_confirm);
    kv(&o, "SETTLE_MIN=", s->cfg.settle_min);
    kv(&o, "SETTLE_FRAMES=", s->cfg.settle_frames);
    kv(&o, "SETTLE_ABS=", s->cfg.settle_abs);
    kv(&o, "SETTLE_MAX=", s->cfg.settle_max);
    kv(&o, "CONFIRM_RETRY=", s->cfg.confirm_retry);
    kv(&o, "KNOB_MAX=", s->cfg.knob_max);
    kv(&o, "F=", s->field_cyc);
    return core_out_len(&o);
}

uint32_t nmax_format_probe(const nmax_probe_rec *r, char *buf, uint32_t cap)
{
    core_out o;

    core_out_init(&o, buf, cap);
    core_out_str(&o, "nmax probe ");
    core_out_u32(&o, r->index);
    core_out_ch(&o, ' ');
    core_out_str(&o, nmax_stage_name(r->stage));
    kv(&o, "knob ", r->knob);
    kv(&o, "settle ", r->settle_frames);
    if (r->settle_timeout)
        core_out_str(&o, " timeout");
    kv(&o, "fields ", r->fields);
    kv(&o, "frames ", r->frames);
    kv(&o, "drops ", r->drops);
    kv(&o, "act_min ", r->act_min);
    kv(&o, "ft_max ", r->ft_max);
    kv(&o, "ft_p99 ", r->ft_p99);
    core_out_str(&o, r->pass ? " pass" : " fail");
    return core_out_len(&o);
}

uint32_t nmax_format_result(const nmax_state *s, char *buf, uint32_t cap)
{
    const int has = (s->status == NMAX_OK || s->status == NMAX_CAPPED);
    core_out  o;

    core_out_init(&o, buf, cap);
    core_out_str(&o, "nmax result ");
    core_out_str(&o, nmax_status_name(s->status));
    core_out_str(&o, " n_max ");
    if (has)
        core_out_u32(&o, s->n_max);
    else
        core_out_ch(&o, '-');
    core_out_str(&o, " knob ");
    if (has)
        core_out_u32(&o, s->res_knob);
    else
        core_out_ch(&o, '-');
    kv(&o, "ft_max ", s->res_ft_max);
    kv(&o, "ft_p99 ", s->res_ft_p99);
    kv(&o, "probes ", s->n_probe);
    kv(&o, "timeouts ", s->settle_timeouts);
    return core_out_len(&o);
}
