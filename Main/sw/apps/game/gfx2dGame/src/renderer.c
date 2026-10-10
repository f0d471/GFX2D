/******************************************************************************
* @file renderer.c: v1 直通与 v2 条带的软硬件执行入口
******************************************************************************/

#include "renderer.h"
#include "board.h"
#include "stg_main.h"
#include "core/format.h"
#include "gfx_engine.h"
#ifndef GFX_FRONTEND_PLAN
#define GFX_FRONTEND_PLAN 0
#endif
#ifndef GFX_FRONTEND_TEMPLATE
#define GFX_FRONTEND_TEMPLATE 0
#endif
#ifndef GFX_FRONTEND_BACKGROUND
#define GFX_FRONTEND_BACKGROUND 0
#endif
#ifndef GFX_FRONTEND_INST
#define GFX_FRONTEND_INST 0
#endif
static gfx_instance_arena s_instances[2];
static gfx_lease s_instance_leases[2];
static void instance_release(gfx_instance_arena *a,uint32_t generation)
{
    uint32_t i;
    for(i=0;i<2;i++)if(a==&s_instances[i] && s_instance_leases[i].owner==generation)
        s_instance_leases[i].owner=0;
}
static int instance_owned(const gfx_list *l)
{
    uint32_t i;
    for(i=0;i<2;i++)if(l->instances==&s_instances[i])
        return s_instance_leases[i].owner==l->instance_generation && l->instances->generation==l->instance_generation;
    return 1; /* 显式外部 arena 的 lease 由调用方持有。 */
}
static gfx_strip_workspace s_workspace[2];
static gfx_geometry_cache s_geometry[2];
static gfx_surface_table s_build_tab[2];

int renderer_list_begin(int hw, gfx_list *list, gfx_cmd *buf, uint32_t cap,
                         const gfx_surface_table *tab, int dst)
{
    if (hw == 3) {
        const gfx_strip_config cfg = {480u >> GFX2D_STRIP_LOG2,
                                      GFX2D_STRIP_LOG2, GFX_FRONTEND_BACKGROUND?15u:(uint32_t)dst, (uint32_t)dst};
        const uint32_t idx=buf==(gfx_cmd *)board_region(REGION_CMDLIST_B).ptr?1u:0u;
        int rc;
        if(GFX_FRONTEND_BACKGROUND) {
            const gfx_surface bg={g_mem_regions[REGION_OFFSCREEN].offset+idx*0x200000u,640,480,12,0,0};
            s_build_tab[idx]=*tab;
            if(gfx_surface_set(&s_build_tab[idx],15,&bg))return GFX_BUILD_BAD_ARG;
            tab=&s_build_tab[idx];
        }
        if (cap > UINT32_MAX / sizeof *buf)
            return GFX_BUILD_BAD_ARG;
        if(GFX_FRONTEND_PLAN) {
            board_mem shared=board_region(REGION_SHARED);
            uint8_t *p=(uint8_t *)shared.ptr+0x40000u+idx*0x1c0000u;
            s_workspace[idx]=(gfx_strip_workspace){.items=(gfx_strip_plan_item *)(void *)p,.item_cap=32768u,
                .refs=(uint32_t *)(void *)(p+0x180000u),.ref_cap=30720u};
            rc=gfx_list_begin_strips_planned(list,buf,cap*32u,tab,&cfg,NULL,&s_workspace[idx]);
        } else rc=gfx_list_begin_strips(list,buf,cap*32u,tab,&cfg,NULL);
        if(!rc && GFX_FRONTEND_TEMPLATE)rc=gfx_list_geometry(list,&s_geometry[idx]);
        return rc;
    }
    gfx_list_begin(list, buf, cap, tab);
    if(GFX_FRONTEND_INST && hw) {
        int capable=1;
#ifdef __riscv
        uint32_t caps=0;
        capable=gfx_engine_caps_read(&caps)==0 && (caps&GFX_CAPS_INST_M);
#endif
        if(capable) {
            uint32_t idx=buf==(gfx_cmd *)board_region(REGION_CMDLIST_B).ptr?1u:0u;
            board_mem shared=board_region(REGION_SHARED);
            uint32_t offset=0x400000u+idx*0x80000u,generation=s_instances[idx].generation+1u;
            if(s_instance_leases[idx].owner)return GFX_BUILD_FULL;
            if(!generation)generation++;
            s_instance_leases[idx]=(gfx_lease){g_mem_regions[REGION_SHARED].offset+offset,0x80000u,generation,generation};
            s_instances[idx]=(gfx_instance_arena){(uint8_t *)shared.ptr+offset,
                g_mem_regions[REGION_SHARED].offset+offset,0x80000u,0u,generation};
            list->instances=&s_instances[idx];
            list->instance_generation=generation;
        }
    }
    return GFX_OK;
}

