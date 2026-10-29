#include "key_packet.h"

static uint32_t crc7(uint32_t lo,uint32_t hi)
{
    uint32_t crc=0,word=(lo<<7)|hi;
    int bit;
    for(bit=13;bit>=0;--bit) {
        const uint32_t feedback=((crc>>6)^(word>>(unsigned)bit))&1u;
        crc=((crc<<1)&127u)^(feedback?9u:0u);
    }
    return crc;
}

int key_packet_feed(key_packet_decoder *d, uint8_t b, uint32_t *keys)
{
    uint32_t k;
    if ((b & 0x80u) != 0u) {
        d->header = b;
        d->state = 1u;
        return 0;
    }
    if (d->state == 1u) {
        d->state = 0u;
        if (d->header == 0xffu && b == 0x7fu) {
            d->state = 2u;
            return 0;
        }
        k = (uint32_t)(d->header & 0x7fu) | ((uint32_t)(b & 7u) << 7);
        if ((uint32_t)(b >> 3) != ((k ^ (k >> 4) ^ (k >> 8)) & 15u))
            return 0;
        *keys = k;
        return 1;
    }
    if (d->state == 2u) {
        d->lo = b;
        d->state = 3u;
    } else if (d->state == 3u) {
        d->hi = b;
        d->state = 4u;
    } else if (d->state == 4u) {
        d->state = 0u;
        if (b != crc7(d->lo,d->hi))
            return 0;
        *keys = (uint32_t)d->lo | ((uint32_t)d->hi << 7);
        return 1;
    }
    return 0;
}
