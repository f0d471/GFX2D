# display 设计文档

`rtl/display/` 的随码文档：扫描输出三模块的寄存器表、定值与跨域协议。
取数侧 `display_fetch`（core_clk 域）、异步 FIFO `display_fifo`、像素侧 `display_out`
（像素时钟域）；寄存器窗口在 APB Slave 0 的 0xE000~0xEFFF（`gfx2d_top` 按 paddr[15:12]==4'hE 分选）。

## 1 display_fetch 寄存器表

偏移为窗口内 12 位偏移（CPU 地址 0xF810_E000 起）。

| 偏移 | 名称 | 读写 | 复位值 | 内容 |
| --- | --- | --- | --- | --- |
| 0x000 | ID | 只读 | — | 版本标识 |
| 0x004 | VERSION | 只读 | — | 接口版本 |
| 0x008 | CTRL | 读写 | 0 | bit0 EN、bit1 SRC（1 帧缓冲/0 彩条，场起始生效）、bit2 CLEAR（清两侧统计） |
| 0x00C | FRONT | 读写 | 0x0200_0000 | 帧缓冲 0 基址（读侧） |
| 0x010 | BACK | 读写 | 0x0240_0000 | 帧缓冲 1 基址（读侧） |
| 0x014 | SWAPREQ | 读写 | 0 | 写 1 请求在下一场起始交换前/后基址（写 0 撤销）；读回请求 pending 位 |
| 0x018 | MAX_FIRST_WAIT | 只读 | 0 | AR 握手到首个 R 拍的最大周期数 |
| 0x01C | MAX_FULL_TXN | 只读 | 0 | AR 握手到 rlast 的最大周期数 |
| 0x020 | MAX_R_GAP | 只读 | 0 | 相邻 R 拍之间的最大周期数 |
| 0x024 | TXN_COUNT | 只读 | 0 | 完成的读事务数 |
| 0x028 | LINE_COUNT | 只读 | 0 | 场内行序号 |
| 0x02C | R_BEAT_COUNT | 只读 | 0 | 收到的 R 拍数 |
| 0x030 | SKIP_COUNT | 只读 | 0 | 水位不足跳过的行数 |
| 0x034 | RESPERR_COUNT | 只读 | 0 | 非 OKAY 读响应计数 |
| 0x038 | STATUS | 只读 | 0 | bit0 BUSY、bit1 AR_PENDING |
| 0x03C | HIST_FIRST[12] | 只读 | 0 | 首拍等待直方图 |
| 0x06C | HIST_FULL[12] | 只读 | 0 | 整笔时间直方图 |
| 0x09C | HIST_GAP[12] | 只读 | 0 | R 拍间隙直方图 |
| 0x0D0 | IRQ_EN / 0x0D4 IRQ_STAT | 读写 | 0 | 场同步中断，STAT 写 1 清 |
| 0x0D8 | FRAME_CNT | 只读 | 0 | 场计数 |
| 0x0DC | SWAP_CNT | 只读 | 0 | 交换次数 |
| 0x0E0 | SNAP_FRAME / 0x0E4 SNAP_MIN | 只读 | 0 | 帧快照：场号与该场最低水位 |
| 0x0E8 | ZERO | 只读 | 0 | 像素侧零拍计数（恒应为 0 的判据） |
| 0x0EC | UNDER | 只读 | 0 | FIFO 欠载拍计数（恒应为 0 的判据） |
| 0x0F0 | FRAME_BEAT | 只读 | 0 | 每场 R 拍数（应恒为 76800） |
| 0x0F4 | END_LEVEL | 只读 | 0 | 场末残留水位（正常恒为 0） |

直方图分桶与计数约定同 bus-manual.md 的 scanout_stub 一节。

## 2 display_fifo 定值

DEPTH=512 拍（约 3 行）、DATA_WIDTH=128、SYNC_CLK=0（真异步）、OUTPUT_REG=0
（读延迟恰 1 个 rd 周期）、ASYM_WIDTH_RATIO=4、RAM_STYLE="block_ram"、SYNC_STAGE=2。
纪律：a_rst_i 是唯一复位；复位窗口 FIFO 不拦写，wr_en 由调用方压住；
判复位用 rst_busy_sync（连续低 4 拍）而不是裸 rst_busy。

## 3 display_out 输出对齐

FIFO 读延迟 1 个 rd 周期，所以 `rd_en = de0 & (psel==0) & ~empty` 只在每组的第一个
像素周期弹拍；`psel_d1` 与时序延迟一拍，像素 = `rd_data[psel_d1]`。
每行 160 次、每场 76800 次弹出，与取数供给一致；相位改成 psel==3 会每场多弹一次。
行号用自己的行计数器（de0 上升沿加一），不取 color_bar_rgb 的 v_cnt（比 de 早一拍）。

## 4 跨域通道

core → pixel：`src_sel` 电平（场起始变化，两级同步后像素侧再锁存一次）、
`clear_toggle`（toggle + 两级同步，清像素侧累计值）。
pixel → core：`fs_toggle`（场起始）、`snap_toggle` + 快照总线（toggle/ack，
ack 按电平追 toggle，接收确认前源侧保持不变）。
像素侧复位 `rst_pixel_n` 由 hdmi_tx 的 vid_rst_n 给出（PLL 锁定后同步释放）。