int renderer_list_census(const gfx_surface_table *tab, const gfx_list *list,
                          gfx_census *census)
{
    gfx_census_state cs;
    uint32_t s, total = 0u;
    gfx_census_reset(&cs);
    gfx_census_clear(census);
#if GFX_FRONTEND_BACKGROUND || defined(FRONTEND_VERIFY)
    if(list->frame) {
        uint32_t i,e;
        for(i=0;i<list->frame->count;i++) {
            gfx_census one;const gfx_pass *p=&list->frame->pass[i];
            if(p->draw->frame||renderer_list_census(&p->surfaces,p->draw,&one))return -1;
            for(e=0;e<GFX_CENSUS_EQ_N;e++) {
                census->cmd_by_eq[e]+=one.cmd_by_eq[e];census->pix_by_eq[e]+=one.pix_by_eq[e];
            }
            census->cmd_other+=one.cmd_other;
        }
        return 0;
    }
#endif
    if(list->instances) {
        const gfx_instance_arena *a=list->instances;
        const gfx_instance_view view={a->host,a->dev_base,a->used};
        return gfx_census_run_instances(&cs,tab,list->buf,list->used,&view,1u,census);
    }
    if (list->strip_table == NULL)
        return gfx_census_run(&cs, tab, list->buf, list->used, census);
    for (s = 0u; s < list->strips.lstrips; s++) {
        const uint32_t n = ((const uint32_t *)(const void *)list->strip_table)[s];
        const gfx_cmd *bucket = (const gfx_cmd *)(const void *)(list->strip_table +
                                (s + 1u) * GFX_STRIP_REGION_BYTES);
        if (n > GFX_STRIP_CAP || gfx_census_run(&cs, tab, bucket, n, census) != 0)
            return -1;
        total += n;
    }
    return total == list->used ? 0 : -1;
}

#ifdef __riscv
#include "gfx_engine.h"
#include "gfx_regs.h"
static int s_pending;
static gfx_instance_arena *s_pending_instance;
static uint32_t s_pending_generation;
static int s_pending_dst;
static uint32_t s_pending_event, s_pending_start;
static uint32_t s_perf_cmd, s_perf_pix, s_perf_key;
#if SW9_PERF
static gfx_frame_plan *s_pending_plan;
static uint32_t s_pending_ticket,s_pending_band_start,s_pending_perf[9];
static exec_sw_stats s_pending_pre;
static int s_pipeline_front=-1;
#endif

static int submit_list(const gfx_list *list)
{
    if (list->first_error || list->rejected || list->overflow || !instance_owned(list))
        return GFX_ENG_E_ARG;
    if(!list->ended || (list->planned&&!list->planned->sealed))return GFX_ENG_E_ARG;
    if (list->strip_table != NULL)
        return gfx_engine_submit_strips((uint32_t)(uintptr_t)list->strip_table,
                                        list->strips.lstrips, list->strips.strip_log2,
                                        list->strips.bg_id, list->strips.dst_id);
    return gfx_engine_submit((uint32_t)(uintptr_t)list->buf, list->used);
}

/* 引擎一次只执行一张表，相邻两次完成之间的 PERF 增量就是那一张表的计数，
   所以每次完成都要取一次，丢掉一次下一张就会算成两张。
   PERF_PIXEL 等于软件的 pixel_total - src_oob，扣掉色键跳过才与 pixel_written 同口径。 */
