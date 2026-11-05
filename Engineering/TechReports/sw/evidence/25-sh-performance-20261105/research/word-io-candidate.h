/******************************************************************************
* @file rast_setup.h: CPU 几何尾段与小端 setup ABI v1（128B）
* @brief 数值契约见 doc/三维数值契约.md；不依赖 BSP，不用 C struct 传输。
******************************************************************************/
#ifndef GFX2D_GFX_RAST_SETUP_H
#define GFX2D_GFX_RAST_SETUP_H

#include <stddef.h>
#include <stdint.h>
#include "core/fixmath.h"

#define RAST_WIDTH 640
#define RAST_HEIGHT 480
#define RAST_STRIP_H 16
#define RAST_STRIPS 30
#define RAST_RECORD_BYTES 128
#define RAST_MAX_FAN 12
/* Software canvases retain the 128B record and 30 fixed-height strips.
   Width is bounded so Q12.4 origins and signed edge stepping stay representable.
   The legacy entry points always retain their 640x480 hardware contract. */
#define RAST_FRAME_MAX_WIDTH 1792u
typedef struct { unsigned width, height; } rast_frame;
int rast_frame_valid(const rast_frame *frame);
#define RAST_TEX_EN 1u
#define RAST_VCOLOR_EN 2u
#define RAST_KEY_EN 4u

#define RAST_O_FLAGS 0
#define RAST_O_E0 4
#define RAST_O_E1 16
#define RAST_O_E2 28
#define RAST_O_Z0 40
#define RAST_O_RSVD0 42
#define RAST_O_DZDX 44
#define RAST_O_DZDY 48
#define RAST_O_U0 52
#define RAST_O_V0 54
#define RAST_O_DUDX 56
#define RAST_O_DUDY 60
#define RAST_O_DVDX 64
#define RAST_O_DVDY 68
#define RAST_O_BBOX 72
#define RAST_O_COLOR 80
#define RAST_O_RSVD1 92
#define RAST_O_DRGB 96
#define RAST_O_ORIGIN 120
#define RAST_O_RSVD2 124

