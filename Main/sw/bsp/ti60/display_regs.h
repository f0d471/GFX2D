#ifndef GFX2D_TI60_DISPLAY_REGS_H
#define GFX2D_TI60_DISPLAY_REGS_H

/* display_fetch.v 的 APB 页；项目 BSP 共用，不让应用直接访问。 */
#define DISP_BASE           0xF810E000u
#define DISP_ID             (DISP_BASE + 0x000u)
#define DISP_CTRL           (DISP_BASE + 0x008u)
#define DISP_BACK_BASE      (DISP_BASE + 0x010u)
#define DISP_SWAP_REQ       (DISP_BASE + 0x014u)
#define DISP_IRQ_EN         (DISP_BASE + 0x0D0u)
#define DISP_IRQ_STAT       (DISP_BASE + 0x0D4u)
#define DISP_FRAME_COUNT    (DISP_BASE + 0x0D8u)
#define DISP_SWAP_COUNT     (DISP_BASE + 0x0DCu)
#define DISP_SNAP_FRAME     (DISP_BASE + 0x0E0u)
#define DISP_SNAP_MIN       (DISP_BASE + 0x0E4u)
#define DISP_ZERO_HITS      (DISP_BASE + 0x0E8u)
#define DISP_UNDER_BEATS    (DISP_BASE + 0x0ECu)

#define DISP_ID_VALUE       0x44495350u
#define DISP_CTRL_EN        (1u << 0)
#define DISP_CTRL_SRC_FB    (1u << 1)
#define DISP_CTRL_CLEAR     (1u << 2)

#endif