static void perf_take(exec_sw_stats *st)
{
    uint32_t v[GFX_PERF_COUNT];

    if (st != NULL)
        exec_sw_stats_clear(st);
    if (gfx_engine_perf_read(v, GFX_PERF_COUNT) != GFX_ENG_OK)
        return;
    if (st != NULL) {
        st->cmd           = v[GFX_PERF_CMD] - s_perf_cmd;
        st->pixel_total   = v[GFX_PERF_PIXEL] - s_perf_pix;
        st->key_skip      = v[GFX_PERF_KEY_SKIP] - s_perf_key;
        st->pixel_written = st->pixel_total - st->key_skip;
    }
    s_perf_cmd = v[GFX_PERF_CMD];
    s_perf_pix = v[GFX_PERF_PIXEL];
    s_perf_key = v[GFX_PERF_KEY_SKIP];
}

static void engine_error(int code, uint32_t index, uint32_t tag)
{
    char buf[96];
    core_out out;
    core_out_init(&out, buf, (uint32_t)sizeof buf);
    core_out_str(&out, "engine err code ");
    core_out_u32(&out, (uint32_t)code);
    core_out_str(&out, " index ");
    core_out_u32(&out, index);
    core_out_str(&out, " tag ");
    core_out_u32(&out, tag);
    core_out_str(&out, "\r\n");
    board_puts(buf);
}
#endif

int renderer_pipeline_enabled(int hw)
{
#if defined(__riscv) && SW9_PERF
    return hw;
#else
    (void)hw;
    return 0;
#endif
}

