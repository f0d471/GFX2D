/* UART input transport shared by the BSP and its protocol tests. */
#ifndef GFX2D_KEY_PACKET_H
#define GFX2D_KEY_PACKET_H
#include <stdint.h>

/* Legacy: high-bit header + 7bit data/checksum, ten key bits.
 * Extended, fourteen-bit span: ff 7f lo7 hi7 CRC7 over lo7 then hi7
 * (unchanged historical bytes).
 * Extended, fifteen-bit span: ff 7e lo7 hi8 CRC7 over lo7 then hi8;
 * hi bit 7 = key bit 14 = APP6/HALTTIME.  The second sync byte selects
 * the span, so each packet maps to exactly one CRC and single-bit payload
 * errors stay deterministically rejected; 0x7e decodes as an invalid
 * legacy packet, so old receivers drop ff 7e frames.
 * poly x^7+x^3+1, init=0, MSB-first.
 * ff 7f is an invalid legacy packet, so old receivers safely ignore it. */
typedef struct {
    uint8_t state, header, lo, hi;
} key_packet_decoder;

/* Returns one only for a complete checksum-valid snapshot. */
int key_packet_feed(key_packet_decoder *decoder, uint8_t byte, uint32_t *keys);
#endif
