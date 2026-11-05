/******************************************************************************
* @file rast_setup.c: 近/远面裁剪、投影、guard band、setup 与保序条带分桶
* @brief 全部插值最近舍入（半值向 +∞）。屏幕坐标保留 Q12.4 亚像素。
******************************************************************************/
#include "gfx/rast_setup.h"
#include <limits.h>
#include <string.h>

static const rast_frame default_frame = {RAST_WIDTH, RAST_HEIGHT};

int rast_frame_valid(const rast_frame *frame)
{
    return frame && frame->width && frame->width <= RAST_FRAME_MAX_WIDTH &&
           frame->height && frame->height <= RAST_HEIGHT;
}

static int32_t mix(int32_t a, int32_t b, int32_t ca, int32_t cb, int32_t plane)
{
    int64_t den = (int64_t)cb - ca;
    int64_t num = (int64_t)a * ((int64_t)cb - plane) + (int64_t)b * ((int64_t)plane - ca);
    if (den < 0) { den = -den; num = -num; }
    return (int32_t)fx16_round_div(num, den);
}

static uint32_t mix_color(uint32_t a, uint32_t b, int32_t ca, int32_t cb, int32_t p)
{
    uint32_t c = 0;
    for (unsigned s = 0; s < 32; s += 8)
        c |= (uint32_t)mix((a >> s) & 255u, (b >> s) & 255u, ca, cb, p) << s;
    return c;
}

static rast_vertex screen_cross(rast_vertex a, rast_vertex b, int axis, int32_t plane)
{
    int32_t ca = axis ? a.y : a.x, cb = axis ? b.y : b.x;
    rast_vertex r;
    r.x = mix(a.x, b.x, ca, cb, plane); r.y = mix(a.y, b.y, ca, cb, plane);
    r.z = (uint16_t)mix(a.z, b.z, ca, cb, plane);
    r.u = (uint16_t)mix(a.u, b.u, ca, cb, plane);
    r.v = (uint16_t)mix(a.v, b.v, ca, cb, plane);
    r.color = mix_color(a.color, b.color, ca, cb, plane);
    if (axis) r.y = plane; else r.x = plane;
    return r;
}

static unsigned clip_screen(const rast_vertex *in, unsigned n, rast_vertex *out,
                             int axis, int32_t plane, int upper)
{
    unsigned m = 0;
    if (!n) return 0;
    rast_vertex a = in[n - 1];
    int ai = upper ? (axis ? a.y : a.x) <= plane : (axis ? a.y : a.x) >= plane;
    for (unsigned i = 0; i < n; ++i) {
        rast_vertex b = in[i];
        int bi = upper ? (axis ? b.y : b.x) <= plane : (axis ? b.y : b.x) >= plane;
        if (ai != bi) out[m++] = screen_cross(a, b, axis, plane);
        if (bi) out[m++] = b;
        a = b; ai = bi;
    }
    return m;
}

static int gradient(rast_record *r, unsigned off, const rast_vertex *v,
                     int64_t a0, int64_t a1, int64_t a2, int64_t area)
{
    if (a0 == a1 && a0 == a2) {
        rast_put32(r, off, 0); rast_put32(r, off + 4, 0);
        return 1;
    }
    int64_t dx = fx16_round_div(((a1 - a0) * (v[2].y - v[0].y) -
                                 (a2 - a0) * (v[1].y - v[0].y)) * 16, area);
    int64_t dy = fx16_round_div(((v[1].x - v[0].x) * (a2 - a0) -
                                 (v[2].x - v[0].x) * (a1 - a0)) * 16, area);
    if (dx <= -(INT64_C(1) << 30) || dx >= (INT64_C(1) << 30) ||
        dy <= -(INT64_C(1) << 30) || dy >= (INT64_C(1) << 30)) return 0;
    rast_put32(r, off, (uint32_t)dx); rast_put32(r, off + 4, (uint32_t)dy);
    return 1;
}

