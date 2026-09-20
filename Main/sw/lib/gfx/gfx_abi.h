/******************************************************************************
* @file gfx_abi.h: 命令 ABI v1 的常量、记录布局与表面表
*
* @brief  这一份是软件与硬件共用的定义：命令记录的位域、表面表的三个字、
*         错误码与游戏区尺寸。它只描述数据的样子，不含任何行为，
*         所以构造层、执行器、参考实现与板上驱动都包含它。
*
*         权威是 Engineering/TechReports/spec/02-命令语义.md（规范条款 §1~§5）
*         与 spec 第 01 章；本文件是那两章的 C 镜像，任何 ABI 改动都先改正文。
*
*         表面表的内存布局与硬件的 SURF[0..15] 寄存器窗口逐位相同
*         （每项 3 个字、步长 12 字节），驱动写两边是同一份代码。
******************************************************************************/
#ifndef GFX2D_GFX_ABI_H
#define GFX2D_GFX_ABI_H

#include <stdint.h>

/* 操作码（w0[7:0]）。BAND 在 v1 是合法编码但不支持，按 BAD_OPCODE 拒绝。 */
#define GFX_OP_NOP          0u
#define GFX_OP_FILL         1u
#define GFX_OP_BLIT         2u
#define GFX_OP_SET_CLIP     3u
#define GFX_OP_BAND         4u
#define GFX_OP_END          0xFFu

/* 混合方程（w0[11:8]），系数表见设计文档 3.5 节与 spec 第 01 章 3.4 节。 */
#define GFX_EQ_COPY             0u
#define GFX_EQ_ALPHA            1u
#define GFX_EQ_ADD              2u
#define GFX_EQ_CONST            3u
#define GFX_EQ_MUL              4u
#define GFX_EQ_ALPHA_STRAIGHT   5u
#define GFX_EQ_MAX              GFX_EQ_ALPHA_STRAIGHT

/* 像素格式（表面控制字 [15:12]）。 */
#define GFX_FMT_XRGB8888        0u
#define GFX_FMT_ARGB8888_PM     1u
#define GFX_FMT_ARGB8888_ST     2u
#define GFX_FMT_INDEX8          3u

/* 地址空间（表面控制字 [9:8]）。v1 只支持 DDR。 */
#define GFX_SPACE_DDR       0u
#define GFX_SPACE_TEX       1u
#define GFX_SPACE_BAND      2u

/* 错误码，spec 第 02 章规范条款 §5 的表。 */
#define GFX_OK              0
#define GFX_E_BAD_OPCODE    1
#define GFX_E_BAD_FIELD     2
#define GFX_E_BAD_EQUATION  3
#define GFX_E_BAD_SURFACE   4
#define GFX_E_BAD_SPACE     5
#define GFX_E_BAD_FORMAT    6
#define GFX_E_OVERLAP       7
#define GFX_E_BAD_PREMULT   8
#define GFX_E_FETCH         9

/* 构造层自己的错误，取负值，与上面的 spec 错误码不会混。 */
#define GFX_BUILD_FULL      (-1)
#define GFX_BUILD_CLOSED    (-2)
#define GFX_BUILD_BAD_ARG   (-3)

/* 游戏区。裁剪的四者求交里有它一项，spec 第 01 章 3.6 节。 */
#define GFX_SCREEN_W        640
#define GFX_SCREEN_H        480

#define GFX_SURF_N          16u

/* 表面控制字里没有分配出去的位，违反时按 BAD_SURFACE 报。
   取值由已用位反推：~(0x1F | 0x300 | 0xF000 | 0x80000000) = 0x7FFF0CE0。 */
#define GFX_SURF_CTRL_RSVD  0x7FFF0CE0u

/* 一条命令固定 8 个字。记录长度是 2 的幂，硬件才能用移位算第 k 条的地址。 */
typedef struct {
    uint32_t w[8];
} gfx_cmd;

/* 表面表的一项：与硬件寄存器窗口逐位相同的三个字。
   +0x0 base；+0x4 低半字 width、高半字 height；+0x8 控制字。
   存原始字而不是解码后的结构，是为了让非法描述符也能表达
   （控制字保留位、stride_log2 越界、末像素溢出都只存在于原始字里）。 */
