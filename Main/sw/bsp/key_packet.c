#include "key_packet.h"

static uint32_t crc7_word(uint32_t word,int bits)
{
    uint32_t crc=0;
    int bit;
    for(bit=bits-1;bit>=0;--bit) {
        const uint32_t feedback=((crc>>6)^(word>>(unsigned)bit))&1u;
        crc=((crc<<1)&127u)^(feedback?9u:0u);
    }
    return crc;
}

/* 历史 14 位跨距:lo7 接 hi7,与旧发送端逐字节一致。 */
static uint32_t crc7_14(uint32_t lo,uint32_t hi)
{
    return crc7_word((lo<<7)|hi,14);
}

/* 15 位跨距:lo7 接 hi8(第 15 位 = APP6/HALTTIME)。
   跨距由第二个同步字节选定(ff 7f=14 位,ff 7e=15 位),不是试出来的:
   一个包只对应一个 CRC,单比特错误才保持确定性检出
   (两个跨距对同一字节流的值不同,若两个都试,错包会以 ~1/128
   概率蹭过另一个跨距的校验)。 */
static uint32_t crc7_15(uint32_t lo,uint32_t hi)
{
    return crc7_word((lo<<8)|hi,15);
}

int key_packet_feed(key_packet_decoder *d, uint8_t b, uint32_t *keys)
{
    uint32_t k;
    /* ff 7e(15 位跨距)的 hi 字节可以带 bit7,在待收 hi(state 3)时无条件收下;
       ff 7f(14 位跨距)与其它状态下,bit7 置位的字节一律算新包头
       (14 位跨距的 hi 永远不带 bit7,截断帧靠它立即重新同步)。 */
    if ((b & 0x80u) != 0u && !(d->state == 3u && d->header == 0x7eu)) {
        d->header = b;
        d->state = 1u;
        return 0;
    }
    if (d->state == 1u) {
        if (d->header == 0xffu && (b == 0x7fu || b == 0x7eu)) {
            d->header = b;          /* 0xff 已用完,跨距选择暂存到 header */
            d->state = 2u;
            return 0;
        }
        d->state = 0u;
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
        if (d->header == 0x7fu) {
            if (b != crc7_14(d->lo,d->hi))
                return 0;
            *keys = (uint32_t)d->lo | ((uint32_t)(d->hi & 0x7fu) << 7);
            return 1;
        }
        if (b != crc7_15(d->lo,d->hi))
            return 0;
        *keys = (uint32_t)d->lo | ((uint32_t)d->hi << 7);
        return 1;
    }
    return 0;
}
