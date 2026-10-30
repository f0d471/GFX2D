/******************************************************************************
* @file sw6_scene_b.c: 场景 B —— 三张整屏图的大图切换
*
* @brief  实现见 sw6_scene_b.h。构图命令都只经 gfx_fill()／gfx_blit()，
*         目的表面是三张离屏图，源是素材包的表面；整屏 BLIT 的源与目的是
*         不同的表面号，不触发同表面重叠检查。
******************************************************************************/

#include <stddef.h>

#include "sw6_cfg.h"

#include "sw6_scene_b.h"
#include "sw6_report.h"

#include "assets.h"

/* 一次预合成提交里最多多少条：构图 46 + SET_CLIP + 打底 FILL + END。 */
#define SW6_COMP_MAX 64u

/* 离屏图 i 的主机指针（板级层给的分区指针 + 槽步长）。 */
static const uint32_t *off_host(const sw6_b_ctx *ctx,uint32_t i)
{
    const gfx_surf_raw *surface=&ctx->tab->s[SW6_SLOT_OFF(i)];
    uint64_t base=surface->base,end=base+(uint64_t)SW6_OFF_SLOT_STEP;
    if(base<ctx->mem->dev_base || end>(uint64_t)ctx->mem->dev_base+ctx->mem->bytes)return NULL;
    return (const uint32_t *)(const void *)(ctx->mem->host+(uint32_t)(base-ctx->mem->dev_base));
}

/* 追加一条并记账：eq_idx 是 SW6_DRAW_* 那一套的方程下标（copy/alpha/add/straight），
   传 SW6_EQ_NONE 表示这条不进 eq（SET_CLIP 与打底的全屏 FILL）。 */
#define SW6_EQ_NONE 4u

static void note(sw6_build_stat *fs, int rc, uint32_t eq_idx)
{
    fs->cmds++;
    if (rc != 0) {
        fs->bad++;
        return;
    }
    if (eq_idx < 4u)
        fs->eq[eq_idx]++;
}

/* OFF0 渐变：整屏 BG_GRAD + 4 个图集 ALPHA 格子 + 2 个色键方块。共 7 条。 */
static int view_x(int x,unsigned item_width,unsigned width)
{return (int)((int64_t)x*(width-item_width)/(SW6_CANVAS_WIDTH-item_width));}

static void comp_off0(gfx_list *l,int dst,sw6_build_stat *fs,unsigned width)
{
    uint32_t i;

    for(unsigned x=0;x<width;x+=SW6_CANVAS_WIDTH) {
        unsigned w=width-x;if(w>SW6_CANVAS_WIDTH)w=SW6_CANVAS_WIDTH;
        note(fs,gfx_blit(l,SW6_SLOT_BG_GRAD,0,0,dst,(int)x,0,(int)w,
                        SW6_CANVAS_HEIGHT,GFX_EQ_COPY,0,0,0),0);
    }

    for (i = 0u; i < 4u; i++) {
        const uint32_t cell = 9u + i * 5u;

        note(fs, gfx_blit(l, SW6_SLOT_ATLAS,
                             (int)(cell % 8u) * 16, (int)(cell / 8u) * 16, dst,
                             view_x((int)(40u+i*40u),16,width), 200, 16, 16,
                             GFX_EQ_ALPHA, 0u, 0, 0u), 1u);
    }

    for (i = 0u; i < 2u; i++)
        note(fs, gfx_blit(l, SW6_SLOT_KEY_DEMO, 0, 0, dst,
                             view_x((int)(200u+i*40u),32,width), 420, 32, 32,
                             GFX_EQ_COPY, 0u, 1, ASSET_KEY_DEMO_KEY), 0u);
}

/* OFF1 棋盘：CHECKER_TILE 5 列 × 4 行平铺。x 方向 5×128 = 640 正好；
   y 的第 4 行 384+128 = 512 > 480，末行由游戏区裁掉（离屏图也是目的表面，
   四者求交里的游戏区一项同样生效）。共 20 条。 */
static void comp_off1(gfx_list *l,int dst,sw6_build_stat *fs,unsigned width)
{
    uint32_t col, row;

    for (row = 0u; row < 4u; row++) {
        for (col = 0u; col < (width+127u)/128u; col++) {
            note(fs, gfx_blit(l, SW6_SLOT_CHECKER, 0, 0, dst,
                                 (int)(col * 128u), (int)(row * 128u), 128, 128,
                                 GFX_EQ_COPY, 0u, 0, 0u), 0u);
        }
    }
}

/* 程序生成的色带：由条号算出，没有随机数。 */
static uint32_t band_color(uint32_t i)
{
    const uint32_t r = (i * 8u) & 0xFFu;
    const uint32_t g = (255u - i * 8u) & 0xFFu;
    const uint32_t b = (i * 17u) & 0xFFu;

    return (r << 16) | (g << 8) | b;
}

/* OFF2 色带：30 条 640×16 的 FILL + 12 个图集 ALPHA 格子 + 4 个弹幕
   （预乘 ALPHA 两个、直通 ALPHA_STRAIGHT 两个）。共 46 条。
   用到 ALPHA／ALPHA_STRAIGHT 的元素都放在 HUD 矩形（x=400..639, y=0..159）以外，
   否则上屏后被 HUD 盖住，逐方程注错就看不出来（spec 2.2、R34）。 */