typedef struct {
    uint32_t base;
    uint32_t dim;
    uint32_t ctrl;
} gfx_surf_raw;

typedef struct {
    gfx_surf_raw s[GFX_SURF_N];
} gfx_surface_table;

/* 结构化的表面描述，只给合法路径的 setter 用。 */
typedef struct {
    uint32_t base;
    uint16_t width;
    uint16_t height;
    uint8_t  stride_log2;
    uint8_t  space;
    uint8_t  format;
} gfx_surface;

/* 闭区间矩形，裁剪与构造接口共用。 */
typedef struct {
    int32_t x0, y0, x1, y1;
} gfx_rect;

/* 设备地址到主机内存的窗口。
   表面的 base 是设备地址：板上可以直接解引用，PC 上不能，所以所有访存都经这里换算。
   板上把 dev_base 设成 host 所指的那个地址即为恒等映射。 */
typedef struct {
    uint32_t  dev_base;
    uint8_t  *host;
    uint32_t  bytes;
} gfx_mem;

_Static_assert(sizeof(gfx_cmd) == 32, "命令记录必须是 32 字节");
_Static_assert(sizeof(gfx_surf_raw) == 12, "表面表每项必须是 12 字节");
_Static_assert(sizeof(gfx_surface_table) == 12u * GFX_SURF_N, "表面表布局必须连续");

/* w0 的取字段。写成内联函数而不是宏，是为了带上类型。 */
static inline uint32_t gfx_op(const gfx_cmd *c)   { return  c->w[0]        & 0xFFu; }
static inline uint32_t gfx_eq(const gfx_cmd *c)   { return (c->w[0] >>  8) & 0xFu; }
static inline uint32_t gfx_key(const gfx_cmd *c)  { return (c->w[0] >> 12) & 1u; }
static inline uint32_t gfx_tint(const gfx_cmd *c) { return (c->w[0] >> 13) & 1u; }
static inline uint32_t gfx_rsvd(const gfx_cmd *c) { return (c->w[0] >> 14) & 3u; }
static inline uint32_t gfx_src(const gfx_cmd *c)  { return (c->w[0] >> 16) & 0xFu; }
static inline uint32_t gfx_dst(const gfx_cmd *c)  { return (c->w[0] >> 20) & 0xFu; }
static inline uint32_t gfx_c(const gfx_cmd *c)    { return (c->w[0] >> 24) & 0xFFu; }

/* 半字的取值。坐标是有符号，宽高是无符号，见 spec 第 01 章 3.6 节的位宽表。 */
static inline int32_t  gfx_s16(uint32_t v) { return (int32_t)(int16_t)(uint16_t)(v & 0xFFFFu); }
static inline uint32_t gfx_u16(uint32_t v) { return v & 0xFFFFu; }

/* 表面项的取字段。 */
static inline uint32_t gfx_surf_width(const gfx_surf_raw *s)  { return  s->dim        & 0xFFFFu; }
static inline uint32_t gfx_surf_height(const gfx_surf_raw *s) { return (s->dim >> 16) & 0xFFFFu; }
static inline uint32_t gfx_surf_stride_log2(const gfx_surf_raw *s) { return s->ctrl & 0x1Fu; }
static inline uint32_t gfx_surf_space(const gfx_surf_raw *s)  { return (s->ctrl >>  8) & 3u; }
static inline uint32_t gfx_surf_format(const gfx_surf_raw *s) { return (s->ctrl >> 12) & 0xFu; }
static inline uint32_t gfx_surf_valid(const gfx_surf_raw *s)  { return (s->ctrl >> 31) & 1u; }

/* 像素地址，spec 第 02 章规范条款 §2 的地址公式。 */
static inline uint32_t gfx_addr_of(const gfx_surf_raw *s, int32_t x, int32_t y)
{
    return s->base + ((uint32_t)y << gfx_surf_stride_log2(s)) + ((uint32_t)x << 2);
}

#endif /* GFX2D_GFX_ABI_H */
