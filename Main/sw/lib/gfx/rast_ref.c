/******************************************************************************
* @file rast_ref.c: top-left、LESS、仿射 CLAMP、色键与原子色/z 提交
* @brief 边缘 s32，属性使用 int64（硬件有符号 48 位足够）；内环仅增量。
******************************************************************************/
#include "gfx/rast_ref.h"

static int32_t signed32(const rast_record *r, unsigned o)
{
    return (int32_t)rast_get32(r, o);
}

static int quantize(int64_t value, int limit)
{
    int64_t q = fx16_floor_div(value + 32768, 65536);
    return q < 0 ? 0 : q > limit ? limit : (int)q;
}

int rast_ref_run(const rast_record *records, unsigned count, const rast_target *t)
{
    const rast_frame frame = {RAST_WIDTH,RAST_HEIGHT};
    return rast_ref_run_frame(records,count,t,&frame);
}

int rast_ref_run_frame(const rast_record *records, unsigned count, const rast_target *t,
                       const rast_frame *frame)
{
    const unsigned grad[6] = {44, 56, 64, 96, 104, 112};
    if (!rast_frame_valid(frame) || !t || !t->color || !t->depth ||
        t->stride < frame->width || t->strip >= (frame->height+RAST_STRIP_H-1u)/RAST_STRIP_H ||
        (count && !records)) return -1;
    for (unsigned i = 0; i < count; ++i) {
        if (!rast_record_valid_frame(&records[i],frame)) return -1;
        if ((rast_get32(&records[i], 0) & RAST_TEX_EN) &&
            (!t->texture || !t->tex_width || !t->tex_height ||
             t->tex_width > 4096 || t->tex_height > 4096 || t->tex_stride < t->tex_width)) return -1;
    }
    for (unsigned i = 0; i < count; ++i) {
        const rast_record *r = &records[i];
        uint32_t flags = rast_get32(r, 0), color = rast_get32(r, 80);
        int x0 = rast_get16(r, 72), x1 = rast_get16(r, 74);
        int y0 = rast_get16(r, 76), y1 = rast_get16(r, 78);
        int sy = (int)t->strip * RAST_STRIP_H;
        int32_t a[3], b[3], row[3], bias[3], gx[6], gy[6];
        int64_t value[6], base[6] = {
            (int64_t)rast_get16(r, 40) * 65536,
            (int64_t)rast_get16(r, 52) * 4096, (int64_t)rast_get16(r, 54) * 4096,
            (int64_t)((color >> 16) & 255u) * 65536,
            (int64_t)((color >> 8) & 255u) * 65536, (int64_t)(color & 255u) * 65536};
        if (y0 < sy) y0 = sy;
        if (y1 >= sy + RAST_STRIP_H) y1 = sy + RAST_STRIP_H - 1;
        if (y0 > y1) continue;
        for (unsigned k = 0; k < 3; ++k) {
            a[k] = signed32(r, 4 + k * 12); b[k] = signed32(r, 8 + k * 12);
#ifdef INJECT_RAST_NO_16
            a[k] /= 16; b[k] /= 16;
#endif
            row[k] = (int32_t)((int64_t)a[k] * x0 + (int64_t)b[k] * y0 + signed32(r, 12 + k * 12));
            bias[k] = (a[k] > 0 || (a[k] == 0 && b[k] > 0)) ? 0 : -1;
#ifdef INJECT_RAST_ALL_EDGES
            bias[k] = 0;
#endif
        }
        for (unsigned k = 0; k < 6; ++k) {
            gx[k] = signed32(r, grad[k]); gy[k] = signed32(r, grad[k] + 4);
            value[k] = base[k] + fx16_round_div(
                (int64_t)gx[k] * (x0 * 16 - (int16_t)rast_get16(r, 120)) +
                (int64_t)gy[k] * (y0 * 16 - (int16_t)rast_get16(r, 122)), 16);
        }
        for (int y = y0; y <= y1; ++y) {
            int32_t e[3] = {row[0], row[1], row[2]};
            int64_t acc[6];
            for (unsigned k = 0; k < 6; ++k) acc[k] = value[k];
            for (int x = x0; x <= x1; ++x) {
                if ((int64_t)e[0] + bias[0] >= 0 && (int64_t)e[1] + bias[1] >= 0 &&
                    (int64_t)e[2] + bias[2] >= 0) {
                    unsigned at = (unsigned)(y - sy) * t->stride + (unsigned)x;
                    uint16_t z = (uint16_t)quantize(acc[0], 65535);
                    if (z < t->depth[at]) {
                        uint32_t texel = 0x00ffffffu, result = 0;
                        int skip = 0;
                        if (flags & RAST_TEX_EN) {
                            unsigned u = (unsigned)quantize(acc[1], (int)t->tex_width - 1);
                            unsigned v = (unsigned)quantize(acc[2], (int)t->tex_height - 1);
                            texel = t->texture[v * t->tex_stride + u];
                            skip = (flags & RAST_KEY_EN) && texel == t->key;
                        }
                        if (!skip) {
                            for (unsigned k = 0; k < 3; ++k) {
                                unsigned shift = 16 - k * 8;
                                unsigned c = (flags & RAST_VCOLOR_EN) ? (unsigned)quantize(acc[k + 3], 255) : 255;
                                unsigned channel = (flags & RAST_TEX_EN) ?
                                    ((((texel >> shift) & 255u) * c + 127) / 255) : c;
                                result |= channel << shift;
                            }
                            t->color[at] = result; t->depth[at] = z;
                        }
                    }
                }
                /* 最后一个像素后也安全；记录系数已限制为 guard-band 范围。 */
                for (unsigned k = 0; k < 3; ++k) e[k] += a[k];
                for (unsigned k = 0; k < 6; ++k) acc[k] += gx[k];
            }
            for (unsigned k = 0; k < 3; ++k) row[k] += b[k];
            for (unsigned k = 0; k < 6; ++k) value[k] += gy[k];
        }
    }
    return 0;
}
