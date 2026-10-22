/******************************************************************************
* @file rast_setup.c: 近/远面裁剪、投影、guard band、setup 与保序条带分桶
* @brief 全部插值最近舍入（半值向 +∞）。屏幕坐标保留 Q12.4 亚像素。
******************************************************************************/
#include "gfx/rast_setup.h"
#include <limits.h>
#include <string.h>

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
                      rast_setup_stats *stats)
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
        int32_t max = k ? RAST_HEIGHT - 1 : RAST_WIDTH - 1;
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

int rast_setup_polygon(const rast_vertex *v, unsigned count, uint32_t flags,
                       rast_record *out, unsigned capacity, rast_setup_stats *stats)
{
    rast_vertex a[16], b[16];
    rast_record records[RAST_MAX_FAN];
    rast_setup_stats delta = {0};
    unsigned n = count, produced = 0;
    if (!v || !out || count < 3 || count > 8 || flags & ~7u) return -1;
    for (unsigned i = 0; i < count; ++i)
        if (v[i].x < -(1 << 28) || v[i].x > (1 << 28) ||
            v[i].y < -(1 << 28) || v[i].y > (1 << 28)) return -1;
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
    for (unsigned i = 1; i + 1 < n; ++i) {
        rast_vertex tri[3] = {a[0], a[i], a[i + 1]};
        int result = setup_one(tri, flags, &records[produced], &delta);
        if (result < 0) return -1;
        produced += (unsigned)result;
    }
    if (produced > capacity) return -2;
    memcpy(out, records, produced * sizeof(*out));
    delta.emitted = produced;
    if (stats) {
        stats->backface += delta.backface; stats->sliver += delta.sliver;
        stats->clipped += delta.clipped; stats->emitted += delta.emitted;
    }
    return (int)produced;
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

int rast_setup_view(const rast_view_vertex v[3], const fx16_mat4 *projection,
                    fx16_t near_z, fx16_t far_z, uint32_t flags,
                    rast_record *out, unsigned capacity, rast_setup_stats *stats)
{
    rast_view_vertex a[8], b[8];
    rast_vertex screen[8];
    unsigned n = 3;
    if (!v || !out || !projection || flags & ~7u || near_z <= 0 || far_z <= near_z) return -1;
    memcpy(a, v, sizeof(*v) * 3);
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
    if (n < 3) return 0;
    for (unsigned i = 0; i < n; ++i) {
        fx16_vec4 in = {{a[i].x, a[i].y, a[i].z, FX16_ONE}}, clip;
        fx16_mat_vec(projection, &in, &clip);
        if (clip.v[3] <= 0) return -1;
        screen[i].x = fx16_viewport(fx16_div(clip.v[0], clip.v[3]), RAST_WIDTH, 0);
        screen[i].y = fx16_viewport(fx16_div(clip.v[1], clip.v[3]), RAST_HEIGHT, 1);
        int64_t z = fx16_round_div(((int64_t)fx16_div(clip.v[2], clip.v[3]) + FX16_ONE) * 65535,
                                  2 * FX16_ONE);
        screen[i].z = (uint16_t)(z < 0 ? 0 : z > 65535 ? 65535 : z);
        screen[i].u = a[i].u; screen[i].v = a[i].v; screen[i].color = a[i].color;
    }
    return rast_setup_polygon(screen, n, flags, out, capacity, stats);
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
    if (!r || rast_get32(r, 0) & ~7u || rast_get16(r, 42) || rast_get32(r, 92) ||
        rast_get32(r, 124)) return 0;
    for (unsigned k = 0; k < 2; ++k) {
        int32_t lo = (int16_t)rast_get16(r, 72 + k * 4);
        int32_t hi = (int16_t)rast_get16(r, 74 + k * 4);
        if (lo < 0 || lo > hi || hi >= (k ? RAST_HEIGHT : RAST_WIDTH)) return 0;
    }
    int32_t ox = (int16_t)rast_get16(r, 120), oy = (int16_t)rast_get16(r, 122);
    if (ox < -4096 || ox > 14335 || oy < -4096 || oy > 11775) return 0;
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
    uint16_t counts[RAST_STRIPS] = {0};
    size_t need = RAST_STRIPS * 4, cursor = 0;
    if (!out || count > 65535 || (count && !records)) return -1;
    for (unsigned i = 0; i < count; ++i) {
        if (!rast_record_valid(&records[i])) return -1;
        unsigned first = rast_get16(&records[i], 76) / RAST_STRIP_H;
        unsigned last = rast_get16(&records[i], 78) / RAST_STRIP_H;
        for (unsigned s = first; s <= last; ++s) { ++counts[s]; need += 2; }
    }
    if (need > capacity) return -2;
    for (unsigned s = 0; s < RAST_STRIPS; ++s) {
        out[cursor++] = (uint8_t)counts[s]; out[cursor++] = (uint8_t)(counts[s] >> 8);
        out[cursor++] = 0; out[cursor++] = 0;
        for (unsigned i = 0; i < count; ++i)
            if (rast_get16(&records[i], 76) / RAST_STRIP_H <= s &&
                rast_get16(&records[i], 78) / RAST_STRIP_H >= s) {
                out[cursor++] = (uint8_t)i; out[cursor++] = (uint8_t)(i >> 8);
            }
    }
    return (int)need;
}