static int setup_one(const rast_vertex v[3], uint32_t flags, rast_record *r,
                      rast_setup_stats *stats, const rast_frame *frame)
{
    int64_t area = ((int64_t)v[1].x - v[0].x) * ((int64_t)v[2].y - v[0].y) -
                   ((int64_t)v[2].x - v[0].x) * ((int64_t)v[1].y - v[0].y);
    int32_t lo[2] = {v[0].x, v[0].y}, hi[2] = {v[0].x, v[0].y};
    if (area <= 0) { ++stats->backface; return 0; }
    memset(r, 0, sizeof(*r));
    rast_put32(r, RAST_O_FLAGS, flags);
    for (unsigned k = 0; k < 3; ++k) {
        const rast_vertex *a = &v[k], *b = &v[(k + 1) % 3];
        int64_t dx = (int64_t)b->x - a->x, dy = (int64_t)b->y - a->y;
        int64_t c = dy * a->x - dx * a->y;
        /* guard band 后的边系数必须可完整表达；变异禁用裁剪时在这里见红。 */
        if (c < INT32_MIN || c > INT32_MAX || dx * 16 < INT32_MIN ||
            dx * 16 > INT32_MAX || dy * 16 < INT32_MIN || dy * 16 > INT32_MAX)
            return -1;
        rast_put32(r, 4 + k * 12, (uint32_t)(-dy * 16));
        rast_put32(r, 8 + k * 12, (uint32_t)(dx * 16));
        rast_put32(r, 12 + k * 12, (uint32_t)c);
        if (a->x < lo[0]) lo[0] = a->x;
        if (a->x > hi[0]) hi[0] = a->x;
        if (a->y < lo[1]) lo[1] = a->y;
        if (a->y > hi[1]) hi[1] = a->y;
    }
    for (unsigned k = 0; k < 2; ++k) {
        int32_t max = (int32_t)(k ? frame->height : frame->width) - 1;
        lo[k] = (int32_t)-fx16_floor_div(-(int64_t)lo[k], 16);
        hi[k] = (int32_t)fx16_floor_div(hi[k], 16);
        if (lo[k] < 0) lo[k] = 0;
        if (hi[k] > max) hi[k] = max;
        if (lo[k] > hi[k]) return 0;
        rast_put16(r, RAST_O_BBOX + k * 4, (uint16_t)lo[k]);
        rast_put16(r, RAST_O_BBOX + k * 4 + 2, (uint16_t)hi[k]);
    }
    rast_put16(r, RAST_O_Z0, v[0].z);
    rast_put16(r, RAST_O_U0, v[0].u); rast_put16(r, RAST_O_V0, v[0].v);
    rast_put16(r, RAST_O_ORIGIN, (uint16_t)v[0].x);
    rast_put16(r, RAST_O_ORIGIN + 2, (uint16_t)v[0].y);
    if (!gradient(r, RAST_O_DZDX, v, (int64_t)v[0].z * 65536,
                  (int64_t)v[1].z * 65536, (int64_t)v[2].z * 65536, area) ||
        !gradient(r, RAST_O_DUDX, v, (int64_t)v[0].u * 4096,
                  (int64_t)v[1].u * 4096, (int64_t)v[2].u * 4096, area) ||
        !gradient(r, RAST_O_DVDX, v, (int64_t)v[0].v * 4096,
                  (int64_t)v[1].v * 4096, (int64_t)v[2].v * 4096, area)) {
        ++stats->sliver; return 0;
    }
    for (unsigned k = 0; k < 3; ++k) {
        unsigned shift = 16 - k * 8;
        if (!gradient(r, RAST_O_DRGB + k * 8, v,
            (int64_t)((v[0].color >> shift) & 255u) * 65536,
            (int64_t)((v[1].color >> shift) & 255u) * 65536,
            (int64_t)((v[2].color >> shift) & 255u) * 65536, area)) {
            ++stats->sliver; return 0;
        }
        rast_put32(r, RAST_O_COLOR + k * 4, v[k].color);
    }
    return 1;
}

/* 扇形拆分与逐三角形 setup；n 已是裁剪后的顶点数。返回记录数，负数为内部错误。 */
static int emit_fan(const rast_vertex *a, unsigned n, uint32_t flags,
                    rast_record *records, rast_setup_stats *delta, const rast_frame *frame)
{
    unsigned produced = 0;
    for (unsigned i = 1; i + 1 < n; ++i) {
        rast_vertex tri[3] = {a[0], a[i], a[i + 1]};
        int result = setup_one(tri, flags, &records[produced], delta, frame);
        if (result < 0) return -1;
        produced += (unsigned)result;
    }
    return (int)produced;
}

static int setup_domain_ok(const rast_vertex *v, unsigned count)
{
    if (!v || count < 3 || count > 8) return 0;
    for (unsigned i = 0; i < count; ++i)
        if (v[i].x < -(1 << 28) || v[i].x > (1 << 28) ||
            v[i].y < -(1 << 28) || v[i].y > (1 << 28)) return 0;
    return 1;
}

