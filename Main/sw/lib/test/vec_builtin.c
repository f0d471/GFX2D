/******************************************************************************
* @file vec_builtin.c: C 侧手写的一致性向量
*
* @brief  输入按 spec 第 02 章的条文构造，期望值手算后写成字面量，
*         不调用任何执行器来算、也不看工具里的常量。
*
*         表面一律取 4x2、stride_log2 = 4，所以一张表面的 8 个像素在地址上
*         正好连续（每行 4 个字 = 16 字节 = 一个行跨），种子与期望可以按
*         一段连续的字来写。
*
*         槽位分工固定，后面每条向量只说用了哪几个：
*           0  帧缓冲 XRGB        5  基址不对齐
*           1  纹理 XRGB          6  stride_log2 = 1
*           2  纹理 PREMULT       7  控制字保留位 [7:5] 非零
*           3  纹理 STRAIGHT      8  space = TEX
*           4  未登记             9  format = INDEX8
******************************************************************************/

#include <string.h>

#include "vec_builtin.h"

#define FB      0x00001000u
#define TX      0x00002000u
#define TP      0x00003000u
#define TS      0x00004000u

#define C_XRGB  0x80000004u                 /* valid | format=0 | space=0 | stride_log2=4 */
#define C_PM    0x80001004u
#define C_ST    0x80002004u
#define C_STR1  0x80000001u                 /* stride_log2 = 1，违反 2 <= stride_log2 */
#define C_RSVD5 0x80000024u                 /* 控制字保留段 [7:5] 非零 */
#define C_TEX   0x80000104u                 /* space = TEX */
#define C_SPC3  0x80000304u                 /* space = 3：v1 与 v1-TEX 都非法（G1 起
                                               space=1 的 BLIT 源已合法化，不能再当
                                               「非法空间」的例子） */
#define C_IDX8  0x80003004u                 /* format = INDEX8 */

#define SEED    0x00004080u                 /* 帧缓冲的种子像素 */
#define SEED2   0x00808080u                 /* ADD 饱和用的亮底 */

/* 逐像素都不同的纹理，用来验坐标映射。 */
static const uint32_t TEXPAT[8] = {
    0x00010203u, 0x00111213u, 0x00212223u, 0x00313233u,
    0x00414243u, 0x00515253u, 0x00616263u, 0x00717273u
};

static uint32_t w0f(uint32_t op, uint32_t eq, uint32_t key,
                    uint32_t src, uint32_t dst, uint32_t c)
{
    return op | (eq << 8) | (key << 12) | (src << 16) | (dst << 20) | (c << 24);
}

static uint32_t pair(uint32_t lo, uint32_t hi)
{
    return (lo & 0xFFFFu) | ((hi & 0xFFFFu) << 16);
}

static void reset(vec_case *v, const char *group, const char *name)
{
    memset(v, 0, sizeof *v);
    v->reads = -1;
    v->clip[0] = 0;
    v->clip[1] = 0;
    v->clip[2] = GFX_SCREEN_W - 1;
    v->clip[3] = GFX_SCREEN_H - 1;
    strncpy(v->group, group, sizeof v->group - 1u);
    strncpy(v->name, name, sizeof v->name - 1u);
}

static void surf(vec_case *v, uint32_t slot, uint32_t base, uint32_t ctrl)
{
    v->surf[slot].base = base;
    v->surf[slot].dim  = pair(4u, 2u);
    v->surf[slot].ctrl = ctrl;
    v->surf_seen |= 1u << slot;
}

static void list_of(vec_case *v, uint32_t count)
{
    v->listcount[v->nlist] = count;
    v->listncmd[v->nlist]  = 0u;
    v->nlist++;
}

static void cmd(vec_case *v, uint32_t a, uint32_t b, uint32_t c, uint32_t d,
                uint32_t e, uint32_t f, uint32_t g, uint32_t h)
{
    gfx_cmd *r = &v->cmds[v->ncmd];

    r->w[0] = a; r->w[1] = b; r->w[2] = c; r->w[3] = d;
    r->w[4] = e; r->w[5] = f; r->w[6] = g; r->w[7] = h;
    v->ncmd++;
    v->listncmd[v->nlist - 1u]++;
}