typedef struct { uint8_t bytes[RAST_RECORD_BYTES]; } rast_record;
/* 仅为逐字段静态证明；读写 ABI 一律走字节函数，不把这个类型映射到主表。 */
typedef struct {
    uint8_t flags[4], e0[12], e1[12], e2[12], z0[2], r0[2];
    uint8_t dzdx[4], dzdy[4], u0[2], v0[2], dudx[4], dudy[4], dvdx[4], dvdy[4];
    uint8_t bbox[8], color[12], r1[4], drgb[24], origin[4], r2[4];
} rast_layout_proof;
#define RAST_OFFSET_ASSERT(member, off) \
    _Static_assert(offsetof(rast_layout_proof, member) == off, "rast offset " #member)
RAST_OFFSET_ASSERT(flags, RAST_O_FLAGS);
RAST_OFFSET_ASSERT(e0, RAST_O_E0);
RAST_OFFSET_ASSERT(e1, RAST_O_E1);
RAST_OFFSET_ASSERT(e2, RAST_O_E2);
RAST_OFFSET_ASSERT(z0, RAST_O_Z0);
RAST_OFFSET_ASSERT(r0, RAST_O_RSVD0);
RAST_OFFSET_ASSERT(dzdx, RAST_O_DZDX);
RAST_OFFSET_ASSERT(dzdy, RAST_O_DZDY);
RAST_OFFSET_ASSERT(u0, RAST_O_U0);
RAST_OFFSET_ASSERT(v0, RAST_O_V0);
RAST_OFFSET_ASSERT(dudx, RAST_O_DUDX);
RAST_OFFSET_ASSERT(dudy, RAST_O_DUDY);
RAST_OFFSET_ASSERT(dvdx, RAST_O_DVDX);
RAST_OFFSET_ASSERT(dvdy, RAST_O_DVDY);
RAST_OFFSET_ASSERT(bbox, RAST_O_BBOX);
RAST_OFFSET_ASSERT(color, RAST_O_COLOR);
RAST_OFFSET_ASSERT(r1, RAST_O_RSVD1);
RAST_OFFSET_ASSERT(drgb, RAST_O_DRGB);
RAST_OFFSET_ASSERT(origin, RAST_O_ORIGIN);
RAST_OFFSET_ASSERT(r2, RAST_O_RSVD2);
_Static_assert(sizeof(rast_record) == 128 && sizeof(rast_layout_proof) == 128, "rast size");

static inline uint16_t rast_get16(const rast_record *r, unsigned o)
{
#if defined(__GNUC__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ && !defined(RAST_BYTE_IO)
    typedef uint16_t alias_u16 __attribute__((__may_alias__));
    const uint8_t *p=r->bytes+o;
    if(!((uintptr_t)p&1u))return *(const alias_u16 *)(const void *)p;
#endif
    return (uint16_t)(r->bytes[o] | (uint16_t)r->bytes[o + 1] * 256u);
}
static inline uint32_t rast_get32(const rast_record *r, unsigned o)
{
#if defined(__GNUC__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ && !defined(RAST_BYTE_IO)
    typedef uint32_t alias_u32 __attribute__((__may_alias__));
    const uint8_t *p=r->bytes+o;
    if(!((uintptr_t)p&3u))return *(const alias_u32 *)(const void *)p;
#endif
    return rast_get16(r, o) | (uint32_t)rast_get16(r, o + 2) * 65536u;
}
static inline void rast_put16(rast_record *r, unsigned o, uint16_t v)
{
#if defined(__GNUC__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ && !defined(RAST_BYTE_IO)
    typedef uint16_t alias_u16 __attribute__((__may_alias__));
    uint8_t *p=r->bytes+o;
    if(!((uintptr_t)p&1u)){*(alias_u16 *)(void *)p=v;return;}
#endif
    r->bytes[o] = (uint8_t)v; r->bytes[o + 1] = (uint8_t)(v >> 8);
}
static inline void rast_put32(rast_record *r, unsigned o, uint32_t v)
{
#if defined(__GNUC__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ && !defined(RAST_BYTE_IO)
    typedef uint32_t alias_u32 __attribute__((__may_alias__));
    uint8_t *p=r->bytes+o;
    if(!((uintptr_t)p&3u)){*(alias_u32 *)(void *)p=v;return;}
#endif
    rast_put16(r, o, (uint16_t)v); rast_put16(r, o + 2, (uint16_t)(v >> 16));
}

typedef struct { int32_t x, y; uint16_t z, u, v; uint32_t color; } rast_vertex;
typedef struct { fx16_t x, y, z; uint16_t u, v; uint32_t color; } rast_view_vertex;
typedef struct { uint32_t backface, sliver, clipped, emitted; } rast_setup_stats;

/* 子视口（帧内像素矩形）。映射与裁剪顺序见契约 §2.1：投影后先裁到
   [16*vx, 16*(vx+w)-1] × [16*vy, 16*(vy+h)-1]（半开区间的最大 Q12.4 格点），
   再进入既有 guard band。矩形必须整体落在帧内。 */
typedef struct { int16_t x, y; uint16_t w, h; } rast_viewport;

/* 返回记录数，负数为错误：-1 参数，-2 容量。失败不写 out。
   输入多边形须为凸多边形且保持绕序（3..8 点）。条带只裁 bbox，不重算 setup。 */
int rast_setup_polygon(const rast_vertex *v, unsigned count, uint32_t flags,
                       rast_record *out, unsigned capacity, rast_setup_stats *stats);
int rast_setup_view(const rast_view_vertex v[3], const fx16_mat4 *projection,
                    fx16_t near_z, fx16_t far_z, uint32_t flags,
                    rast_record *out, unsigned capacity, rast_setup_stats *stats);
/* 子视口入口。v 为 count(3..8) 个视空间顶点（四边形传 4）；映射：
   x_raw = 16*vx + fx16_viewport(ndc_x, w, 0)，y 同理 flip=1；
   视口矩形四边先裁（左、右、下、上），随后与旧入口同一 guard band 与 setup。
   视口外不产生记录；其余口径（舍入、破平、bbox、容量）与旧入口一致。
   投影映射结果超出 raw [-2^28,2^28] 输入域时返回 -1。 */
int rast_setup_polygon_vp(const rast_vertex *v, unsigned count, const rast_viewport *vp,
                          uint32_t flags, rast_record *out, unsigned capacity,
                          rast_setup_stats *stats);
int rast_setup_view_vp(const rast_view_vertex *v, unsigned count,
                       const fx16_mat4 *projection,
                       fx16_t near_z, fx16_t far_z, const rast_viewport *vp,
                       uint32_t flags, rast_record *out, unsigned capacity,
                       rast_setup_stats *stats);
int rast_setup_polygon_frame(const rast_vertex *v, unsigned count, const rast_viewport *vp,
                             const rast_frame *frame, uint32_t flags,
                             rast_record *out, unsigned capacity, rast_setup_stats *stats);
int rast_setup_view_frame(const rast_view_vertex *v, unsigned count,
                          const fx16_mat4 *projection, fx16_t near_z, fx16_t far_z,
                          const rast_viewport *vp, const rast_frame *frame, uint32_t flags,
                          rast_record *out, unsigned capacity, rast_setup_stats *stats);
/* S3L 默认 S3L_F=512；+z 朝前 -> 我方 -z 朝前。超出 Q16.16 返回 -1。 */
int rast_from_s3l(int32_t x, int32_t y, int32_t z, rast_view_vertex *out);
/* 子表：[u16 count,u16 0,u16 index ...] × 30，无额外对齐。返回字节数。
   先完整计容，再写出；主表保持提交顺序，空帧输出 30 个空头。 */
int rast_bin(const rast_record *records, unsigned count, uint8_t *out, size_t capacity);
int rast_record_valid(const rast_record *r);
int rast_record_valid_frame(const rast_record *r, const rast_frame *frame);
int rast_bin_frame(const rast_record *records, unsigned count, uint8_t *out,
                    size_t capacity, const rast_frame *frame);

#endif
