/* Ti60 timer port constants. board.c checks these against generated soc.h.
 * Only the BSP and RTOS port use the CLINT addresses. */
#ifndef GFX2D_BOARD_CLOCK_H
#define GFX2D_BOARD_CLOCK_H
#define BOARD_CLOCK_HZ 100000000UL
#define BOARD_CLINT_BASE 0xf8b00000UL
#define BOARD_MTIME_ADDRESS (BOARD_CLINT_BASE + 0xbff8UL)
#define BOARD_MTIMECMP_ADDRESS (BOARD_CLINT_BASE + 0x4000UL)
#endif
