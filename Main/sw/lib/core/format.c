/******************************************************************************
* @file format.c: 轻量格式化输出
*
* @brief  见 format.h。全程只用 32 位整数运算，两个平台结果相同。
*         缓冲区在每次追加之后都保持以 '\0' 结尾，调用方拼完直接使用，
*         不需要再补一个终止符。
******************************************************************************/

#include "core/format.h"

static void out_terminate(core_out *o)
{
    if (o->cap > 0u) {
        uint32_t end = (o->n < o->cap) ? o->n : (o->cap - 1u);

        o->buf[end] = '\0';
    }
}

void core_out_init(core_out *o, char *buf, uint32_t cap)
{
    o->buf = buf;
    o->cap = cap;
    o->n   = 0u;
    out_terminate(o);
}

void core_out_ch(core_out *o, char c)
{
    if (o->n + 1u < o->cap)
        o->buf[o->n] = c;
    o->n++;
    out_terminate(o);
}

void core_out_str(core_out *o, const char *s)
{
    while (*s != '\0')
        core_out_ch(o, *s++);
}

static void out_digits(core_out *o, uint32_t v, uint32_t base)
{
    char     tmp[12];
    uint32_t n = 0u;

    if (v == 0u) {
        tmp[n++] = '0';
    } else {
        while (v != 0u) {
            uint32_t d = v % base;
            tmp[n++] = (char)((d < 10u) ? ('0' + d) : ('a' + (d - 10u)));
            v /= base;
        }
    }
    while (n != 0u)
        core_out_ch(o, tmp[--n]);
}

/* 十进制位数，只用于对齐 */
static uint32_t dec_width(uint32_t v)
{
    uint32_t w = 1u;

    while (v >= 10u) {
        v /= 10u;
        w++;
    }
    return w;
}

void core_out_u32(core_out *o, uint32_t v)
{
    out_digits(o, v, 10u);
}

void core_out_u32_pad(core_out *o, uint32_t v, uint32_t width)
{
    uint32_t w = dec_width(v);

    while (w < width) {
        core_out_ch(o, ' ');
        w++;
    }
    out_digits(o, v, 10u);
}

void core_out_hex32(core_out *o, uint32_t v)
{
    static const char digits[] = "0123456789abcdef";

    for (int k = 7; k >= 0; k--)
        core_out_ch(o, digits[(v >> (uint32_t)(k * 4)) & 0xFu]);
}

uint32_t core_out_len(const core_out *o)
{
    return o->n;
}