#ifdef __riscv
static int pending_finish(exec_sw_stats *st, uint32_t *cycles, int *presented,
                          uint32_t *ready_time)
{
    uint32_t code = 0u, index = 0u, tag = 0u, ticket;
    int rc;

    *presented = -1;
    if (st != NULL)
        exec_sw_stats_clear(st);
    if (!s_pending) {
        *cycles = 0u;
        *ready_time = board_cycles();
        return 0;
    }
    /* 失败的表也算收尾：留着 s_pending 会让之后每帧都再等 1 秒超时，
       且 renderer_prepare 拒绝软复位，切后端也救不回来。 */
    s_pending = 0;
    while (board_irq_engine_events() == s_pending_event) {
        if ((uint32_t)(board_cycles() - s_pending_start) > BOARD_HZ) {
            *ready_time = board_cycles();
            *cycles = *ready_time - s_pending_start;
            engine_error(-1, 0u, 0u);
            if(!gfx_engine_soft_reset())instance_release(s_pending_instance,s_pending_generation);
            return -1;
        }
    }
    rc = gfx_engine_wait(&code, &index, &tag);
    if(rc==GFX_ENG_OK) {
        instance_release(s_pending_instance,s_pending_generation);s_pending_instance=NULL;
    }
    *ready_time = board_cycles();  /* 此刻以前是 CPU 与引擎并行的工作，不含等场 */
    *cycles = *ready_time - s_pending_start;
#if SW9_PERF
    if(s_pending_plan) {
        gfx_frame_plan *p=s_pending_plan;uint32_t last=p->count-1u,v[9],k;
        exec_sw_stats band;
        if(rc==GFX_ENG_OK)perf_take(&band);else exec_sw_stats_clear(&band);
        p->pass[last].executed=band;p->pass[last].elapsed_cycles=*ready_time-s_pending_band_start;
        if(gfx_engine_perf_read(v,9)==0)for(k=0;k<9u;k++)p->pass[last].perf[k]=v[k]-s_pending_perf[k];
        if(st) {
            *st=s_pending_pre;st->cmd+=band.cmd;st->records_read+=band.records_read;
            st->pixel_total+=band.pixel_total;st->pixel_written+=band.pixel_written;
            st->key_skip+=band.key_skip;st->src_oob+=band.src_oob;
        }
        if(gfx_frame_complete(p,last,rc?rc:(int)code,rc==GFX_ENG_OK))rc=rc?rc:(code?(int)code:GFX_ENG_E_ARG);
        if(p->drained) {
            int released=gfx_frame_release(p,s_pending_ticket);s_pending_plan=NULL;
            if(!rc&&released)rc=released;
        }
    } else
#endif
    if (rc == GFX_ENG_OK)
        perf_take(st);
    if (rc != GFX_ENG_OK || code != 0u) {
        engine_error((rc != GFX_ENG_OK) ? rc : (int)code, index, tag);
        return -1;
    }
    if (board_present_request(s_pending_dst, &ticket) != 0 ||
        board_present_wait(ticket) != 0) {
#if SW9_PERF
        s_pipeline_front=-2;
#endif
        return -1;
    }
#if SW9_PERF
    s_pipeline_front=s_pending_dst;
#endif
    *presented = s_pending_dst;
    return 0;
}
#if SW9_PERF
static int bind_pass(const gfx_pass *v)
{
    uint32_t j;
    for(j=0;j<GFX_SURF_N;j++) {
        const gfx_surf_raw *s=&v->surfaces.s[j];int rc;
        if(!(s->ctrl&0x80000000u))continue;
        rc=gfx_engine_surf_set_space((int)j,s->base,gfx_surf_width(s),gfx_surf_height(s),
                                     gfx_surf_stride_log2(s),gfx_surf_space(s),gfx_surf_format(s));
        if(rc)return rc;
    }
    return 0;
}
static int pipeline_frame_begin(const gfx_list *list,uint32_t *sync_cycles)
{
    gfx_frame_plan *p=list->frame;uint32_t start=board_cycles(),ticket=0,i,last;
    int rc;exec_sw_state es;exec_sw_error err;
    if(gfx_frame_publish(p,&ticket))return -1;
    s_pending_start=start;exec_sw_stats_clear(&s_pending_pre);last=p->count-1u;
    for(i=0;i<last;i++) {
        gfx_pass *v=&p->pass[i];exec_sw_stats one;uint32_t before[9],after[9],elapsed,k,t0=board_cycles();
        rc=gfx_engine_perf_read(before,9);if(rc)goto fail;
        rc=bind_pass(v);if(rc)goto fail;
        if(v->dependency!=GFX_PASS_NO_DEP&&p->pass[v->dependency].completion_ticket!=ticket){rc=GFX_BUILD_BAD_ARG;goto fail;}
        exec_sw_reset(&es);rc=renderer_run(1,&v->surfaces,NULL,&es,&err,v->draw,&one,&elapsed);
        v->elapsed_cycles=board_cycles()-t0;v->executed=one;
        if(gfx_engine_perf_read(after,9)==0)for(k=0;k<9;k++)v->perf[k]=after[k]-before[k];
        if(rc)goto fail;
        rc=gfx_frame_complete(p,i,0,1);if(rc)goto fail;
        s_pending_pre.cmd+=one.cmd;s_pending_pre.records_read+=one.records_read;
        s_pending_pre.pixel_total+=one.pixel_total;s_pending_pre.pixel_written+=one.pixel_written;
        s_pending_pre.key_skip+=one.key_skip;s_pending_pre.src_oob+=one.src_oob;
    }
    i=last;
    if(p->pass[last].dependency!=GFX_PASS_NO_DEP&&p->pass[p->pass[last].dependency].completion_ticket!=ticket){rc=GFX_BUILD_BAD_ARG;goto fail;}
    s_pending_band_start=board_cycles();rc=bind_pass(&p->pass[last]);if(rc)goto fail;
    rc=gfx_engine_perf_read(s_pending_perf,9);if(rc)goto fail;
    s_pending_event=board_irq_engine_events();rc=submit_list(p->pass[last].draw);if(rc)goto fail;
    s_pending_plan=p;s_pending_ticket=ticket;*sync_cycles=board_cycles()-start;
#ifdef INJECT_PIPELINE_EARLY_RELEASE
    p->drained=1;gfx_frame_release(p,ticket);
#endif
    return 0;
fail:
    gfx_frame_complete(p,i,rc,rc!=GFX_ENG_E_TIMEOUT);
    if(p->drained)gfx_frame_release(p,ticket);
    return -1;
}
#endif
#endif