/* 连续 n 个字的初始内容 */
static void mem_run(vec_case *v, uint32_t base, uint32_t n, uint32_t val)
{
    uint32_t i;

    for (i = 0u; i < n; i++) {
        v->mem_addr[v->nmem] = base + i * 4u;
        v->mem_val[v->nmem]  = val;
        v->nmem++;
    }
}

static void mem_pat(vec_case *v, uint32_t base, const uint32_t *pat, uint32_t n)
{
    uint32_t i;

    for (i = 0u; i < n; i++) {
        v->mem_addr[v->nmem] = base + i * 4u;
        v->mem_val[v->nmem]  = pat[i];
        v->nmem++;
    }
}

static void exp_run(vec_case *v, uint32_t base, uint32_t n, uint32_t val)
{
    uint32_t i;

    for (i = 0u; i < n; i++) {
        v->exp_addr[v->nexp] = base + i * 4u;
        v->exp_val[v->nexp]  = val;
        v->nexp++;
    }
}

static void exp_pat(vec_case *v, uint32_t base, const uint32_t *pat, uint32_t n)
{
    uint32_t i;

    for (i = 0u; i < n; i++) {
        v->exp_addr[v->nexp] = base + i * 4u;
        v->exp_val[v->nexp]  = pat[i];
        v->nexp++;
    }
}

static void err_at(vec_case *v, int code, uint32_t listidx, uint32_t idx, uint32_t tag)
{
    v->code    = code;
    v->errlist = listidx;
    v->erridx  = idx;
    v->errtag  = tag;
}

/* 一条只有一个错误记录的向量：登记帧缓冲、放一条命令、期望它报某个码。 */
static void one_bad(vec_case *v, uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3,
                    uint32_t w4, uint32_t w5, uint32_t w6, uint32_t tag, int code)
{
    list_of(v, 1u);
    cmd(v, w0, w1, w2, w3, w4, w5, w6, tag);
    err_at(v, code, 0u, 0u, tag);
    v->reads = 1;
}

uint32_t vec_builtin_count(void)
{
    return 45u;
}