int rast_setup_polygon(const rast_vertex *v, unsigned count, uint32_t flags,
                       rast_record *out, unsigned capacity, rast_setup_stats *stats)
{
    rast_vertex a[16], b[16];
    rast_record records[RAST_MAX_FAN];
    rast_setup_stats delta = {0};
    unsigned n = count;
    int produced;
    if (!setup_domain_ok(v, count) || !out || flags & ~7u) return -1;
    memcpy(a, v, count * sizeof(*v));
#ifndef INJECT_RAST_NO_GUARD
    const int32_t plane[4] = {-4096, 14335, -4096, 11775};
    for (unsigned p = 0; p < 4; ++p) {
        n = clip_screen(a, n, b, p / 2, plane[p], p & 1);
        memcpy(a, b, n * sizeof(*a));
    }
#else
    (void)b; (void)clip_screen;
#endif
    delta.clipped = n != count;
    if (n == count)
        for (unsigned i = 0; i < n; ++i)
            if (a[i].x != v[i].x || a[i].y != v[i].y) delta.clipped = 1;
    produced = emit_fan(a, n, flags, records, &delta, &default_frame);
    if (produced < 0) return -1;
    if ((unsigned)produced > capacity) return -2;
    memcpy(out, records, (unsigned)produced * sizeof(*out));
    delta.emitted = (unsigned)produced;
    if (stats) {
        stats->backface += delta.backface; stats->sliver += delta.sliver;
        stats->clipped += delta.clipped; stats->emitted += delta.emitted;
    }
    return produced;
}

static int setup_polygon_vp(const rast_vertex *v, unsigned count, const rast_viewport *vp,
                          uint32_t flags, rast_record *out, unsigned capacity,
                          rast_setup_stats *stats, unsigned input_limit, const rast_frame *frame)
{
    rast_vertex a[16], b[16];
    rast_record records[RAST_MAX_FAN];
    rast_setup_stats delta = {0};
    unsigned n = count;
    int produced;
    if (!v || count<3u || count>input_limit || !out || !vp || flags & ~7u ||
        !rast_frame_valid(frame)) return -1;
    for(unsigned i=0;i<count;++i)
        if(v[i].x<-(1<<28)||v[i].x>(1<<28)||v[i].y<-(1<<28)||v[i].y>(1<<28))return -1;
    if (vp->x < 0 || vp->y < 0 || vp->w == 0u || vp->h == 0u ||
        vp->x + (int32_t)vp->w > (int32_t)frame->width ||
        vp->y + (int32_t)vp->h > (int32_t)frame->height)
        return -1;
#ifndef RAST_DISABLE_INSIDE_FAST
    if(count==3u) {
        int inside=1;
        for(unsigned i=0;i<3;++i)
            if(v[i].x<(int32_t)vp->x*16 || v[i].x>=(int32_t)(vp->x+vp->w)*16 ||
               v[i].y<(int32_t)vp->y*16 || v[i].y>=(int32_t)(vp->y+vp->h)*16)inside=0;
        if(inside) {
            /* Clipping against both rectangles is an identity for this case.
               Keep setup's local record and commit/capacity ordering intact. */
            produced=setup_one(v,flags,records,&delta,frame);
            if(produced<0)return -1;
            if((unsigned)produced>capacity)return -2;
            if(produced)memcpy(out,records,sizeof(*out));
            if(stats){stats->backface+=delta.backface;stats->sliver+=delta.sliver;
                stats->emitted+=(unsigned)produced;}
            return produced;
        }
    }
#endif
    memcpy(a, v, count * sizeof(*v));
    {
        /* 半开区间的最大 Q12.4 格点；平面顺序与 guard band 相同（左、右、下、上）。
           矩形在帧内，裁完必在 guard band 内侧，后面的 guard 裁剪不会再产生交点。 */
        const int32_t plane[4] = {(int32_t)vp->x * 16, (int32_t)(vp->x + vp->w) * 16 - 1,
                                  (int32_t)vp->y * 16, (int32_t)(vp->y + vp->h) * 16 - 1};
        for (unsigned p = 0; p < 4 && n; ++p) {
            n = clip_screen(a, n, b, p / 2, plane[p], p & 1);
            memcpy(a, b, n * sizeof(*a));
        }
    }
#ifndef INJECT_RAST_NO_GUARD
    {
        const int32_t plane[4] = {-4096, (int32_t)(frame->width + 256u)*16 - 1,
                                 -4096, (int32_t)(frame->height + 256u)*16 - 1};
        for (unsigned p = 0; p < 4 && n; ++p) {
            n = clip_screen(a, n, b, p / 2, plane[p], p & 1);
            memcpy(a, b, n * sizeof(*a));
        }
    }
#endif
    delta.clipped = n != count;
    if (n == count)
        for (unsigned i = 0; i < n; ++i)
            if (a[i].x != v[i].x || a[i].y != v[i].y) delta.clipped = 1;
    produced = emit_fan(a, n, flags, records, &delta, frame);
    if (produced < 0) return -1;
    if ((unsigned)produced > capacity) return -2;
    memcpy(out, records, (unsigned)produced * sizeof(*out));
    delta.emitted = (unsigned)produced;
    if (stats) {
        stats->backface += delta.backface; stats->sliver += delta.sliver;
        stats->clipped += delta.clipped; stats->emitted += delta.emitted;
    }
    return produced;
}

