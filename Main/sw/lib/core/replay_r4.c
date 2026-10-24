/******************************************************************************
* @file replay_r4.c: 三维演示模式（R4）录像的格式与逐 tick 取键
*
* @brief  读写都逐字节做小端换算，理由同 replay.c：这份格式要同时被
*         LP64 的 PC、ILP32 的板上程序和一个 Python 脚本解析。
*         与 .g2dr 的解析器互不调用、互不改语义。
******************************************************************************/

#include <stddef.h>

#include "core/replay_r4.h"
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

/* 头部 32 字节 + 每 tick 4 字节。溢出按「装不下」处理，返回 0。 */
uint32_t replay_r4_bytes_for(uint32_t frames)
{
    if (frames > (UINT32_MAX - REPLAY_R4_HEADER_BYTES) / REPLAY_R4_RECORD_BYTES)
        return 0u;
    return REPLAY_R4_HEADER_BYTES + frames * REPLAY_R4_RECORD_BYTES;
}

static int ladder_ok(uint32_t ladder)
{
    return ladder == REPLAY_R4_L1 || ladder == REPLAY_R4_L2;
}

static void write_header(uint8_t *buf, uint32_t seed, uint32_t scene_id,
                         uint32_t ladder, uint32_t asset_version, uint32_t frames)
{
    put_u32(buf + 0, REPLAY_R4_MAGIC);
    put_u16(buf + 4, REPLAY_R4_VERSION);
    put_u16(buf + 6, 0u);
    put_u32(buf + 8, REPLAY_R4_TICK_HZ);
    put_u32(buf + 12, seed);
    put_u32(buf + 16, scene_id);
    put_u32(buf + 20, ladder);
    put_u32(buf + 24, asset_version);
    put_u32(buf + 28, frames);
}

uint32_t replay_r4_pack(uint8_t *buf, uint32_t cap, uint32_t seed, uint32_t scene_id,
                        uint32_t ladder, uint32_t asset_version,
                        const uint32_t *keys, uint32_t frames)
{
    uint32_t need = replay_r4_bytes_for(frames);
    uint32_t i;

    if (buf == NULL || keys == NULL || need == 0u || cap < need || !ladder_ok(ladder))
        return 0u;

    write_header(buf, seed, scene_id, ladder, asset_version, frames);
    for (i = 0u; i < frames; i++)
        put_u32(buf + REPLAY_R4_HEADER_BYTES + i * REPLAY_R4_RECORD_BYTES, keys[i]);
    return need;
}

void replay_r4_rec_begin(replay_r4_rec *r, uint8_t *buf, uint32_t cap,
                         uint32_t seed, uint32_t scene_id, uint32_t ladder,
                         uint32_t asset_version)
{
    r->buf           = buf;
    r->cap           = cap;
    r->frames        = 0u;
    r->seed          = seed;
    r->scene_id      = scene_id;
    r->ladder        = ladder;
    r->asset_version = asset_version;
    r->full          = 0;

    if (buf == NULL || cap < REPLAY_R4_HEADER_BYTES || !ladder_ok(ladder)) {
        r->full = 1;
        return;
    }
    write_header(buf, seed, scene_id, ladder, asset_version, 0u);
}

int replay_r4_rec_put(replay_r4_rec *r, uint32_t keys)
{
    uint32_t off;

    if (r->full)
        return -1;

    off = REPLAY_R4_HEADER_BYTES + r->frames * REPLAY_R4_RECORD_BYTES;
    if (off + REPLAY_R4_RECORD_BYTES > r->cap) {
        r->full = 1;
        return -1;
    }
    put_u32(r->buf + off, keys);
    r->frames++;
    return 0;
}

uint32_t replay_r4_rec_frames(const replay_r4_rec *r)
{
    return r->frames;
}

uint32_t replay_r4_rec_end(replay_r4_rec *r)
{
    /* rec_begin 因缓冲或非法阶梯拒绝过就不再写：头部根本没落笔。 */
    if (r->buf == NULL || r->cap < REPLAY_R4_HEADER_BYTES || !ladder_ok(r->ladder))
        return 0u;
    put_u32(r->buf + 28, r->frames);
    return REPLAY_R4_HEADER_BYTES + r->frames * REPLAY_R4_RECORD_BYTES;
}

int replay_r4_open(const uint8_t *buf, uint32_t len, replay_r4_view *out)
{
    uint32_t frames;
    uint32_t need;
    uint32_t ladder;

    if (buf == NULL || out == NULL)
        return REPLAY_R4_E_LEN;
    if (len < REPLAY_R4_HEADER_BYTES)
        return REPLAY_R4_E_LEN;
    if (get_u32(buf + 0) != REPLAY_R4_MAGIC)
        return REPLAY_R4_E_MAGIC;
    if (get_u16(buf + 4) != REPLAY_R4_VERSION)
        return REPLAY_R4_E_VERSION;
    if (get_u16(buf + 6) != 0u)
        return REPLAY_R4_E_FLAGS;
    if (get_u32(buf + 8) != REPLAY_R4_TICK_HZ)
        return REPLAY_R4_E_TICK;
    ladder = get_u32(buf + 20);
    if (!ladder_ok(ladder))
        return REPLAY_R4_E_LADDER;

    frames = get_u32(buf + 28);
    need   = replay_r4_bytes_for(frames);
    if (need == 0u || len != need)
        return REPLAY_R4_E_LEN;         /* 多一个字节或少一个字节都不算合法 */

    out->buf           = buf;
    out->len           = len;
    out->seed          = get_u32(buf + 12);
    out->scene_id      = get_u32(buf + 16);
    out->ladder        = ladder;
    out->asset_version = get_u32(buf + 24);
    out->frames        = frames;
    return 0;
}

int replay_r4_key_at(const replay_r4_view *v, uint32_t frame, uint32_t *keys)
{
    if (v == NULL || v->buf == NULL || keys == NULL)
        return REPLAY_R4_E_RANGE;
    if (frame >= v->frames)
        return REPLAY_R4_E_RANGE;
    *keys = get_u32(v->buf + REPLAY_R4_HEADER_BYTES + frame * REPLAY_R4_RECORD_BYTES);
    return 0;
}

uint32_t replay_r4_text_header(uint32_t seed, uint32_t scene_id, uint32_t ladder,
                               uint32_t asset_version, uint32_t frames,
                               char *buf, uint32_t cap)
{
    core_out o;

    core_out_init(&o, buf, cap);
    core_out_str(&o, "r4rp ");
    core_out_u32(&o, REPLAY_R4_VERSION);
    core_out_ch(&o, ' ');
    core_out_u32(&o, seed);
    core_out_ch(&o, ' ');
    core_out_u32(&o, scene_id);
    core_out_ch(&o, ' ');
    core_out_u32(&o, ladder);
    core_out_ch(&o, ' ');
    core_out_u32(&o, asset_version);
    core_out_ch(&o, ' ');
    core_out_u32(&o, frames);
    core_out_str(&o, "\r\n");
    return (o.n < cap) ? o.n : 0u;
}

uint32_t replay_r4_text_key(uint32_t frame, uint32_t keys, char *buf, uint32_t cap)
{
    core_out o;

    core_out_init(&o, buf, cap);
    core_out_str(&o, "r4key ");
    core_out_u32(&o, frame);
    core_out_ch(&o, ' ');
    core_out_hex32(&o, keys);
    core_out_str(&o, "\r\n");
    return (o.n < cap) ? o.n : 0u;
}