static void comp_off2(gfx_list *l,int dst,sw6_build_stat *fs,unsigned width)
{
    uint32_t i;

    for (i = 0u; i < 30u; i++) {
        gfx_rect r;

        r.x0 = 0;
        r.y0 = (int32_t)(i * 16u);
        r.x1 = (int32_t)width - 1;
        r.y1 = (int32_t)(i * 16u + 15u);
        note(fs, gfx_fill(l, dst, r, band_color(i), GFX_EQ_COPY, 0u), 0u);
    }

    for (i = 0u; i < 12u; i++) {
        const uint32_t cell = i * 3u;

        note(fs, gfx_blit(l, SW6_SLOT_ATLAS,
                             (int)(cell % 8u) * 16, (int)(cell / 8u) * 16, dst,
                             view_x(24+(int)i*20,16,width), 200 + (int)(i / 6u) * 24, 16, 16,
                             GFX_EQ_ALPHA, 0u, 0, 0u), 1u);
    }

    note(fs, gfx_blit(l, SW6_SLOT_BULLET_PM, 0, 0, dst, view_x(300,32,width), 420, 32, 32,
                         GFX_EQ_ALPHA, 0u, 0, 0u), 1u);
    note(fs, gfx_blit(l, SW6_SLOT_BULLET_PM, 0, 0, dst, view_x(340,32,width), 420, 32, 32,
                         GFX_EQ_ALPHA, 0u, 0, 0u), 1u);
    note(fs, gfx_blit(l, SW6_SLOT_BULLET_ST, 0, 0, dst, view_x(60,32,width), 240, 32, 32,
                         GFX_EQ_ALPHA_STRAIGHT, 0u, 0, 0u), 3u);
    note(fs, gfx_blit(l, SW6_SLOT_BULLET_ST, 0, 0, dst, view_x(100,32,width), 240, 32, 32,
                         GFX_EQ_ALPHA_STRAIGHT, 0u, 0, 0u), 3u);
}

void sw6_b_begin(sw6_scene_b *b, uint32_t k0)
{
    uint32_t i;

    b->s     = 0u;
    b->shown = 0u;
    b->k     = (k0 < SW6_K_MIN) ? SW6_K_MIN : ((k0 > SW6_K_MAX) ? SW6_K_MAX : k0);
    for (i = 0u; i < SW6_SCENES; i++)
        b->ref[i] = 0u;
}

void sw6_b_build_view(unsigned scene,gfx_list *list,int dst,unsigned width,sw6_build_stat *stats)
{
    if(scene==0)comp_off0(list,dst,stats,width);
    else if(scene==1)comp_off1(list,dst,stats,width);
    else comp_off2(list,dst,stats,width);
}

int sw6_b_precompose(sw6_scene_b *b, const sw6_b_ctx *ctx,
                     uint32_t eq_out[4], uint32_t *out_bad)
{
    sw6_build_stat fs;
    uint32_t       i;

    sw6_build_stat_clear(&fs);

    for (i = 0u; i < SW6_SCENES; i++) {
        gfx_list       l;
        exec_sw_stats  st;
        gfx_rect       all;
        const int      dst = SW6_SLOT_OFF(i);
        int            rc;

        all.x0 = 0;
        all.y0 = 0;
        all.x1 = (int32_t)SW6_CANVAS_WIDTH - 1;
        all.y1 = (int32_t)SW6_CANVAS_HEIGHT - 1;

        gfx_list_begin(&l, ctx->cmds, ctx->cap, ctx->tab);
        note(&fs, gfx_set_clip(&l, all), SW6_EQ_NONE);
        note(&fs, gfx_fill(&l, dst, all, SW6_BG_COLOR, GFX_EQ_COPY, 0u), SW6_EQ_NONE);

        if (i == 0u)
            comp_off0(&l, dst, &fs,SW6_CANVAS_WIDTH);
        else if (i == 1u)
            comp_off1(&l, dst, &fs,SW6_CANVAS_WIDTH);
        else
            comp_off2(&l, dst, &fs,SW6_CANVAS_WIDTH);

        if (gfx_list_end(&l) != 0)
            fs.bad++;
        if (l.rejected != 0u || l.overflow != 0u)
            fs.bad += l.rejected + l.overflow;

        exec_sw_reset(ctx->es);
        exec_sw_stats_clear(&st);
        rc = exec_sw_run(ctx->es, ctx->tab, ctx->mem, l.buf, l.used, l.used,
                         &st, ctx->err);
        if (rc != GFX_OK)
            return rc;

        const uint32_t *pixels=off_host(ctx,i);
        if(!pixels)return GFX_BUILD_BAD_ARG;
        b->ref[i] = sw6_crc_scene(pixels);
    }

    if (eq_out != NULL) {
        uint32_t k;

        for (k = 0u; k < 4u; k++)
            eq_out[k] = fs.eq[k];
    }
    if (out_bad != NULL)
        *out_bad = fs.bad;
    return 0;
}

void sw6_b_adjust(sw6_scene_b *b, int up, int down)
{
    if (up && b->k < SW6_K_MAX)
        b->k++;
    if (down && b->k > SW6_K_MIN)
        b->k--;
}

uint32_t sw6_b_step(sw6_scene_b *b)
{
    if (b->shown >= b->k) {
        b->s = (b->s + 1u) % SW6_SCENES;
        b->shown = 0u;
    }
    b->shown++;
    return b->s;
}