int rast_setup_polygon_vp(const rast_vertex *v, unsigned count, const rast_viewport *vp,
                          uint32_t flags, rast_record *out, unsigned capacity,
                          rast_setup_stats *stats)
{
    return setup_polygon_vp(v,count,vp,flags,out,capacity,stats,8u,&default_frame);
}

int rast_setup_polygon_frame(const rast_vertex *v, unsigned count, const rast_viewport *vp,
                             const rast_frame *frame, uint32_t flags,
                             rast_record *out, unsigned capacity, rast_setup_stats *stats)
{
    return setup_polygon_vp(v,count,vp,flags,out,capacity,stats,8u,frame);
}

static rast_view_vertex view_cross(rast_view_vertex a, rast_view_vertex b, int32_t p)
{
    rast_view_vertex r;
    r.x = mix(a.x, b.x, a.z, b.z, p); r.y = mix(a.y, b.y, a.z, b.z, p); r.z = p;
    r.u = (uint16_t)mix(a.u, b.u, a.z, b.z, p);
    r.v = (uint16_t)mix(a.v, b.v, a.z, b.z, p);
    r.color = mix_color(a.color, b.color, a.z, b.z, p);
    return r;
}

/* 近/远面裁剪 + 投影映射。vx/vy/vw/vh 是视口（像素）；旧入口传整帧。
   返回裁剪后的顶点数（<3 表示整棵被裁掉），投影映射越出 [-2^28,2^28] 返回 -1。 */
static int view_project(const rast_view_vertex *v, unsigned count,
                        const fx16_mat4 *projection,
                        fx16_t near_z, fx16_t far_z,
                        int32_t vx, int32_t vy, int32_t vw, int32_t vh,
                        rast_vertex screen[16])
{
    rast_view_vertex a[16], b[16];
    unsigned n = count;
    if (count < 3 || count > 8) return -1;
    memcpy(a, v, sizeof(*v) * count);
    for (unsigned p = 0; p < 2; ++p) {
        unsigned m = 0;
        int32_t plane = p ? -far_z : -near_z;
        if (!n) break;
        rast_view_vertex prev = a[n - 1];
        int pi = p ? prev.z >= plane : prev.z <= plane;
        for (unsigned i = 0; i < n; ++i) {
            int inside = p ? a[i].z >= plane : a[i].z <= plane;
            if (pi != inside) b[m++] = view_cross(prev, a[i], plane);
            if (inside) b[m++] = a[i];
            prev = a[i]; pi = inside;
        }
        n = m; memcpy(a, b, n * sizeof(*a));
    }
    if (n < 3) return (int)n;
    for (unsigned i = 0; i < n; ++i) {
        fx16_vec4 in = {{a[i].x, a[i].y, a[i].z, FX16_ONE}}, clip;
        int64_t x, y;
        fx16_mat_vec(projection, &in, &clip);
        if (clip.v[3] <= 0) return -1;
        x = (int64_t)vx * 16 + fx16_viewport(fx16_div(clip.v[0], clip.v[3]), vw, 0);
        y = (int64_t)vy * 16 + fx16_viewport(fx16_div(clip.v[1], clip.v[3]), vh, 1);
        if (x < -(1 << 28) || x > (1 << 28) || y < -(1 << 28) || y > (1 << 28)) return -1;
        screen[i].x = (int32_t)x;
        screen[i].y = (int32_t)y;
        {
            int64_t z = fx16_round_div(((int64_t)fx16_div(clip.v[2], clip.v[3]) + FX16_ONE) * 65535,
                                       2 * FX16_ONE);
            screen[i].z = (uint16_t)(z < 0 ? 0 : z > 65535 ? 65535 : z);
        }
        screen[i].u = a[i].u; screen[i].v = a[i].v; screen[i].color = a[i].color;
    }
    return (int)n;
}

