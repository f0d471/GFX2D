/******************************************************************************
* @file replay.c: 录像的格式与逐帧取键
*
* @brief  读写都逐字节做小端换算。理由见 replay.h：这份格式要同时被
*         LP64 的 PC、ILP32 的板上程序和一个 Python 脚本解析，
*         三方对同一个字节串必须得出同一串按键。
*
*         按键位图按原样存取，本层不解释它的位：位分配是 board.h 的约定。
******************************************************************************/

#include <stddef.h>

#include "core/replay.h"
#include "core/format.h"

static void put_u16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint32_t get_u16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* 头部 16 字节 + 每帧 4 字节。溢出按「装不下」处理，返回 0。 */
uint32_t replay_bytes_for(uint32_t frames)
{
    if (frames > (UINT32_MAX - REPLAY_HEADER_BYTES) / REPLAY_RECORD_BYTES)
        return 0u;
    return REPLAY_HEADER_BYTES + frames * REPLAY_RECORD_BYTES;
}

static void write_header(uint8_t *buf, uint32_t seed, uint32_t frames)
{
    put_u32(buf + 0, REPLAY_MAGIC);
    put_u16(buf + 4, REPLAY_VERSION);
    put_u16(buf + 6, 0u);
    put_u32(buf + 8, seed);
    put_u32(buf + 12, frames);
}

uint32_t replay_pack(uint8_t *buf, uint32_t cap, uint32_t seed,
                     const uint32_t *keys, uint32_t frames)
{
    uint32_t need = replay_bytes_for(frames);
    uint32_t i;

    if (buf == NULL || keys == NULL || need == 0u || cap < need)
        return 0u;

    write_header(buf, seed, frames);
    for (i = 0u; i < frames; i++)
        put_u32(buf + REPLAY_HEADER_BYTES + i * REPLAY_RECORD_BYTES, keys[i]);
    return need;
}

void replay_rec_begin(replay_rec *r, uint8_t *buf, uint32_t cap, uint32_t seed)
{
    r->buf    = buf;
    r->cap    = cap;
    r->frames = 0u;
    r->seed   = seed;
    r->full   = 0;

    if (buf == NULL || cap < REPLAY_HEADER_BYTES) {
        r->full = 1;
        return;
    }
    write_header(buf, seed, 0u);
}

int replay_rec_put(replay_rec *r, uint32_t keys)
{
    uint32_t off;

    if (r->full)
        return -1;

    off = REPLAY_HEADER_BYTES + r->frames * REPLAY_RECORD_BYTES;
    if (off + REPLAY_RECORD_BYTES > r->cap) {
        r->full = 1;
        return -1;
    }
    put_u32(r->buf + off, keys);
    r->frames++;
    return 0;
}

uint32_t replay_rec_frames(const replay_rec *r)
{
    return r->frames;
}

uint32_t replay_rec_end(replay_rec *r)
{
    if (r->buf == NULL || r->cap < REPLAY_HEADER_BYTES)
        return 0u;
    put_u32(r->buf + 12, r->frames);
    return REPLAY_HEADER_BYTES + r->frames * REPLAY_RECORD_BYTES;
}

int replay_open(const uint8_t *buf, uint32_t len, replay_view *out)
{
    uint32_t frames;
    uint32_t need;

    if (buf == NULL || out == NULL)
        return REPLAY_E_LEN;
    if (len < REPLAY_HEADER_BYTES)
        return REPLAY_E_LEN;
    if (get_u32(buf + 0) != REPLAY_MAGIC)
        return REPLAY_E_MAGIC;
    if (get_u16(buf + 4) != REPLAY_VERSION)
        return REPLAY_E_VERSION;
    if (get_u16(buf + 6) != 0u)
        return REPLAY_E_FLAGS;

    frames = get_u32(buf + 12);
    need   = replay_bytes_for(frames);
    if (need == 0u || len != need)
        return REPLAY_E_LEN;            /* 多一个字节或少一个字节都不算合法 */

    out->buf    = buf;
    out->len    = len;
    out->seed   = get_u32(buf + 8);
    out->frames = frames;
    return 0;
}

int replay_key_at(const replay_view *v, uint32_t frame, uint32_t *keys)
{
    if (v == NULL || v->buf == NULL || keys == NULL)
        return REPLAY_E_RANGE;
    if (frame >= v->frames)
        return REPLAY_E_RANGE;
    *keys = get_u32(v->buf + REPLAY_HEADER_BYTES + frame * REPLAY_RECORD_BYTES);
    return 0;
}

uint32_t replay_text_header(uint32_t seed, uint32_t frames, char *buf, uint32_t cap)
{
    core_out o;

    core_out_init(&o, buf, cap);
    core_out_str(&o, "g2dr ");
    core_out_u32(&o, REPLAY_VERSION);
    core_out_ch(&o, ' ');
    core_out_u32(&o, seed);
    core_out_ch(&o, ' ');
    core_out_u32(&o, frames);
    core_out_str(&o, "\r\n");
    return (o.n < cap) ? o.n : 0u;
}

uint32_t replay_text_key(uint32_t frame, uint32_t keys, char *buf, uint32_t cap)
{
    core_out o;

    core_out_init(&o, buf, cap);
    core_out_str(&o, "key ");
    core_out_u32(&o, frame);
    core_out_ch(&o, ' ');
    core_out_hex32(&o, keys);
    core_out_str(&o, "\r\n");
    return (o.n < cap) ? o.n : 0u;
}
