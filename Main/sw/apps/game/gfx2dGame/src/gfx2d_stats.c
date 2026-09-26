/******************************************************************************
* @file gfx2d_stats.c: 模式框架的统计埋点（每秒一行 CSV）
*
* @brief  口径见 gfx2d_stats.h 与 SW8 2.4 节。计数的增量都先按 uint32 相减（场号、切换次数、周期数都会回绕）。
******************************************************************************/

#include "gfx2d_stats.h"

#include "board.h"

#include "core/format.h"

static const char *const s_mode_name[4] = { "?", "demo", "game", "press" };

static void put(stats_state *s, const char *line)
{
    (void)logq_put(s->q, line);
}

void stats_init(stats_state *s, logq *q, uint32_t mode, uint32_t exec_hw, uint32_t ovf_now)
{
    s->q      = q;
    s->frames = 0u;
    s->rows   = 0u;
    s->fail   = 0u;
    s->field0 = board_frame_count();
    s->swap0  = board_swap_count();
    fhist_init(&s->hist, BOARD_HZ / 60u);
    put(s, "csvh,t,mode,exec,fps,drop,n_active,n_bullet,overflow,cmd,pixel,key_skip,ft_max,"
           "cyc_logic,cyc_build,cyc_exec,cyc_auto,cmd_eq0,cmd_eq1,cmd_eq2,cmd_eq3,cmd_eq4,cmd_eq5,"
           "cmd_other,pix_eq0,pix_eq1,pix_eq2,pix_eq3,pix_eq4,pix_eq5\r\n");
    stats_window(s, mode, exec_hw, ovf_now);
}

void stats_window(stats_state *s, uint32_t mode, uint32_t exec_hw, uint32_t ovf_now)
{
    s->mode        = (mode <= 3u) ? mode : 0u;
    s->exec_hw     = exec_hw;
    s->win_field0  = board_frame_count();
    s->win_swap0   = board_swap_count();
    s->win_ovf0    = ovf_now;
    s->win_ft_max  = 0u;
    s->have_sample = 0;
}

int stats_want_sample(const stats_state *s)
{
    return !s->have_sample;
}

void stats_sample_list(stats_state *s, const gfx_surface_table *t, const gfx_cmd *list, uint32_t used,
                       const stats_sample *other)
{
    gfx_census_state cs;

    s->sample = *other;
    gfx_census_reset(&cs);
    gfx_census_clear(&s->sample.cc);
    if (gfx_census_run(&cs, t, list, used, &s->sample.cc) != 0)
        s->fail++;
    s->have_sample = 1;
}

static void c_u32(core_out *o, uint32_t v)
{
    core_out_ch(o, ',');
    core_out_u32(o, v);
}

static void row(stats_state *s, uint32_t fields, uint32_t swaps, uint32_t ovf)
{
    const stats_sample *p = &s->sample;
    char                buf[400];
    core_out            o;
    uint32_t            e, sum_cmd, sum_pix;

    /* 自查：按内部值，不按打印出来的 */
    sum_cmd = p->cc.cmd_other;
    sum_pix = 0u;
    for (e = 0u; e < GFX_CENSUS_EQ_N; e++) {
        sum_cmd += p->cc.cmd_by_eq[e];
        sum_pix += p->cc.pix_by_eq[e];
    }
    if (s->have_sample && (sum_cmd != p->xs.cmd || sum_pix != p->xs.pixel_total)) {
        core_out_init(&o, buf, (uint32_t)sizeof buf);
        core_out_str(&o, "err csv t ");
        core_out_u32(&o, s->rows);
        core_out_str(&o, " cmd ");
        core_out_u32(&o, sum_cmd);
        core_out_ch(&o, '/');
        core_out_u32(&o, p->xs.cmd);
        core_out_str(&o, " pix ");
        core_out_u32(&o, sum_pix);
        core_out_ch(&o, '/');
        core_out_u32(&o, p->xs.pixel_total);
        core_out_str(&o, "\r\n");
        put(s, buf);
        s->fail++;
    }

    core_out_init(&o, buf, (uint32_t)sizeof buf);
    core_out_str(&o, "csv,");
    core_out_u32(&o, s->rows);
    core_out_ch(&o, ',');
    core_out_str(&o, s_mode_name[s->mode]);
    core_out_str(&o, s->exec_hw ? ",hw" : ",sw");
    c_u32(&o, swaps);
    c_u32(&o, (fields > swaps) ? fields - swaps : 0u);     /* 不等场（PC 的 GFX2D_UNPACED）时呈现可以多于场 */
    c_u32(&o, p->n_active);
    c_u32(&o, p->n_bullet);
    c_u32(&o, ovf);
    c_u32(&o, p->xs.cmd);
    c_u32(&o, p->xs.pixel_total);
    c_u32(&o, p->xs.key_skip);
    c_u32(&o, s->win_ft_max);
    c_u32(&o, p->cyc_logic);
    c_u32(&o, p->cyc_build);
    c_u32(&o, p->cyc_exec);
    c_u32(&o, p->cyc_auto);
    for (e = 0u; e < GFX_CENSUS_EQ_N; e++) {
#ifdef INJECT_CSV_DROP_EQ
        c_u32(&o, (e == GFX_EQ_ALPHA) ? 0u : p->cc.cmd_by_eq[e]);     /* 注错：漏掉 ALPHA 那一列 */
#else
        c_u32(&o, p->cc.cmd_by_eq[e]);
#endif
    }
    c_u32(&o, p->cc.cmd_other);
    for (e = 0u; e < GFX_CENSUS_EQ_N; e++) {
#ifdef INJECT_CSV_DROP_EQ
        c_u32(&o, (e == GFX_EQ_ALPHA) ? 0u : p->cc.pix_by_eq[e]);
#else
        c_u32(&o, p->cc.pix_by_eq[e]);
#endif
    }
    core_out_str(&o, "\r\n");
    put(s, buf);
    s->rows++;
}

void stats_frame(stats_state *s, uint32_t ft_cyc, uint32_t ovf_now)
{
    const uint32_t fields = board_frame_count() - s->win_field0;

    s->frames++;
    fhist_add(&s->hist, ft_cyc);
    if (ft_cyc > s->win_ft_max)
        s->win_ft_max = ft_cyc;
    if (fields >= STATS_ROW_FIELDS) {
        row(s, fields, board_swap_count() - s->win_swap0, ovf_now - s->win_ovf0);
        stats_window(s, s->mode, s->exec_hw, ovf_now);
    }
}

void stats_end(stats_state *s)
{
    const uint32_t fields = board_frame_count() - s->field0;
    const uint32_t swaps  = board_swap_count() - s->swap0;
    char           buf[160];
    core_out       o;

    core_out_init(&o, buf, (uint32_t)sizeof buf);
    core_out_str(&o, "csvsum");
    c_u32(&o, s->frames);
    c_u32(&o, fields);
    c_u32(&o, (fields > swaps) ? fields - swaps : 0u);
    c_u32(&o, fhist_max(&s->hist));
    c_u32(&o, fhist_quantile(&s->hist, 99u, 100u));
    c_u32(&o, s->rows);
    c_u32(&o, logq_dropped(s->q));
    core_out_str(&o, "\r\n");
    put(s, buf);
}
