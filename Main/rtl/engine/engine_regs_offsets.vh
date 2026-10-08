// 由 tools/check/regs_gen.py 生成（D51 的单一真源），不要手改；
// 语义见 Main/doc/赛题二总体设计.md 3.4 节与 spec 第 02 章。
`ifndef GFX_ENGINE_REGS_OFFSETS_VH
`define GFX_ENGINE_REGS_OFFSETS_VH

// 寄存器偏移（APB 第 0 页内 12 位地址）
localparam [11:0] EC_REG_ID = 12'h000;
localparam [11:0] EC_REG_CAPS = 12'h004;
localparam [11:0] EC_REG_CTRL = 12'h008;
localparam [11:0] EC_REG_STATUS = 12'h00C;
localparam [11:0] EC_REG_IRQ_EN = 12'h010;
localparam [11:0] EC_REG_IRQ_STAT = 12'h014;
localparam [11:0] EC_REG_ERROR_INDEX = 12'h018;
localparam [11:0] EC_REG_ERROR_TAG = 12'h01C;
localparam [11:0] EC_REG_LIST_BASE = 12'h020;
localparam [11:0] EC_REG_LIST_COUNT = 12'h024;
localparam [11:0] EC_REG_DOORBELL = 12'h028;
localparam [11:0] EC_REG_SURF0 = 12'h040;
localparam [11:0] EC_REG_PERF0 = 12'h100;

// 表面表与性能计数器的下标
localparam [4:0]  EC_SURF_SLOTS = 5'd16;
localparam [3:0]  EC_PERF_COUNT = 4'd9;

// 性能计数器下标（0x100 + 4*i）
localparam [3:0] EC_PERF_CMD = 4'd0;
localparam [3:0] EC_PERF_PIXEL = 4'd1;
localparam [3:0] EC_PERF_KEY_SKIP = 4'd2;
localparam [3:0] EC_PERF_RD_BURST = 4'd3;
localparam [3:0] EC_PERF_WR_BURST = 4'd4;
localparam [3:0] EC_PERF_RD_BEAT = 4'd5;
localparam [3:0] EC_PERF_WR_BEAT = 4'd6;
localparam [3:0] EC_PERF_WAIT_DDR = 4'd7;
localparam [3:0] EC_PERF_BUSY = 4'd8;

// ID 与版本
localparam [31:0] EC_ID_VALUE = 32'h47465801;
localparam [7:0]  EC_ID_VERSION = 8'h01;

// CAPS 位位号
localparam EC_CAPS_KEY_LO = 0;
localparam EC_CAPS_KEY_W  = 1;
localparam EC_CAPS_KEY_VALUE = 1;
localparam EC_CAPS_ALPHA_PM_LO = 1;
localparam EC_CAPS_ALPHA_PM_W  = 1;
localparam EC_CAPS_ALPHA_PM_VALUE = 1;
localparam EC_CAPS_ALPHA_ST_LO = 2;
localparam EC_CAPS_ALPHA_ST_W  = 1;
localparam EC_CAPS_ALPHA_ST_VALUE = 1;
localparam EC_CAPS_MUL_LO = 3;
localparam EC_CAPS_MUL_W  = 1;
localparam EC_CAPS_MUL_VALUE = 1;
localparam EC_CAPS_TINT_LO = 4;
localparam EC_CAPS_TINT_W  = 1;
localparam EC_CAPS_TINT_VALUE = 0;
localparam EC_CAPS_BAND_LO = 5;
localparam EC_CAPS_BAND_W  = 1;
localparam EC_CAPS_BAND_VALUE = 0;
localparam EC_CAPS_TEX_LO = 6;
localparam EC_CAPS_TEX_W  = 1;
localparam EC_CAPS_TEX_VALUE = 1;
localparam EC_CAPS_INDEX8_LO = 7;
localparam EC_CAPS_INDEX8_W  = 1;
localparam EC_CAPS_INDEX8_VALUE = 0;
localparam EC_CAPS_LANES_LO = 8;
localparam EC_CAPS_LANES_W  = 4;
localparam EC_CAPS_LANES_VALUE = 4;
localparam [31:0] EC_CAPS_VALUE = 32'h0000044F;

// CTRL 位位号（SOFT_RST/CNT_CLR 写 1 自清）
localparam EC_CTRL_EN_BIT = 0;
localparam EC_CTRL_SOFT_RST_BIT = 1;
localparam EC_CTRL_CNT_CLR_BIT = 2;

// STATUS 位位号与错误码字段
localparam EC_STATUS_BUSY_BIT = 0;
localparam EC_STATUS_DRAINING_BIT = 1;
localparam EC_STATUS_DONE_BIT = 2;
localparam EC_STATUS_ERR_BIT = 3;
localparam EC_STATUS_CODE_HI = 7;
localparam EC_STATUS_CODE_LO = 4;

// 中断位位号（IRQ_STAT 写 1 清）
localparam EC_IRQ_LIST_DONE_BIT = 0;
localparam EC_IRQ_ERROR_BIT = 1;

// 错误码值（错误码字段 [STATUS_CODE_HI:LO] 的取值）
localparam [3:0] EC_ERR_OK = 4'd0;
localparam [3:0] EC_ERR_BAD_OPCODE = 4'd1;
localparam [3:0] EC_ERR_BAD_FIELD = 4'd2;
localparam [3:0] EC_ERR_BAD_EQUATION = 4'd3;
localparam [3:0] EC_ERR_BAD_SURFACE = 4'd4;
localparam [3:0] EC_ERR_BAD_SPACE = 4'd5;
localparam [3:0] EC_ERR_BAD_FORMAT = 4'd6;
localparam [3:0] EC_ERR_OVERLAP = 4'd7;
localparam [3:0] EC_ERR_BAD_PREMULT = 4'd8;
localparam [3:0] EC_ERR_FETCH = 4'd9;
localparam [3:0] EC_ERR_BUS_READ = 4'd10;
localparam [3:0] EC_ERR_BUS_WRITE = 4'd11;
localparam [3:0] EC_ERR_TIMEOUT = 4'd12;

// TEX 装载窗口（v1-TEX profile，G1 的 D57）：APB Slave0 页内偏移，
// 不是第 0 页寄存器表的 12 位偏移；CPU 地址 = 0xF810_0000 + EC_TEX_WINDOW。
localparam [15:0] EC_TEX_WINDOW = 16'h4000;
localparam [31:0] EC_TEX_BYTES  = 32'd16384;

// 超时门限 T 的复位值（D30；本表不设 T 寄存器，B6 若要可写再扩 CTRL）
localparam [12:0] EC_TMO_RESET = 13'd4096;

`endif
