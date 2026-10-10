/******************************************************************************
* @file check.c: 记录校验与表面谓词
*
* @brief  spec 第 02 章规范条款 §5 固定了十步检查顺序。第一步「取记录」是
*         执行器的有界取数，与命令表状态有关；第二到第十步只看一条记录与
*         表面表，写成纯函数放在这里，构造层与执行器共用同一份。
*
*         顺序本身就是规格的一部分：一条同时有两处错的记录只能报一个码，
*         报哪个由这里的先后决定。spec 第 02 章 4.2 节记了为什么
*         space 与 format 要各算一步、且各自按「源在前、目的在后」查。
******************************************************************************/

#include "gfx/gfx.h"
#include "core/format.h"

/* 字段编号只在本文件内用：把「必须为 0 的字段集合」做成一个掩码，
   比按名字查表省一次字符串比较，也不会漏项。 */
enum {
    F_EQ = 0, F_KEY, F_TINT, F_RSVD, F_SRC, F_DST, F_C,
    F_W1, F_W2, F_W3, F_W4, F_W5, F_W6, F_N
};

#define ZF(x) (1u << (x))

static uint32_t field_value(const gfx_cmd *c, uint32_t f)
{
    switch (f) {
    case F_EQ:   return gfx_eq(c);
    case F_KEY:  return gfx_key(c);
    case F_TINT: return gfx_tint(c);
    case F_RSVD: return gfx_rsvd(c);
    case F_SRC:  return gfx_src(c);
    case F_DST:  return gfx_dst(c);
    case F_C:    return gfx_c(c);
    case F_W1:   return c->w[1];
    case F_W2:   return c->w[2];
    case F_W3:   return c->w[3];
    case F_W4:   return c->w[4];
    case F_W5:   return c->w[5];
    default:     return c->w[6];
    }
}

/* 规范条款 §3 的字段矩阵，加上 w0[15:14]、w0[13] 两句散文。
   w3（宽高）对 FILL/BLIT/SET_CLIP 都是有效字段，所以它只在 NOP/END 的集合里。 */
static uint32_t must_zero_mask(uint32_t op, uint32_t eq, uint32_t key)
{
    uint32_t z = ZF(F_RSVD) | ZF(F_TINT);

    if (op == GFX_OP_NOP || op == GFX_OP_END) {
        z |= ZF(F_EQ) | ZF(F_KEY) | ZF(F_SRC) | ZF(F_DST) | ZF(F_C)
           | ZF(F_W1) | ZF(F_W2) | ZF(F_W3) | ZF(F_W4) | ZF(F_W5) | ZF(F_W6);
    } else if (op == GFX_OP_FILL) {
        z |= ZF(F_KEY) | ZF(F_SRC) | ZF(F_W1) | ZF(F_W5) | ZF(F_W6);
        if (eq != GFX_EQ_CONST)
            z |= ZF(F_C);
    } else if (op == GFX_OP_BLIT) {
        z |= ZF(F_W4) | ZF(F_W6);
        if (eq != GFX_EQ_CONST)
            z |= ZF(F_C);
        if (key == 0u)
            z |= ZF(F_W5);
    } else {                                    /* SET_CLIP */
        z |= ZF(F_EQ) | ZF(F_KEY) | ZF(F_SRC) | ZF(F_DST) | ZF(F_C)
           | ZF(F_W2) | ZF(F_W4) | ZF(F_W5) | ZF(F_W6);
    }
    return z;
}

static int has_bad_field(const gfx_cmd *c)
{
    uint32_t z = must_zero_mask(gfx_op(c), gfx_eq(c), gfx_key(c));
    uint32_t f;

    for (f = 0; f < (uint32_t)F_N; f++) {
        if ((z & ZF(f)) != 0u && field_value(c, f) != 0u)
            return 1;
    }
    return 0;
}

