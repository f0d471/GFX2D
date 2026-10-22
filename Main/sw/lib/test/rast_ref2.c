/******************************************************************************
* @file rast_ref2.c: 直接求值的独立标量参考，不复用增量执行器的数值路径
******************************************************************************/
#include "test/rast_ref2.h"

static int64_t nearest(int64_t n, int64_t d)
{
    int64_t q = n / d, r = n % d;
    if (r < 0) { --q; r += d; }
    return q + (r * 2 >= d);
}

static int sample(const rast_record *r, unsigned channel, int x, int y, int limit)
{
    const unsigned gradients[] = {44, 56, 64, 96, 104, 112};
    int64_t start;
    if (!channel) start = (int64_t)rast_get16(r, 40) * 65536;
    else if (channel < 3) start = (int64_t)rast_get16(r, 50 + channel * 2) * 4096;
    else start = (int64_t)((rast_get32(r, 80) >> (40 - channel * 8)) & 255) * 65536;
    unsigned g = gradients[channel];
    int64_t n = (int64_t)(int32_t)rast_get32(r, g) * (x * 16 - (int16_t)rast_get16(r, 120)) +
                (int64_t)(int32_t)rast_get32(r, g + 4) * (y * 16 - (int16_t)rast_get16(r, 122));
    int64_t value = nearest(start + nearest(n, 16), 65536);
    return value < 0 ? 0 : value > limit ? limit : (int)value;
}

int rast_ref2_run(const rast_record *records, unsigned count, const rast_target *t)
{
    if (!t || !t->color || !t->depth || t->stride < 640 || t->strip >= 30 || (count && !records)) return -1;
    for (unsigned i = 0; i < count; ++i) {
        const rast_record *r = records + i;
        if (!rast_record_valid(r)) return -1;
        if ((rast_get32(r, 0) & 1) && (!t->texture || !t->tex_width || !t->tex_height ||
            t->tex_width > 4096 || t->tex_height > 4096 || t->tex_stride < t->tex_width)) return -1;
    }
    for (unsigned i = 0; i < count; ++i) {
        const rast_record *r = records + i;
        unsigned flags = rast_get32(r, 0);
        int first = (int)t->strip * 16, last = first + 15;
        if (first < rast_get16(r, 76)) first = rast_get16(r, 76);
        if (last > rast_get16(r, 78)) last = rast_get16(r, 78);
        for (int y = first; y <= last; ++y)
            for (int x = rast_get16(r, 72); x <= rast_get16(r, 74); ++x) {
                int covered = 1;
                for (unsigned k = 0; k < 3; ++k) {
                    int32_t a = (int32_t)rast_get32(r, 4 + k * 12);
                    int32_t b = (int32_t)rast_get32(r, 8 + k * 12);
                    int64_t e = (int64_t)a * x + (int64_t)b * y + (int32_t)rast_get32(r, 12 + k * 12);
                    if (e < 0 || (!e && !(a > 0 || (!a && b > 0)))) covered = 0;
                }
                if (!covered) continue;
                unsigned at = (unsigned)(y % 16) * t->stride + (unsigned)x;
                int z = sample(r, 0, x, y, 65535);
                if (z >= t->depth[at]) continue;
                uint32_t tex = 0xffffffu, result = 0;
                if (flags & 1) {
                    tex = t->texture[(unsigned)sample(r, 2, x, y, (int)t->tex_height - 1) * t->tex_stride +
                                     (unsigned)sample(r, 1, x, y, (int)t->tex_width - 1)];
                    if ((flags & 4) && tex == t->key) continue;
                }
                for (unsigned k = 0; k < 3; ++k) {
                    unsigned s = 16 - 8 * k;
                    unsigned c = flags & 2 ? (unsigned)sample(r, k + 3, x, y, 255) : 255;
                    if (flags & 1) c = (((tex >> s) & 255) * c + 127) / 255;
                    result += c << s;
                }
                t->color[at] = result; t->depth[at] = (uint16_t)z;
            }
    }
    return 0;
}
