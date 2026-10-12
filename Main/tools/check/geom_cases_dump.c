/******************************************************************************
* @file geom_cases_dump.c: 把十一个整帧用例的表面表与命令表导成几何层模型能吃的记录
*
* @brief  几何层的独立模型 tools/check/geom_model.py 要「能直接吃十一个整帧用例的命令」。
*         用例的命令表是 C 代码在运行时构造的（sw/lib/gfx/cases.c 与 list.c），
*         所以不要在 Python 里照抄一遍——照抄会与真源分叉。这里链接 sw/lib/gfx
*         把它导成与 check_cmd_semantics.py --emit 相同的文本记录格式，模型再去解析。
*
*         编译与运行用同目录的 geom_cases_dump.sh（源文件清单以它为准：本文件加
*         sw/lib/gfx 的 cases.c、list.c、check.c 与 sw/lib/core/format.c）：
*             bash tools/check/geom_cases_dump.sh [输出文件]
*
*         逐像素期望值的导出用另一个更全的小程序（pix_cases_dump）。
******************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx/gfx.h"

static uint8_t *g_mem;

static void wr_hex(uint32_t v)
{
    printf("0x%08X", v);
}

/* 与 check_cmd_semantics.py 的 emit() 同形，但只写几何层要的三类记录。 */
static void dump_surface(const gfx_surface_table *t, uint32_t sid)
{
    const gfx_surf_raw *s = &t->s[sid];

    printf("[surf] slot=%u base=", sid);
    wr_hex(s->base);
    printf(" width=%u height=%u ctrl=", gfx_surf_width(s), gfx_surf_height(s));
    wr_hex(s->ctrl);
    printf(" (stride_log2=%u space=%u format=%u valid=%u)\n",
           gfx_surf_stride_log2(s), gfx_surf_space(s),
           gfx_surf_format(s), gfx_surf_valid(s));
}

static void dump_list(const gfx_cmd *cmds, uint32_t count)
{
    uint32_t i, j;

    printf("[list] count=%u\n", count);
    for (i = 0u; i < count; i++) {
        printf("[cmd]");
        for (j = 0u; j < 8u; j++) {
            printf(" ");
            wr_hex(cmds[i].w[j]);
        }
        printf("\n");
    }
}

int main(void)
{
    gfx_surface_table *t;
    gfx_cmd           *cmds;
    gfx_mem            m;
    uint32_t           idx, n1, n2, used;

    t    = (gfx_surface_table *)calloc(1u, sizeof(*t));
    cmds = (gfx_cmd *)calloc(2u * GFX_CASE_MAX_CMD, sizeof(*cmds));
    g_mem = (uint8_t *)calloc(1u, GFX_CASE_DEV_BYTES);
    if (t == 0 || cmds == 0 || g_mem == 0) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    m.dev_base = 0u;
    m.host     = g_mem;
    m.bytes    = GFX_CASE_DEV_BYTES;

    printf("# gfx2d 整帧用例的几何输入  abi=%d\n", 1);
    printf("# 生成者 tools/check/geom_cases_dump.c（链接 sw/lib/gfx），\n");
    printf("# 供 tools/check/geom_model.py --cases 解析；格式同 check_cmd_semantics.py --emit。\n");

    for (idx = 0u; idx < GFX_CASE_N; idx++) {
        const char *name = gfx_case_name(idx);

        n1 = gfx_case_build(idx, t, &m, GFX_CASE_FB_BASE, GFX_CASE_TEX_BASE,
                            cmds, &n2);
        used = n1 + n2;
        printf("[vector] group=whole-frame name=%s lists=%u code=0\n",
               name, (n2 != 0u) ? 2u : 1u);

        dump_surface(t, 0u);
        dump_surface(t, 1u);
        dump_list(cmds, n1);
        if (n2 != 0u)
            dump_list(cmds + n1, n2);

        fprintf(stderr, "%-24s 第一表 %u 条，第二表 %u 条，合计 %u 条\n",
                name, n1, n2, used);
    }

    free(g_mem);
    free(cmds);
    free(t);
    return 0;
}