int renderer_pipeline_step(const gfx_list *list, int dst, exec_sw_stats *done_st,
                           uint32_t *exec_cycles, uint32_t *ready_time, int *presented)
{
#ifdef __riscv
    if (pending_finish(done_st, exec_cycles, presented, ready_time) != 0) {
#if SW9_PERF
        if(list->frame&&!list->frame->published)gfx_frame_release(list->frame,list->frame->ticket);
#endif
        return -1;
    }
#if SW9_PERF
    if(list->frame) {
        uint32_t sync;
        if(s_pipeline_front==-2||dst==s_pipeline_front||dst<0||dst>=BOARD_FB_COUNT||
           list->frame->pass[list->frame->count-1u].target.base!=g_mem_regions[REGION_FB0+dst].offset) {
            if(!list->frame->published)gfx_frame_release(list->frame,list->frame->ticket);
            engine_error(-3,0,0);return -1;
        }
        if(pipeline_frame_begin(list,&sync))return -1;
        /* 同步 prepass/绑定计入帧预算；上一帧等场仍由原 ready 时刻排除。 */
        *ready_time+=sync;s_pending_dst=dst;s_pending=1;return 0;
    }
    s_pending_plan=NULL;
#endif
    s_pending_instance=list->instances;
    s_pending_generation=list->instance_generation;
    s_pending_event = board_irq_engine_events();
    s_pending_start = board_cycles();
    if (submit_list(list) != GFX_ENG_OK) {
        instance_release(s_pending_instance,s_pending_generation);
        engine_error(-2, 0u, 0u);
        return -1;
    }
    s_pending_dst = dst;
    s_pending = 1;
    return 0;
#else
    (void)list; (void)dst; (void)exec_cycles; (void)ready_time; (void)presented;
    if (done_st != NULL)
        exec_sw_stats_clear(done_st);
    return -1;
#endif
}

int renderer_pipeline_flush(int *presented)
{
#ifdef __riscv
    uint32_t cycles, ready;
    return pending_finish(NULL, &cycles, presented, &ready);
#else
    *presented = -1;
    return 0;
#endif
}

int renderer_prepare(int hw, const gfx_surface_table *tab)
{
#ifdef __riscv
    uint32_t i;

    if (!hw)
        return 0;
    if (s_pending)
        return -1;             /* 切换前必须显式 flush，不能复位仍在途的命令表 */
    if (gfx_engine_soft_reset() != GFX_ENG_OK)
        return -1;
    for(i=0;i<2u;i++)s_instance_leases[i].owner=0u;
    s_pending_instance=NULL;s_pending_generation=0u;
#if SW9_PERF
    if(s_pipeline_front!=-2)s_pipeline_front=-1;
#endif
    for (i = 0u; i < GFX_SURF_N; i++) {
        const gfx_surf_raw *s = &tab->s[i];
        if ((s->ctrl & 0x80000000u) == 0u)
            continue;
        /* G1：space 随表登记——HW/TEX 后端的热集槽是 space=1 的 TEX 源 */
        if (gfx_engine_surf_set_space((int)i, s->base, s->dim & 0xFFFFu,
                                      s->dim >> 16, s->ctrl & 0x1Fu,
                                      (s->ctrl >> 8) & 3u,
                                      (s->ctrl >> 12) & 0xFu) != GFX_ENG_OK)
            return -1;
    }
    perf_take(NULL);
    board_engine_irq_enable();
#else
    (void)hw;
    (void)tab;
#endif
    return 0;
}