int gfx_surface_struct_ok(const gfx_surface_table *t, uint32_t id)
{
    const gfx_surf_raw *s;
    uint32_t            w, h, sl, stride;
    uint64_t            last;

    if (t == 0 || id >= GFX_SURF_N)
        return 0;
    s = &t->s[id];

    if (gfx_surf_valid(s) == 0u)
        return 0;
    if ((s->base & 3u) != 0u)
        return 0;

    w  = gfx_surf_width(s);
    h  = gfx_surf_height(s);
    sl = gfx_surf_stride_log2(s);

    if (w == 0u || h == 0u)
        return 0;
    if (sl < 2u)                                /* 高端 31 由 5 位字段本身保证 */
        return 0;

    stride = 1u << sl;
    if ((uint64_t)w * 4u > (uint64_t)stride)
        return 0;

    /* 末像素的最后一个字节不能越过 32 位地址空间。用 64 位算，
       否则这条检查自己就会溢出回绕，反而判成合法。 */
    last = (uint64_t)s->base + (uint64_t)(h - 1u) * stride
         + (uint64_t)(w - 1u) * 4u + 3u;
    if (last > 0xFFFFFFFFu)
        return 0;

    if ((s->ctrl & GFX_SURF_CTRL_RSVD) != 0u)
        return 0;

    /* v1-TEX（G1，spec 第 02 章 3.8 节）：space=1 的 base 是片上 16 KiB 窗口内的
       偏移，额外要求 16 字节对齐与 base + height × 2^stride_log2 ≤ 0x4000；
       64 位求值，完整行跨含 padding。违反按 BAD_SURFACE 报，仍在 space 步之前。 */
    if (gfx_surf_space(s) == GFX_SPACE_TEX) {
        uint64_t end;

        if ((s->base & 15u) != 0u)
            return 0;
        end = (uint64_t)s->base + (uint64_t)h * ((uint64_t)1 << sl);
        if (end > 0x4000u)
            return 0;
    }

    return 1;
}

/* BLIT 的方程与源格式匹配，规范条款 §2 末的三行。 */
static int fmt_eq_ok(uint32_t sfmt, uint32_t eq)
{
    if (eq == GFX_EQ_ALPHA)
        return sfmt == GFX_FMT_XRGB8888 || sfmt == GFX_FMT_ARGB8888_PM;
    if (eq == GFX_EQ_ALPHA_STRAIGHT)
        return sfmt == GFX_FMT_XRGB8888 || sfmt == GFX_FMT_ARGB8888_ST;
    return sfmt == GFX_FMT_XRGB8888 || sfmt == GFX_FMT_ARGB8888_PM
        || sfmt == GFX_FMT_ARGB8888_ST;
}

static int space_step(const gfx_surface_table *t, uint32_t op,
                      uint32_t srcid, uint32_t dstid)
{
    if (op == GFX_OP_FILL) {
        if (gfx_surf_space(&t->s[dstid]) != GFX_SPACE_DDR)
            return GFX_E_BAD_SPACE;
    } else if (op == GFX_OP_BLIT) {
        /* v1-TEX（G1）：BLIT 源放行 TEX，目的仍必须 DDR；BAND 与 space=3 拒绝 */
        uint32_t ssp = gfx_surf_space(&t->s[srcid]);

        if (ssp != GFX_SPACE_DDR && ssp != GFX_SPACE_TEX)
            return GFX_E_BAD_SPACE;
        if (gfx_surf_space(&t->s[dstid]) != GFX_SPACE_DDR)
            return GFX_E_BAD_SPACE;
    }
    return GFX_OK;
}

static int format_step(const gfx_surface_table *t, uint32_t op,
                       uint32_t srcid, uint32_t dstid, uint32_t eq)
{
    if (op == GFX_OP_FILL) {
        if (gfx_surf_format(&t->s[dstid]) != GFX_FMT_XRGB8888)
            return GFX_E_BAD_FORMAT;
    } else if (op == GFX_OP_BLIT) {
        if (!fmt_eq_ok(gfx_surf_format(&t->s[srcid]), eq))
            return GFX_E_BAD_FORMAT;
        if (gfx_surf_format(&t->s[dstid]) != GFX_FMT_XRGB8888)
            return GFX_E_BAD_FORMAT;
    }
    return GFX_OK;
}

/* 半开区间求交，规范条款 §4.3。 */
static int rects_intersect(int32_t ax0, int32_t ay0, int32_t ax1, int32_t ay1,
                           int32_t bx0, int32_t by0, int32_t bx1, int32_t by1)
{
    if (ax1 <= bx0 || bx1 <= ax0)
        return 0;
    if (ay1 <= by0 || by1 <= ay0)
        return 0;
    return 1;
}