int rast_setup_view(const rast_view_vertex v[3], const fx16_mat4 *projection,
                    fx16_t near_z, fx16_t far_z, uint32_t flags,
                    rast_record *out, unsigned capacity, rast_setup_stats *stats)
{
    rast_vertex screen[16];
    if (!v || !out || !projection || flags & ~7u || near_z <= 0 || far_z <= near_z) return -1;
    {
        int n = view_project(v, 3u, projection, near_z, far_z,
                             0, 0, RAST_WIDTH, RAST_HEIGHT, screen);
        if (n < 0) return -1;
        if (n < 3) return 0;
        return rast_setup_polygon(screen, (unsigned)n, flags, out, capacity, stats);
    }
}

int rast_setup_view_vp(const rast_view_vertex *v, unsigned count,
                       const fx16_mat4 *projection,
                       fx16_t near_z, fx16_t far_z, const rast_viewport *vp,
                       uint32_t flags, rast_record *out, unsigned capacity,
                       rast_setup_stats *stats)
{
    return rast_setup_view_frame(v,count,projection,near_z,far_z,vp,&default_frame,
                                 flags,out,capacity,stats);
}

int rast_setup_view_frame(const rast_view_vertex *v, unsigned count,
                          const fx16_mat4 *projection, fx16_t near_z, fx16_t far_z,
                          const rast_viewport *vp, const rast_frame *frame, uint32_t flags,
                          rast_record *out, unsigned capacity, rast_setup_stats *stats)
{
    rast_vertex screen[16];
    if (!v || !out || !projection || !vp || flags & ~7u || !rast_frame_valid(frame) ||
        near_z <= 0 || far_z <= near_z) return -1;
    if (vp->x < 0 || vp->y < 0 || vp->w == 0u || vp->h == 0u ||
        vp->x + (int32_t)vp->w > (int32_t)frame->width ||
        vp->y + (int32_t)vp->h > (int32_t)frame->height)
        return -1;
    {
        int n = view_project(v, count, projection, near_z, far_z,
                             vp->x, vp->y, vp->w, vp->h, screen);
        if (n < 0) return -1;
        if (n < 3) return 0;
        return setup_polygon_vp(screen, (unsigned)n, vp, flags, out, capacity, stats,10u,frame);
    }
}

int rast_from_s3l(int32_t x, int32_t y, int32_t z, rast_view_vertex *out)
{
    int64_t a = (int64_t)x * 128, b = (int64_t)y * 128, c = -(int64_t)z * 128;
    if (!out || a < INT32_MIN || a > INT32_MAX || b < INT32_MIN || b > INT32_MAX ||
        c < INT32_MIN || c > INT32_MAX) return -1;
    out->x = (int32_t)a; out->y = (int32_t)b; out->z = (int32_t)c;
    return 0;
}

int rast_record_valid(const rast_record *r)
{
    return rast_record_valid_frame(r,&default_frame);
}

int rast_record_valid_frame(const rast_record *r, const rast_frame *frame)
{
    if (!rast_frame_valid(frame) || !r || rast_get32(r, 0) & ~7u || rast_get16(r, 42) || rast_get32(r, 92) ||
        rast_get32(r, 124)) return 0;
    for (unsigned k = 0; k < 2; ++k) {
        int32_t lo = (int16_t)rast_get16(r, 72 + k * 4);
        int32_t hi = (int16_t)rast_get16(r, 74 + k * 4);
        if (lo < 0 || lo > hi || hi >= (int32_t)(k ? frame->height : frame->width)) return 0;
    }
    int32_t ox = (int16_t)rast_get16(r, 120), oy = (int16_t)rast_get16(r, 122);
    if (ox < -4096 || ox > (int32_t)(frame->width+256u)*16-1 ||
        oy < -4096 || oy > (int32_t)(frame->height+256u)*16-1) return 0;
    const unsigned offsets[6] = {44, 56, 64, 96, 104, 112};
    for (unsigned k = 0; k < 6; ++k)
        for (unsigned j = 0; j < 2; ++j) {
            int32_t g = (int32_t)rast_get32(r, offsets[k] + j * 4);
            if (g <= -(1 << 30) || g >= (1 << 30)) return 0;
        }
    for (unsigned k = 0; k < 3; ++k) {
        int64_t a = (int32_t)rast_get32(r, 4 + k * 12);
        int64_t b = (int32_t)rast_get32(r, 8 + k * 12);
        int64_t c = (int32_t)rast_get32(r, 12 + k * 12);
        if (a % 16 || b % 16 || a < -524288 || a > 524288 ||
            b < -524288 || b > 524288 || c < -(INT64_C(1) << 29) || c > (INT64_C(1) << 29)) return 0;
    }
    return 1;
}

