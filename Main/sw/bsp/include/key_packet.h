/* UART input transport shared by the BSP and its protocol tests. */
#ifndef GFX2D_KEY_PACKET_H
#define GFX2D_KEY_PACKET_H
#include <stdint.h>

/* Legacy: high-bit header + 7bit data/checksum, ten key bits.
 * Extended: ff 7f lo7 hi7 CRC7 (poly x^7+x^3+1, init=0,
 * MSB-first over lo7 followed by hi7).
 * ff 7f is an invalid legacy packet, so old receivers safely ignore it. */
typedef struct {
    uint8_t state, header, lo, hi;
} key_packet_decoder;

/* Returns one only for a complete checksum-valid snapshot. */
int key_packet_feed(key_packet_decoder *decoder, uint8_t byte, uint32_t *keys);
#endif