int renderer_run(int hw, const gfx_surface_table *tab, const gfx_mem *mem,
                 exec_sw_state *es, exec_sw_error *err, const gfx_list *list,
                 exec_sw_stats *st, uint32_t *cycles)
{
    const uint32_t t0 = board_cycles();
    int rc;
    if(list->frame) {
        uint32_t ticket;
        rc=renderer_submit_plan(hw,list->frame,mem,es,err,st,cycles,&ticket);
        if(list->frame->drained) {
            int release=gfx_frame_release(list->frame,ticket);
            if(!rc)rc=release;
        }
        return rc;
    }

#ifdef __riscv
    if (hw) {
        const uint32_t event0 = board_irq_engine_events();
        uint32_t code = 0u, index = 0u, tag = 0u;

        exec_sw_stats_clear(st);
        rc = submit_list(list);
        if (rc != GFX_ENG_OK)
            goto hw_fail;
        while (board_irq_engine_events() == event0) {
            if ((uint32_t)(board_cycles() - t0) > BOARD_HZ) {
                rc = GFX_ENG_E_TIMEOUT;
                goto hw_fail;
            }
        }
        rc = gfx_engine_wait(&code, &index, &tag);
        if (rc != GFX_ENG_OK)
            goto hw_fail;
        *cycles = board_cycles() - t0;
        instance_release(list->instances,list->instance_generation);
        perf_take(st);
        err->code = (int)code;
        err->index = index;
        err->tag = tag;
        if (code != 0u)
            engine_error((int)code, index, tag);
        return (int)code;
hw_fail:
        if(rc!=GFX_ENG_E_TIMEOUT || !gfx_engine_soft_reset())
            instance_release(list->instances,list->instance_generation);
        err->code = rc;
        err->index = 0u;
        err->tag = 0u;
        engine_error(rc, 0u, 0u);
        *cycles = board_cycles() - t0;
        return rc;
    }
#else
    (void)hw;
#endif
    exec_sw_stats_clear(st);
    if (list->first_error || list->rejected || list->overflow || !instance_owned(list))
        rc = GFX_BUILD_BAD_ARG;
    else if (!list->ended || (list->planned&&!list->planned->sealed))
        rc=GFX_BUILD_BAD_ARG;
    else if (list->strip_table != NULL) {
        static uint32_t scratch[32768];
        rc = exec_sw_run_strips(tab, mem, list->strip_table,
                                (list->strips.lstrips + 1u) * GFX_STRIP_REGION_BYTES,
                                &list->strips, scratch, sizeof scratch, st, err);
    } else if(list->instances) {
        const gfx_instance_arena *a=list->instances;
        const gfx_instance_view view={a->host,a->dev_base,a->used};
        rc=exec_sw_run_instances(es,tab,mem,list->buf,list->used,list->used,&view,1u,st,err,NULL,NULL);
    } else
        rc = exec_sw_run(es, tab, mem, list->buf, list->used, list->used, st, err);
    *cycles = board_cycles() - t0;
    instance_release(list->instances,list->instance_generation);
    return rc;
}