int rast_bin(const rast_record *records, unsigned count, uint8_t *out, size_t capacity)
{
    return rast_bin_frame(records,count,out,capacity,&default_frame);
}

int rast_bin_frame(const rast_record *records, unsigned count, uint8_t *out,
                    size_t capacity, const rast_frame *frame)
{
    uint16_t counts[RAST_STRIPS] = {0};
    size_t next[RAST_STRIPS];
    size_t need = RAST_STRIPS * 4, cursor = 0;
    if (!rast_frame_valid(frame) || !out || count > 65535 || (count && !records)) return -1;
    for (unsigned i = 0; i < count; ++i) {
        if (!rast_record_valid_frame(&records[i],frame)) return -1;
        unsigned first = rast_get16(&records[i], 76) / RAST_STRIP_H;
        unsigned last = rast_get16(&records[i], 78) / RAST_STRIP_H;
        for (unsigned s = first; s <= last; ++s) { ++counts[s]; need += 2; }
    }
    if (need > capacity) return -2;
    for (unsigned s = 0; s < RAST_STRIPS; ++s) {
        out[cursor++] = (uint8_t)counts[s]; out[cursor++] = (uint8_t)(counts[s] >> 8);
        out[cursor++] = 0; out[cursor++] = 0;
        next[s] = cursor; cursor += (size_t)counts[s] * 2;
    }
    /* Prefix each bucket once, then append in input order. The byte stream is
       identical to the old strip-major scan, without reading every record 30x. */
    for (unsigned i = 0; i < count; ++i) {
        unsigned first = rast_get16(&records[i], 76) / RAST_STRIP_H;
        unsigned last = rast_get16(&records[i], 78) / RAST_STRIP_H;
        for (unsigned s = first; s <= last; ++s) {
            size_t at = next[s]; next[s] += 2;
            out[at] = (uint8_t)i; out[at + 1] = (uint8_t)(i >> 8);
        }
    }
    return (int)need;
}

int rast_bin_frame_cache(const rast_record *records, unsigned count, uint8_t *ranges,
                         uint8_t *out, size_t capacity, const rast_frame *frame)
{
    uint16_t counts[RAST_STRIPS] = {0};
    size_t next[RAST_STRIPS];
    size_t need = RAST_STRIPS * 4, cursor = 0;
    if (!rast_frame_valid(frame) || !out || count > 65535 || (count && !records) ||
        (count && !ranges)) return -1;
    /* Pass 1: fresh entries (0xFF) validate the record body and fill their
       range in place; cached entries are trusted and never re-read. */
    for (unsigned i = 0; i < count; ++i) {
        uint8_t *range = &ranges[i * 2u];
        uint8_t first, last;
        if (range[0] == 0xFFu) {
            if (!rast_record_valid_frame(&records[i],frame)) return -1;
            first = (uint8_t)(rast_get16(&records[i], 76) / RAST_STRIP_H);
            last = (uint8_t)(rast_get16(&records[i], 78) / RAST_STRIP_H);
            range[0] = first; range[1] = last;
        } else {
            first = range[0]; last = range[1];
        }
        if (last >= RAST_STRIPS || first > last) return -1;
        for (unsigned s = first; s <= last; ++s) { ++counts[s]; need += 2; }
    }
    if (need > capacity) return -2;
    for (unsigned s = 0; s < RAST_STRIPS; ++s) {
        out[cursor++] = (uint8_t)counts[s]; out[cursor++] = (uint8_t)(counts[s] >> 8);
        out[cursor++] = 0; out[cursor++] = 0;
        next[s] = cursor; cursor += (size_t)counts[s] * 2;
    }
    for (unsigned i = 0; i < count; ++i) {
        for (unsigned s = ranges[i * 2u]; s <= ranges[i * 2u + 1u]; ++s) {
            size_t at = next[s]; next[s] += 2;
            out[at] = (uint8_t)i; out[at + 1] = (uint8_t)(i >> 8);
        }
    }
    return (int)need;
}
