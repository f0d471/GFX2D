#include <stdio.h>
#include <string.h>
#include "board.h"
#include "key_packet.h"
static unsigned fail;
static uint32_t raw_physical,raw_remote;
static int window_escape;
uint32_t board_input_raw(uint32_t *physical) { *physical=raw_physical;return raw_remote; }
void board_input_window_policy(int enabled) { window_escape=enabled; }
/* Independent polynomial long division, not the decoder's shift register. */
static uint8_t reference(uint32_t lo,uint32_t hi)
{
    uint32_t word=((lo<<7)|hi)<<7;
    int bit;
    for(bit=20;bit>=7;--bit)if(word&(1u<<(unsigned)bit))word^=0x89u<<(unsigned)(bit-7);
    return (uint8_t)(word&127u);
}
/* Fifteen-bit span: lo7 then hi8, one more data bit before the same CRC. */
static uint8_t reference15(uint32_t lo,uint32_t hi)
{
    uint32_t word=((lo<<8)|hi)<<7;
    int bit;
    for(bit=21;bit>=7;--bit)if(word&(1u<<(unsigned)bit))word^=0x89u<<(unsigned)(bit-7);
    return (uint8_t)(word&127u);
}
#define CHECK(c,n) do { if(!(c)) { ++fail;printf("FAIL %s\n",n); } } while(0)
int main(void)
{
    key_packet_decoder d={0};uint32_t k,got=0;
    for(k=0;k<1024u;++k) {
        uint32_t check=(k^(k>>4)^(k>>8))&15u;
        CHECK(!key_packet_feed(&d,(uint8_t)(0x80u|(k&127u)),&got),"legacy-header");
        CHECK(key_packet_feed(&d,(uint8_t)((check<<3)|(k>>7)),&got)&&got==k,"legacy-roundtrip");
    }
    for(k=0;k<16384u;++k) {
        const uint8_t lo=(uint8_t)(k&127u),hi=(uint8_t)(k>>7);
        const uint8_t check=reference(lo,hi);
        CHECK(!key_packet_feed(&d,255,&got),"extended-sync-a");
        CHECK(!key_packet_feed(&d,127,&got),"extended-sync-b");
        CHECK(!key_packet_feed(&d,lo,&got),"extended-lo");
        CHECK(!key_packet_feed(&d,hi,&got),"extended-hi");
        CHECK(key_packet_feed(&d,check,&got)&&got==k,"extended-roundtrip");
        (void)key_packet_feed(&d,255,&got);(void)key_packet_feed(&d,127,&got);
        (void)key_packet_feed(&d,lo,&got);(void)key_packet_feed(&d,hi,&got);
        CHECK(!key_packet_feed(&d,(uint8_t)(check^1u),&got),"bad-checksum-rejected");
        for(unsigned bit=0;bit<14u;++bit) {
            uint8_t badlo=lo,badhi=hi;
            if(bit<7u)badlo^=(uint8_t)(1u<<bit);else badhi^=(uint8_t)(1u<<(bit-7u));
            (void)key_packet_feed(&d,255,&got);(void)key_packet_feed(&d,127,&got);
            (void)key_packet_feed(&d,badlo,&got);(void)key_packet_feed(&d,badhi,&got);
            CHECK(!key_packet_feed(&d,check,&got),"single-payload-bit-error-rejected");
        }
    }
    for(k=16384u;k<32768u;++k) {
        const uint8_t lo=(uint8_t)(k&127u),hi=(uint8_t)(k>>7);  /* hi carries bit 7 */
        const uint8_t check=reference15(lo,hi);
        CHECK(!key_packet_feed(&d,255,&got),"fifteen-sync-a");
        CHECK(!key_packet_feed(&d,126,&got),"fifteen-sync-b");
        CHECK(!key_packet_feed(&d,lo,&got),"fifteen-lo");
        CHECK(!key_packet_feed(&d,hi,&got),"fifteen-hi");
        CHECK(key_packet_feed(&d,check,&got)&&got==k,"fifteen-roundtrip");
        (void)key_packet_feed(&d,255,&got);(void)key_packet_feed(&d,126,&got);
        (void)key_packet_feed(&d,lo,&got);(void)key_packet_feed(&d,hi,&got);
        CHECK(!key_packet_feed(&d,(uint8_t)(check^1u),&got),"fifteen-bad-checksum-rejected");
        for(unsigned bit=0;bit<15u;++bit) {
            uint8_t badlo=lo,badhi=hi;
            if(bit<7u)badlo^=(uint8_t)(1u<<bit);else badhi^=(uint8_t)(1u<<(bit-7u));
            (void)key_packet_feed(&d,255,&got);(void)key_packet_feed(&d,126,&got);
            (void)key_packet_feed(&d,badlo,&got);(void)key_packet_feed(&d,badhi,&got);
            CHECK(!key_packet_feed(&d,check,&got),"fifteen-single-bit-error-rejected");
        }
    }
    memset(&d,0,sizeof d);(void)key_packet_feed(&d,255,&got);(void)key_packet_feed(&d,127,&got);
    (void)key_packet_feed(&d,12,&got);(void)key_packet_feed(&d,128,&got);
    CHECK(key_packet_feed(&d,0,&got)&&got==0,"truncated-frame-resyncs-to-release");
    board_product_keys(0);
    CHECK(board_map_physical_keys(15)==15,"legacy-four-keys-unchanged");
    board_product_keys(1);
    CHECK(board_map_physical_keys(BOARD_KEY_LEFT|BOARD_KEY_RIGHT)==BOARD_KEY_CONFIRM,"physical-confirm");
    CHECK(board_map_physical_keys(BOARD_KEY_UP|BOARD_KEY_DOWN)==BOARD_KEY_BACK,"physical-back");
    CHECK(board_map_physical_keys(BOARD_KEY_UP|BOARD_KEY_LEFT)==(BOARD_KEY_UP|BOARD_KEY_LEFT),"normal-direction-pair");
    raw_physical=BOARD_KEY_LEFT|BOARD_KEY_RIGHT;raw_remote=0;
    CHECK(board_keys()==BOARD_KEY_CONFIRM && !window_escape,"service-maps-physical-only");
    raw_physical=0;raw_remote=BOARD_KEY_LEFT|BOARD_KEY_RIGHT;
    CHECK(board_keys()==raw_remote,"host-direction-pair-is-not-physical-chord");
    board_product_keys(0);
    CHECK(window_escape && board_keys()==raw_remote,"legacy-window-policy-retained");
    printf("input exhaustive legacy=1024 extended=32768 fail %u\n",fail);
    return fail?1:0;
}