int gfx_cmd_check(const gfx_surface_table *t, const gfx_cmd *c)
{
    uint32_t op, eq, srcid, dstid, w, h;
    int      bad;

    if (t == 0 || c == 0)
        return GFX_E_BAD_FIELD;

    op = gfx_op(c);

    /* 第二步：操作码。BAND 是已分配但 v1 不支持的编码，和未知码一样拒绝。 */
    if (op != GFX_OP_NOP && op != GFX_OP_FILL && op != GFX_OP_BLIT
     && op != GFX_OP_SET_CLIP && op != GFX_OP_END)
        return GFX_E_BAD_OPCODE;

    /* 第三步：保留位与未用字段。 */
    if (has_bad_field(c))
        return GFX_E_BAD_FIELD;

    eq    = gfx_eq(c);
    srcid = gfx_src(c);
    dstid = gfx_dst(c);

    /* 第四步：方程。只有 FILL 与 BLIT 能走到这里，
       NOP/END/SET_CLIP 的 eq 非零已经在第三步报了 BAD_FIELD。 */
    if ((op == GFX_OP_FILL || op == GFX_OP_BLIT) && eq > GFX_EQ_MAX)
        return GFX_E_BAD_EQUATION;

    /* 第五、六步：源表面、目的表面。零面积不跳过这里，规范条款 §4.3。 */
    if (op == GFX_OP_FILL) {
        if (!gfx_surface_struct_ok(t, dstid))
            return GFX_E_BAD_SURFACE;
    } else if (op == GFX_OP_BLIT) {
        if (!gfx_surface_struct_ok(t, srcid))
            return GFX_E_BAD_SURFACE;
        if (!gfx_surface_struct_ok(t, dstid))
            return GFX_E_BAD_SURFACE;
    }

    /* 第七、八步：space 与 format 各一步，各自源在前。 */
    bad = space_step(t, op, srcid, dstid);
    if (bad != GFX_OK)
        return bad;
    bad = format_step(t, op, srcid, dstid, eq);
    if (bad != GFX_OK)
        return bad;

    /* 第九步：ALPHA FILL 的常量必须满足预乘约束。 */
    if (op == GFX_OP_FILL && eq == GFX_EQ_ALPHA) {
        uint32_t a = (c->w[4] >> 24) & 0xFFu;

        if (((c->w[4] >> 16) & 0xFFu) > a
         || ((c->w[4] >>  8) & 0xFFu) > a
         || ( c->w[4]        & 0xFFu) > a)
            return GFX_E_BAD_PREMULT;
    }

    /* 第十步：同一表面号上的 BLIT 矩形不得重叠。用未裁剪的半开区间判，
       所以它在屏幕与 SET_CLIP 裁剪之前。 */
    if (op == GFX_OP_BLIT && srcid == dstid) {
        w = gfx_u16(c->w[3]);
        h = gfx_u16(c->w[3] >> 16);
        if (w > 0u && h > 0u) {
            int32_t sx = gfx_s16(c->w[1]);
            int32_t sy = gfx_s16(c->w[1] >> 16);
            int32_t dx = gfx_s16(c->w[2]);
            int32_t dy = gfx_s16(c->w[2] >> 16);

            if (rects_intersect(sx, sy, sx + (int32_t)w, sy + (int32_t)h,
                                dx, dy, dx + (int32_t)w, dy + (int32_t)h))
                return GFX_E_OVERLAP;
        }
    }

    return GFX_OK;
}

/* 坐标是有符号的，core_out_u32 只能打无符号，所以符号在这里出。 */
static void out_i32(core_out *o, int32_t v)
{
    if (v < 0) {
        core_out_ch(o, '-');
        core_out_u32(o, (uint32_t)(-(int64_t)v));
    } else {
        core_out_u32(o, (uint32_t)v);
    }
}

static const char *op_name(uint32_t op)
{
    switch (op) {
    case GFX_OP_NOP:      return "NOP";
    case GFX_OP_FILL:     return "FILL";
    case GFX_OP_BLIT:     return "BLIT";
    case GFX_OP_BLIT_INST:return "BLIT_INST";
    case GFX_OP_SET_CLIP: return "SET_CLIP";
    case GFX_OP_BAND:     return "BAND";
    case GFX_OP_END:      return "END";
    default:              return "?";
    }
}