int renderer_submit_plan(int hw,gfx_frame_plan *p,const gfx_mem *mem,
                         exec_sw_state *es,exec_sw_error *err,exec_sw_stats *stats,
                         uint32_t *cycles,uint32_t *ticket)
{
    uint32_t i,j,t0=board_cycles();int rc;
    exec_sw_stats_clear(stats);*cycles=0;*ticket=0;
    *err=(exec_sw_error){0,0,0};
    if(gfx_frame_publish(p,ticket))return GFX_BUILD_BAD_ARG;
    for(i=0;i<p->count;i++) {
        gfx_pass *v=&p->pass[i];exec_sw_stats one;uint32_t elapsed,pass_start=board_cycles();
#ifdef __riscv
        uint32_t perf_before[9],perf_after[9],k;
        if(hw) {
            rc=gfx_engine_perf_read(perf_before,9);
            if(rc){gfx_frame_complete(p,i,rc,1);return rc;}
        }
#endif
        if(v->dependency!=GFX_PASS_NO_DEP&&p->pass[v->dependency].completion_ticket!=p->ticket) {
            gfx_frame_complete(p,i,GFX_BUILD_BAD_ARG,1);return GFX_BUILD_BAD_ARG;
        }
#ifdef __riscv
        if(hw)for(j=0;j<GFX_SURF_N;j++) {
            const gfx_surf_raw *s=&v->surfaces.s[j];
            if(!(s->ctrl&0x80000000u))continue;
            rc=gfx_engine_surf_set_space((int)j,s->base,gfx_surf_width(s),gfx_surf_height(s),
                                         gfx_surf_stride_log2(s),gfx_surf_space(s),gfx_surf_format(s));
            if(rc){gfx_frame_complete(p,i,rc,1);return rc;}
        }
#else
        (void)j;
#endif
        exec_sw_reset(es);
        rc=renderer_run(hw,&v->surfaces,mem,es,err,v->draw,&one,&elapsed);
        v->elapsed_cycles=board_cycles()-pass_start;
        v->executed=one;
#ifdef __riscv
        if(hw) {
            int pr=gfx_engine_perf_read(perf_after,9);
            if(pr&&!rc)rc=pr;
            if(!pr)for(k=0;k<9;k++)v->perf[k]=perf_after[k]-perf_before[k];
        }
#endif
        if(rc){gfx_frame_complete(p,i,rc,rc!=GFX_ENG_E_TIMEOUT);*cycles=board_cycles()-t0;return rc;}
        gfx_frame_complete(p,i,0,1);
        stats->cmd+=one.cmd;stats->records_read+=one.records_read;stats->pixel_total+=one.pixel_total;
        stats->pixel_written+=one.pixel_written;stats->key_skip+=one.key_skip;stats->src_oob+=one.src_oob;
    }
    *cycles=board_cycles()-t0;return 0;
}

int renderer_perf_read(uint32_t values[9])
{
#ifdef __riscv
    return gfx_engine_perf_read(values, 9u);
#else
    uint32_t i;
    for (i = 0u; i < 9u; i++)
        values[i] = 0u;
    return 0;
#endif
}

/* 诊断路径：软件按原表画 dst；硬件临时将同一槽映射到另一面执行同一命令表。 */
int renderer_compare_shadow(int dst, int shadow, const gfx_surface_table *tab,
                            const gfx_mem *mem, exec_sw_state *es,
                            exec_sw_error *err, const gfx_list *list,
                            const exec_sw_stats *expected)
{
#ifdef __riscv
    const gfx_surf_raw *orig = &tab->s[dst];
    const gfx_surf_raw *other = &tab->s[shadow];
    volatile uint32_t *probe;
    uint32_t cycles;
    exec_sw_stats st;
    int rc, restore;

    if (gfx_engine_surf_set(dst, other->base, gfx_surf_width(orig),
                            gfx_surf_height(orig), gfx_surf_stride_log2(orig),
                            gfx_surf_format(orig)) != GFX_ENG_OK)
        return -1;
    board_dcache_invalidate_all();
    probe = (volatile uint32_t *)board_fb(shadow);
    *probe = 0xDEADBEEFu; /* 故意留下旧缓存行；整帧 COPY 会覆盖 DDR 中这个哨兵 */
    (void)*probe;         /* 本核写缺失不分配缓存行，必须再读一次装入 */
    rc = renderer_run(1, tab, mem, es, err, list, &st, &cycles);
    restore = gfx_engine_surf_set(dst, orig->base, gfx_surf_width(orig),
                                   gfx_surf_height(orig), gfx_surf_stride_log2(orig),
                                   gfx_surf_format(orig));
    if (rc != GFX_OK || restore != GFX_ENG_OK)
        return -1;
    if (st.pixel_total != expected->pixel_total - expected->src_oob ||
        st.key_skip != expected->key_skip || st.pixel_written != expected->pixel_written) {
        board_puts("compare PERF mismatch\r\n");
        return -1;
    }
#ifndef INJECT_SW9_SKIP_INVALIDATE
    board_dcache_invalidate_all();
#endif
    return 0;
#else
    (void)dst; (void)shadow; (void)tab; (void)mem;
    (void)es; (void)err; (void)list; (void)expected;
    return -1;
#endif
}