int vec_builtin_get(uint32_t idx, vec_case *v)
{
    const uint32_t wh42 = pair(4u, 2u);

    switch (idx) {

    /* --- 第 01 组：合法操作码与方程 --------------------------------- */
    case 0:
        reset(v, "01", "b-nop");
        list_of(v, 1u);
        cmd(v, w0f(GFX_OP_NOP, 0u, 0u, 0u, 0u, 0u), 0u, 0u, 0u, 0u, 0u, 0u, 0u);
        v->reads = 1;
        break;

    case 1:
        reset(v, "01", "b-end");
        list_of(v, 1u);
        cmd(v, w0f(GFX_OP_END, 0u, 0u, 0u, 0u, 0u), 0u, 0u, 0u, 0u, 0u, 0u, 0x77u);
        v->reads = 1;
        break;

    case 2:
        /* COPY 的 Fs = 255、Fd = 0，输出等于源的 RGB，高字节清零 */
        reset(v, "01", "b-fill-copy");
        surf(v, 0u, FB, C_XRGB);
        list_of(v, 1u);
        cmd(v, w0f(GFX_OP_FILL, GFX_EQ_COPY, 0u, 0u, 0u, 0u),
            0u, 0u, wh42, 0x00112233u, 0u, 0u, 0u);
        mem_run(v, FB, 8u, SEED);
        exp_run(v, FB, 8u, 0x00112233u);
        v->reads = 1;
        break;

    case 3:
        /* ADD：192 + 128 = 320，clamp 到 255 */
        reset(v, "01", "b-fill-add-saturates");
        surf(v, 0u, FB, C_XRGB);
        list_of(v, 1u);
        cmd(v, w0f(GFX_OP_FILL, GFX_EQ_ADD, 0u, 0u, 0u, 0u),
            0u, 0u, wh42, 0x00C0C0C0u, 0u, 0u, 0u);
        mem_run(v, FB, 8u, SEED2);
        exp_run(v, FB, 8u, 0x00FFFFFFu);
        v->reads = 1;
        break;

    case 4:
        /* MUL 是单项：Fs = dst、Fd = 0，源为 0 时输出全 0 */
        reset(v, "01", "b-fill-mul-src-zero");
        surf(v, 0u, FB, C_XRGB);
        list_of(v, 1u);
        cmd(v, w0f(GFX_OP_FILL, GFX_EQ_MUL, 0u, 0u, 0u, 0u),
            0u, 0u, wh42, 0x00000000u, 0u, 0u, 0u);
        mem_run(v, FB, 8u, SEED);
        exp_run(v, FB, 8u, 0x00000000u);
        v->reads = 1;
        break;

    case 5:
        /* BLIT COPY，逐像素不同的源，验坐标映射 */
        reset(v, "01", "b-blit-copy-coords");
        surf(v, 0u, FB, C_XRGB);
        surf(v, 1u, TX, C_XRGB);
        list_of(v, 1u);
        cmd(v, w0f(GFX_OP_BLIT, GFX_EQ_COPY, 0u, 1u, 0u, 0u),
            0u, 0u, wh42, 0u, 0u, 0u, 0u);
        mem_run(v, FB, 8u, SEED);
        mem_pat(v, TX, TEXPAT, 8u);
        exp_pat(v, FB, TEXPAT, 8u);
        exp_pat(v, TX, TEXPAT, 8u);
        v->reads = 1;
        break;

    case 6:
        /* SET_CLIP：w1 左上角、w3 尺寸、闭区间 */
        reset(v, "01", "b-set-clip");
        list_of(v, 1u);
        cmd(v, w0f(GFX_OP_SET_CLIP, 0u, 0u, 0u, 0u, 0u),
            pair(1u, 0u), 0u, pair(2u, 1u), 0u, 0u, 0u, 0u);
        v->clip[0] = 1; v->clip[1] = 0; v->clip[2] = 2; v->clip[3] = 0;
        v->reads = 1;
        break;

    /* --- 第 02 组：必须为 0 的字段 ---------------------------------- */
    case 7:
        reset(v, "02", "b-nop-w1-nonzero");
        one_bad(v, w0f(GFX_OP_NOP, 0u, 0u, 0u, 0u, 0u), 1u, 0u, 0u, 0u, 0u, 0u,
                0x1234u, GFX_E_BAD_FIELD);
        break;

    case 8:
        /* FILL 不支持色键，置位不静默忽略 */
        reset(v, "02", "b-fill-key-set");
        surf(v, 0u, FB, C_XRGB);
        one_bad(v, w0f(GFX_OP_FILL, GFX_EQ_COPY, 1u, 0u, 0u, 0u), 0u, 0u, wh42,
                0x00112233u, 0u, 0u, 0x11u, GFX_E_BAD_FIELD);
        mem_run(v, FB, 8u, SEED);
        exp_run(v, FB, 8u, SEED);
        break;

    case 9:
        reset(v, "02", "b-fill-src-nonzero");
        surf(v, 0u, FB, C_XRGB);
        one_bad(v, w0f(GFX_OP_FILL, GFX_EQ_COPY, 0u, 5u, 0u, 0u), 0u, 0u, wh42,
                0u, 0u, 0u, 0x12u, GFX_E_BAD_FIELD);
        break;

    case 10:
        reset(v, "02", "b-blit-w4-nonzero");
        surf(v, 0u, FB, C_XRGB);
        surf(v, 1u, TX, C_XRGB);
        one_bad(v, w0f(GFX_OP_BLIT, GFX_EQ_COPY, 0u, 1u, 0u, 0u), 0u, 0u, wh42,
                1u, 0u, 0u, 0x13u, GFX_E_BAD_FIELD);
        break;

    case 11:
        reset(v, "02", "b-setclip-w2-nonzero");
        one_bad(v, w0f(GFX_OP_SET_CLIP, 0u, 0u, 0u, 0u, 0u), 0u, 1u, pair(2u, 1u),
                0u, 0u, 0u, 0x14u, GFX_E_BAD_FIELD);
        break;

    case 12:
        /* END 的方程列是 0，所以 eq != 0 在字段步就报 BAD_FIELD，不是 BAD_EQUATION */
        reset(v, "02", "b-end-eq-nonzero");
        one_bad(v, w0f(GFX_OP_END, 1u, 0u, 0u, 0u, 0u), 0u, 0u, 0u, 0u, 0u, 0u,
                0x15u, GFX_E_BAD_FIELD);
        break;

    case 13:
        reset(v, "02", "b-tint-set");
        surf(v, 0u, FB, C_XRGB);
        one_bad(v, w0f(GFX_OP_FILL, GFX_EQ_COPY, 0u, 0u, 0u, 0u) | (1u << 13),
                0u, 0u, wh42, 0u, 0u, 0u, 0x16u, GFX_E_BAD_FIELD);
        break;

    case 14:
        reset(v, "02", "b-rsvd-set");
        one_bad(v, w0f(GFX_OP_NOP, 0u, 0u, 0u, 0u, 0u) | (1u << 14),
                0u, 0u, 0u, 0u, 0u, 0u, 0x17u, GFX_E_BAD_FIELD);
        break;

    case 15:
        /* 非 CONST 命令的常数字段必须为 0 */
        reset(v, "02", "b-copy-c-nonzero");
        surf(v, 0u, FB, C_XRGB);
        one_bad(v, w0f(GFX_OP_FILL, GFX_EQ_COPY, 0u, 0u, 0u, 0x40u),
                0u, 0u, wh42, 0u, 0u, 0u, 0x18u, GFX_E_BAD_FIELD);
        break;

    /* --- 第 03 组：表面、space 与 format ---------------------------- */
    case 16:
        reset(v, "03", "b-dst-unregistered");
        one_bad(v, w0f(GFX_OP_FILL, GFX_EQ_COPY, 0u, 0u, 4u, 0u), 0u, 0u, wh42,
                0u, 0u, 0u, 0x21u, GFX_E_BAD_SURFACE);
        break;

    case 17:
        reset(v, "03", "b-dst-misaligned-base");
        surf(v, 5u, 0x00005002u, C_XRGB);
        one_bad(v, w0f(GFX_OP_FILL, GFX_EQ_COPY, 0u, 0u, 5u, 0u), 0u, 0u, wh42,
                0u, 0u, 0u, 0x22u, GFX_E_BAD_SURFACE);
        break;

    case 18:
        reset(v, "03", "b-dst-stride-too-small");
        surf(v, 6u, 0x00006000u, C_STR1);
        one_bad(v, w0f(GFX_OP_FILL, GFX_EQ_COPY, 0u, 0u, 6u, 0u), 0u, 0u, wh42,
                0u, 0u, 0u, 0x23u, GFX_E_BAD_SURFACE);
        break;

    case 19:
        reset(v, "03", "b-dst-ctrl-reserved");
        surf(v, 7u, 0x00007000u, C_RSVD5);
        one_bad(v, w0f(GFX_OP_FILL, GFX_EQ_COPY, 0u, 0u, 7u, 0u), 0u, 0u, wh42,
                0u, 0u, 0u, 0x24u, GFX_E_BAD_SURFACE);
        break;

    case 20:
        reset(v, "03", "b-dst-space-tex");
        surf(v, 8u, 0x00003000u, C_TEX);
        one_bad(v, w0f(GFX_OP_FILL, GFX_EQ_COPY, 0u, 0u, 8u, 0u), 0u, 0u, wh42,
                0u, 0u, 0u, 0x25u, GFX_E_BAD_SPACE);
        break;

    case 21:
        reset(v, "03", "b-dst-format-index8");
        surf(v, 9u, 0x00009000u, C_IDX8);
        one_bad(v, w0f(GFX_OP_FILL, GFX_EQ_COPY, 0u, 0u, 9u, 0u), 0u, 0u, wh42,
                0u, 0u, 0u, 0x26u, GFX_E_BAD_FORMAT);
        break;

    case 22:
        /* ALPHA 只收 XRGB 或 PREMULT 源 */
        reset(v, "03", "b-alpha-with-straight-src");
        surf(v, 0u, FB, C_XRGB);
        surf(v, 3u, TS, C_ST);
        one_bad(v, w0f(GFX_OP_BLIT, GFX_EQ_ALPHA, 0u, 3u, 0u, 0u), 0u, 0u, wh42,
                0u, 0u, 0u, 0x27u, GFX_E_BAD_FORMAT);
        break;

    case 23:
        /* ALPHA_STRAIGHT 只收 XRGB 或 STRAIGHT 源 */
        reset(v, "03", "b-straight-with-pm-src");
        surf(v, 0u, FB, C_XRGB);
        surf(v, 2u, TP, C_PM);
        one_bad(v, w0f(GFX_OP_BLIT, GFX_EQ_ALPHA_STRAIGHT, 0u, 2u, 0u, 0u),
                0u, 0u, wh42, 0u, 0u, 0u, 0x28u, GFX_E_BAD_FORMAT);
        break;

    /* --- 第 04 组：FILL 的常量与预乘约束 ---------------------------- */
    case 24:
        /* a = 255 时 Fd = 0，结果就是源的 RGB */
        reset(v, "04", "b-fill-alpha-a255");
        surf(v, 0u, FB, C_XRGB);
        list_of(v, 1u);
        cmd(v, w0f(GFX_OP_FILL, GFX_EQ_ALPHA, 0u, 0u, 0u, 0u),
            0u, 0u, wh42, 0xFFAABBCCu, 0u, 0u, 0u);
        mem_run(v, FB, 8u, SEED);
        exp_run(v, FB, 8u, 0x00AABBCCu);
        v->reads = 1;
        break;

    case 25:
        reset(v, "04", "b-fill-alpha-r-over-a");
        surf(v, 0u, FB, C_XRGB);
        one_bad(v, w0f(GFX_OP_FILL, GFX_EQ_ALPHA, 0u, 0u, 0u, 0u), 0u, 0u, wh42,
                0x80FF0000u, 0u, 0u, 0x31u, GFX_E_BAD_PREMULT);
        mem_run(v, FB, 8u, SEED);
        exp_run(v, FB, 8u, SEED);
        break;

    case 26:
        /* a = 0 照常写回，写入值与原值相同：不做 alpha 分支 */
        reset(v, "04", "b-fill-alpha-a0-rewrites");
        surf(v, 0u, FB, C_XRGB);
        list_of(v, 1u);
        cmd(v, w0f(GFX_OP_FILL, GFX_EQ_ALPHA, 0u, 0u, 0u, 0u),
            0u, 0u, wh42, 0x00000000u, 0u, 0u, 0u);
        mem_run(v, FB, 8u, SEED);
        exp_run(v, FB, 8u, SEED);
        v->reads = 1;
        break;

    /* --- 第 05 组：重叠、零面积、色键、源越界 ----------------------- */
    case 27:
        reset(v, "05", "b-overlap-same-surface");
        surf(v, 0u, FB, C_XRGB);
        one_bad(v, w0f(GFX_OP_BLIT, GFX_EQ_COPY, 0u, 0u, 0u, 0u),
                pair(0u, 0u), pair(3u, 0u), wh42, 0u, 0u, 0u, 0x41u, GFX_E_OVERLAP);
        mem_run(v, FB, 8u, SEED);
        exp_run(v, FB, 8u, SEED);
        break;

    case 28:
        /* 半开区间相邻不算相交；目的落在表面之外，没有像素被写 */
        reset(v, "05", "b-same-surface-adjacent-ok");
        surf(v, 0u, FB, C_XRGB);
        list_of(v, 1u);
        cmd(v, w0f(GFX_OP_BLIT, GFX_EQ_COPY, 0u, 0u, 0u, 0u),
            pair(0u, 0u), pair(4u, 0u), wh42, 0u, 0u, 0u, 0u);
        mem_run(v, FB, 8u, SEED);
        exp_run(v, FB, 8u, SEED);
        v->reads = 1;
        break;

    case 29:
        reset(v, "05", "b-zero-width-ok");
        surf(v, 0u, FB, C_XRGB);
        list_of(v, 1u);
        cmd(v, w0f(GFX_OP_FILL, GFX_EQ_COPY, 0u, 0u, 0u, 0u),
            0u, 0u, pair(0u, 2u), 0x00112233u, 0u, 0u, 0u);
        mem_run(v, FB, 8u, SEED);
        exp_run(v, FB, 8u, SEED);
        v->reads = 1;
        break;

    case 30:
        /* 零面积不跳过前八步：目的格式非法仍要报出来 */
        reset(v, "05", "b-zero-width-bad-format");
        surf(v, 9u, 0x00009000u, C_IDX8);
        one_bad(v, w0f(GFX_OP_FILL, GFX_EQ_COPY, 0u, 0u, 9u, 0u), 0u, 0u,
                pair(0u, 2u), 0u, 0u, 0u, 0x42u, GFX_E_BAD_FORMAT);
        break;

    case 31:
        /* 色键比较的是源表面的 32 位存储字，命中即跳过，目的不变 */
        reset(v, "05", "b-key-hits-all");
        surf(v, 0u, FB, C_XRGB);
        surf(v, 1u, TX, C_XRGB);
        list_of(v, 1u);
        cmd(v, w0f(GFX_OP_BLIT, GFX_EQ_COPY, 1u, 1u, 0u, 0u),
            0u, 0u, wh42, 0u, 0x00112233u, 0u, 0u);
        mem_run(v, FB, 8u, SEED);
        mem_run(v, TX, 8u, 0x00112233u);
        exp_run(v, FB, 8u, SEED);
        exp_run(v, TX, 8u, 0x00112233u);
        v->reads = 1;
        break;

    case 32:
        /* 源越界只跳过该像素：sx = 2 + px，px = 2、3 时越出 4 宽的纹理 */
        reset(v, "05", "b-src-out-of-bounds-skips");
        surf(v, 0u, FB, C_XRGB);
        surf(v, 1u, TX, C_XRGB);
        list_of(v, 1u);
        cmd(v, w0f(GFX_OP_BLIT, GFX_EQ_COPY, 0u, 1u, 0u, 0u),
            pair(2u, 0u), 0u, wh42, 0u, 0u, 0u, 0u);
        mem_run(v, FB, 8u, SEED);
        mem_pat(v, TX, TEXPAT, 8u);
        {
            static const uint32_t want[8] = {
                0x00212223u, 0x00313233u, SEED, SEED,
                0x00616263u, 0x00717273u, SEED, SEED
            };
            exp_pat(v, FB, want, 8u);
        }
        exp_pat(v, TX, TEXPAT, 8u);
        v->reads = 1;
        break;

    /* --- 第 06 组：COUNT、END 与取数失败 ---------------------------- */
    case 33:
        reset(v, "06", "b-count-zero");
        list_of(v, 0u);
        v->reads = 0;
        break;

    case 34:
        /* COUNT 耗尽而没有 END 也是正常结束；两条都执行，后一条盖住前一条 */
        reset(v, "06", "b-count-exhausted-no-end");
        surf(v, 0u, FB, C_XRGB);
        list_of(v, 2u);
        cmd(v, w0f(GFX_OP_FILL, GFX_EQ_COPY, 0u, 0u, 0u, 0u),
            0u, 0u, wh42, 0x00111111u, 0u, 0u, 0u);
        cmd(v, w0f(GFX_OP_FILL, GFX_EQ_COPY, 0u, 0u, 0u, 0u),
            0u, 0u, wh42, 0x00222222u, 0u, 0u, 1u);
        mem_run(v, FB, 8u, SEED);
        exp_run(v, FB, 8u, 0x00222222u);
        v->reads = 2;
        break;

    case 35:
        /* COUNT 大于可读记录数：第二次取数失败，标签读 0 而不是上一条的 */
        reset(v, "06", "b-fetch-short-table");
        list_of(v, 2u);
        cmd(v, w0f(GFX_OP_NOP, 0u, 0u, 0u, 0u, 0u), 0u, 0u, 0u, 0u, 0u, 0u, 0x99u);
        err_at(v, GFX_E_FETCH, 0u, 1u, 0u);
        v->reads = 2;
        break;

    /* --- 第 07 组：相邻两步的优先级 --------------------------------- */
    case 36:
        reset(v, "07", "b-prio-opcode-over-field");
        one_bad(v, 0x20u, 1u, 0u, 0u, 0u, 0u, 0u, 0x51u, GFX_E_BAD_OPCODE);
        break;

    case 37:
        /* 字段在方程之前：eq = 7 且 tint = 1 时报 BAD_FIELD */
        reset(v, "07", "b-prio-field-over-equation");
        surf(v, 0u, FB, C_XRGB);
        one_bad(v, w0f(GFX_OP_FILL, 7u, 0u, 0u, 0u, 0u) | (1u << 13),
                0u, 0u, wh42, 0u, 0u, 0u, 0x52u, GFX_E_BAD_FIELD);
        break;

    case 38:
        /* 方程在表面之前 */
        reset(v, "07", "b-prio-equation-over-surface");
        one_bad(v, w0f(GFX_OP_FILL, 7u, 0u, 0u, 4u, 0u), 0u, 0u, wh42,
                0u, 0u, 0u, 0x53u, GFX_E_BAD_EQUATION);
        break;

    case 39:
        /* space 与 format 各一步、源在前：源 space 非法压过目的格式非法。
           源用 space=3（两个 profile 都非法）；v1-TEX（G1）起 space=1 的
           BLIT 源合法化，这条在 2026-10-07 从 C_TEX 改过来。 */
        reset(v, "07", "b-prio-space-over-format");
        surf(v, 8u, 0x00008000u, C_SPC3);
        surf(v, 9u, 0x00009000u, C_IDX8);
        one_bad(v, w0f(GFX_OP_BLIT, GFX_EQ_COPY, 0u, 8u, 9u, 0u), 0u, 0u, wh42,
                0u, 0u, 0u, 0x54u, GFX_E_BAD_SPACE);
        break;

    /* --- 第 08 组：首错停止与错误定位 ------------------------------- */
    case 40:
        /* 前序写入不回滚，后续记录不读取 */
        reset(v, "08", "b-error-keeps-prior-writes");
        surf(v, 0u, FB, C_XRGB);
        list_of(v, 3u);
        cmd(v, w0f(GFX_OP_FILL, GFX_EQ_COPY, 0u, 0u, 0u, 0u),
            0u, 0u, wh42, 0x00112233u, 0u, 0u, 0u);
        cmd(v, w0f(GFX_OP_BAND, 0u, 0u, 0u, 0u, 0u), 0u, 0u, 0u, 0u, 0u, 0u, 0xBEEFu);
        cmd(v, w0f(GFX_OP_FILL, GFX_EQ_COPY, 0u, 0u, 0u, 0u),
            0u, 0u, wh42, 0x00445566u, 0u, 0u, 2u);
        mem_run(v, FB, 8u, SEED);
        exp_run(v, FB, 8u, 0x00112233u);
        err_at(v, GFX_E_BAD_OPCODE, 0u, 1u, 0xBEEFu);
        v->reads = 2;
        break;

    case 41:
        /* 标签取失败记录自己的 w7，与索引是两回事 */
        reset(v, "08", "b-error-tag-differs-from-index");
        surf(v, 0u, FB, C_XRGB);
        list_of(v, 2u);
        cmd(v, w0f(GFX_OP_NOP, 0u, 0u, 0u, 0u, 0u), 0u, 0u, 0u, 0u, 0u, 0u, 0u);
        cmd(v, w0f(GFX_OP_FILL, GFX_EQ_COPY, 1u, 0u, 0u, 0u),
            0u, 0u, wh42, 0u, 0u, 0u, 0x5A5Au);
        err_at(v, GFX_E_BAD_FIELD, 0u, 1u, 0x5A5Au);
        v->reads = 2;
        break;

    /* --- 第 09 组：SET_CLIP 的状态 ---------------------------------- */
    case 42:
        /* 两次提交共用一个执行器状态：第二张表继承第一张表留下的框 */
        reset(v, "09", "b-clip-inherits-across-lists");
        surf(v, 0u, FB, C_XRGB);
        list_of(v, 1u);
        cmd(v, w0f(GFX_OP_SET_CLIP, 0u, 0u, 0u, 0u, 0u),
            0u, 0u, pair(2u, 1u), 0u, 0u, 0u, 0u);
        list_of(v, 1u);
        cmd(v, w0f(GFX_OP_FILL, GFX_EQ_COPY, 0u, 0u, 0u, 0u),
            0u, 0u, wh42, 0x00112233u, 0u, 0u, 0u);
        mem_run(v, FB, 8u, SEED);
        {
            static const uint32_t want[8] = {
                0x00112233u, 0x00112233u, SEED, SEED,
                SEED, SEED, SEED, SEED
            };
            exp_pat(v, FB, want, 8u);
        }
        v->clip[0] = 0; v->clip[1] = 0; v->clip[2] = 1; v->clip[3] = 0;
        v->reads = 2;
        break;

    case 43:
        /* 空矩形全部裁掉，之后的命令一条也不落地 */
        reset(v, "09", "b-clip-empty-skips-following");
        surf(v, 0u, FB, C_XRGB);
        list_of(v, 2u);
        cmd(v, w0f(GFX_OP_SET_CLIP, 0u, 0u, 0u, 0u, 0u),
            0u, 0u, pair(0u, 0u), 0u, 0u, 0u, 0u);
        cmd(v, w0f(GFX_OP_FILL, GFX_EQ_COPY, 0u, 0u, 0u, 0u),
            0u, 0u, wh42, 0x00112233u, 0u, 0u, 1u);
        mem_run(v, FB, 8u, SEED);
        exp_run(v, FB, 8u, SEED);
        v->clip[0] = 0; v->clip[1] = 0; v->clip[2] = -1; v->clip[3] = -1;
        v->reads = 2;
        break;

    case 44:
        /* 新框替换旧框而不是与它求交，所以后发的大框能把先前的小框撑开 */
        reset(v, "09", "b-clip-later-can-enlarge");
        surf(v, 0u, FB, C_XRGB);
        list_of(v, 3u);
        cmd(v, w0f(GFX_OP_SET_CLIP, 0u, 0u, 0u, 0u, 0u),
            0u, 0u, pair(1u, 1u), 0u, 0u, 0u, 0u);
        cmd(v, w0f(GFX_OP_SET_CLIP, 0u, 0u, 0u, 0u, 0u),
            0u, 0u, pair(4u, 2u), 0u, 0u, 0u, 1u);
        cmd(v, w0f(GFX_OP_FILL, GFX_EQ_COPY, 0u, 0u, 0u, 0u),
            0u, 0u, wh42, 0x00112233u, 0u, 0u, 2u);
        mem_run(v, FB, 8u, SEED);
        exp_run(v, FB, 8u, 0x00112233u);
        v->clip[0] = 0; v->clip[1] = 0; v->clip[2] = 3; v->clip[3] = 1;
        v->reads = 3;
        break;

    default:
        return 0;
    }

    return 1;
}
