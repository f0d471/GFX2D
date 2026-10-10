/* 由 tools/check/regs_gen.py 生成（D51 的单一真源），不要手改；
 * 语义见 Main/doc/赛题二总体设计.md 3.4 节与 spec 第 02 章。
 * 引擎寄存器页：APB 第 0 页，基址 0xF810_0000（soc.h 的 IO_APB_SLAVE_0_INPUT）。 */
#ifndef GFX_REGS_H
#define GFX_REGS_H

/* 寄存器偏移（相对页基址的字节偏移） */
#define GFX_ID          0x000
#define GFX_CAPS          0x004
#define GFX_CTRL          0x008
#define GFX_STATUS          0x00C
#define GFX_IRQ_EN          0x010
#define GFX_IRQ_STAT          0x014
#define GFX_ERROR_INDEX          0x018
#define GFX_ERROR_TAG          0x01C
#define GFX_LIST_BASE          0x020
#define GFX_LIST_COUNT          0x024
#define GFX_DOORBELL          0x028
#define GFX_LSTRIPS          0x02C
#define GFX_STRIP_H          0x030
#define GFX_BAND_SURF          0x034
#define GFX_ERROR_INSTANCE          0x038
#define GFX_SURF(s, w)   (0x040 + ((s) * 12) + ((w) * 4))  /* s=0..15, w=0..2 */
#define GFX_PERF(i)      (0x100 + (i) * 4)

/* 性能计数器下标 */
#define GFX_PERF_CMD     0
#define GFX_PERF_PIXEL     1
#define GFX_PERF_KEY_SKIP     2
#define GFX_PERF_RD_BURST     3
#define GFX_PERF_WR_BURST     4
#define GFX_PERF_RD_BEAT     5
#define GFX_PERF_WR_BEAT     6
#define GFX_PERF_WAIT_DDR     7
#define GFX_PERF_BUSY     8
#define GFX_PERF_INSTANCE     9
#define GFX_PERF_COUNT   10

/* ID 与版本 */
#define GFX_ID_VALUE     0x47465801u  /* "GFX" << 8 | 版本 */
#define GFX_ID_VERSION   0x01u

/* CAPS 位掩码 */
#define GFX_CAPS_KEY_M   0x00000001u
#define GFX_CAPS_KEY_S   0
#define GFX_CAPS_ALPHA_PM_M   0x00000002u
#define GFX_CAPS_ALPHA_PM_S   1
#define GFX_CAPS_ALPHA_ST_M   0x00000004u
#define GFX_CAPS_ALPHA_ST_S   2
#define GFX_CAPS_MUL_M   0x00000008u
#define GFX_CAPS_MUL_S   3
#define GFX_CAPS_TINT_M   0x00000010u
#define GFX_CAPS_TINT_S   4
#define GFX_CAPS_BAND_M   0x00000020u
#define GFX_CAPS_BAND_S   5
#define GFX_CAPS_TEX_M   0x00000040u
#define GFX_CAPS_TEX_S   6
#define GFX_CAPS_INDEX8_M   0x00000080u
#define GFX_CAPS_INDEX8_S   7
#define GFX_CAPS_LANES_M   0x00000F00u
#define GFX_CAPS_LANES_S   8
#define GFX_CAPS_INST_M   0x00001000u
#define GFX_CAPS_INST_S   12
#define GFX_CAPS_VALUE   0x0000146Fu

/* CTRL 位（SOFT_RST/CNT_CLR 写 1 自清，读恒 0） */
#define GFX_CTRL_EN     (1u << 0)
#define GFX_CTRL_SOFT_RST     (1u << 1)
#define GFX_CTRL_CNT_CLR     (1u << 2)
#define GFX_CTRL_BAND     (1u << 3)

/* STATUS 位与错误码字段 */
#define GFX_STATUS_BUSY   (1u << 0)
#define GFX_STATUS_DRAINING   (1u << 1)
#define GFX_STATUS_DONE   (1u << 2)
#define GFX_STATUS_ERR   (1u << 3)
#define GFX_STATUS_CODE_M 0x000000F0u  /* [7:4] */
#define GFX_STATUS_CODE_S 4

/* 中断位（IRQ_STAT 写 1 清） */
#define GFX_IRQ_LIST_DONE      (1u << 0)
#define GFX_IRQ_ERROR      (1u << 1)

/* 错误码值 */
#define GFX_ERR_OK      0x0u
#define GFX_ERR_BAD_OPCODE      0x1u
#define GFX_ERR_BAD_FIELD      0x2u
#define GFX_ERR_BAD_EQUATION      0x3u
#define GFX_ERR_BAD_SURFACE      0x4u
#define GFX_ERR_BAD_SPACE      0x5u
#define GFX_ERR_BAD_FORMAT      0x6u
#define GFX_ERR_OVERLAP      0x7u
#define GFX_ERR_BAD_PREMULT      0x8u
#define GFX_ERR_FETCH      0x9u
#define GFX_ERR_BUS_READ      0xAu
#define GFX_ERR_BUS_WRITE      0xBu
#define GFX_ERR_TIMEOUT      0xCu

/* TEX 装载窗口（v1-TEX profile，G1 的 D57）：APB Slave0 页内偏移，
 * 不是第 0 页寄存器表的偏移；CPU 地址 = 0xF810_0000 + GFX_TEX_WINDOW。 */
#define GFX_TEX_WINDOW   0x4000u
#define GFX_TEX_BYTES    16384u

/* 超时门限 T 的复位值（D30） */
#define GFX_TMO_RESET    4096u

#endif