static const char *eq_name(uint32_t eq)
{
    switch (eq) {
    case GFX_EQ_COPY:           return "COPY";
    case GFX_EQ_ALPHA:          return "ALPHA";
    case GFX_EQ_ADD:            return "ADD";
    case GFX_EQ_CONST:          return "CONST";
    case GFX_EQ_MUL:            return "MUL";
    case GFX_EQ_ALPHA_STRAIGHT: return "ALPHA_ST";
    default:                    return "?";
    }
}

uint32_t gfx_cmd_decode(const gfx_cmd *c, char *buf, uint32_t cap)
{
    core_out o;

    core_out_init(&o, buf, cap);
    if (c == 0) {
        core_out_str(&o, "(null)");
        return core_out_len(&o);
    }

    core_out_str(&o, op_name(gfx_op(c)));

    if (gfx_op(c) == GFX_OP_FILL || gfx_op(c) == GFX_OP_BLIT) {
        core_out_str(&o, " eq=");
        core_out_str(&o, eq_name(gfx_eq(c)));
        core_out_str(&o, " dst=");
        core_out_u32(&o, gfx_dst(c));
        if (gfx_op(c) == GFX_OP_BLIT) {
            core_out_str(&o, " src=");
            core_out_u32(&o, gfx_src(c));
            core_out_str(&o, " s=");
            out_i32(&o, gfx_s16(c->w[1]));
            core_out_str(&o, ",");
            out_i32(&o, gfx_s16(c->w[1] >> 16));
        }
        core_out_str(&o, " d=");
        out_i32(&o, gfx_s16(c->w[2]));
        core_out_str(&o, ",");
        out_i32(&o, gfx_s16(c->w[2] >> 16));
        core_out_str(&o, " wh=");
        core_out_u32(&o, gfx_u16(c->w[3]));
        core_out_str(&o, "x");
        core_out_u32(&o, gfx_u16(c->w[3] >> 16));
        if (gfx_op(c) == GFX_OP_FILL) {
            core_out_str(&o, " w4=");
            core_out_hex32(&o, c->w[4]);
        }
        if (gfx_key(c) != 0u) {
            core_out_str(&o, " key=");
            core_out_hex32(&o, c->w[5]);
        }
        if (gfx_eq(c) == GFX_EQ_CONST) {
            core_out_str(&o, " c=");
            core_out_u32(&o, gfx_c(c));
        }
    } else if (gfx_op(c) == GFX_OP_BLIT_INST) {
        core_out_str(&o," base=");core_out_hex32(&o,c->w[1]);
        core_out_str(&o," count=");core_out_u32(&o,c->w[2]);
        core_out_str(&o," layout=");core_out_u32(&o,(c->w[5]>>16)&3u);
        core_out_str(&o," stride=");core_out_u32(&o,c->w[5]&65535u);
        core_out_str(&o," fixed=");out_i32(&o,gfx_s16(c->w[6]));
        core_out_str(&o,",");out_i32(&o,gfx_s16(c->w[6]>>16));
        core_out_str(&o," wh=");core_out_u32(&o,gfx_u16(c->w[3]));
        core_out_str(&o,"x");core_out_u32(&o,gfx_u16(c->w[3]>>16));
        core_out_str(&o," src=");core_out_u32(&o,gfx_src(c));
        core_out_str(&o," dst=");core_out_u32(&o,gfx_dst(c));
        core_out_str(&o," eq=");core_out_str(&o,eq_name(gfx_eq(c)));
        if(gfx_key(c)){core_out_str(&o," key=");core_out_hex32(&o,c->w[4]);}
        if(gfx_eq(c)==GFX_EQ_CONST){core_out_str(&o," c=");core_out_u32(&o,gfx_c(c));}
    } else if (gfx_op(c) == GFX_OP_SET_CLIP) {
        core_out_str(&o, " at=");
        out_i32(&o, gfx_s16(c->w[1]));
        core_out_str(&o, ",");
        out_i32(&o, gfx_s16(c->w[1] >> 16));
        core_out_str(&o, " wh=");
        core_out_u32(&o, gfx_u16(c->w[3]));
        core_out_str(&o, "x");
        core_out_u32(&o, gfx_u16(c->w[3] >> 16));
    }

    core_out_str(&o, " tag=");
    core_out_hex32(&o, c->w[7]);
    return core_out_len(&o);
}
