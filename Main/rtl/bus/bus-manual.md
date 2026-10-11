# bus 设计文档

`rtl/bus/` 的随码文档：各 APB 附件的寄存器表与专用语义。
地址窗口分配（APB Slave 0，CPU 地址 0xF810_0000 起）：engine 0x0000~0x0FFF、
scanout_stub 0xE000~0xEFFF、axi_mux_guard 0xF000~0xFFFF、axi_probe_master 0x1000~0x1FFF；
其余页默认应答 PREADY=1、PSLVERR=1、PRDATA=0。研究背景在
`Engineering/TechReports/platform/`（E4 第 03 章、E5 第 06 章）。

## 1 apb_probe（已从顶层撤下）

| 偏移 | 名称 | 读写 | 内容 |
| --- | --- | --- | --- |
| 0x0000 | ID | 只读 | 32'h4746_5832（ASCII "GFX2"） |
| 0x0004 | VERSION | 只读 | 32'h0000_0001 |
| 0x0008 | SCRATCH | 读写 | 复位值 0，写什么读回什么 |
| 其余 | — | — | 读回 0；读写都置 PSLVERROR |

16 位地址全部参与译码；写只读寄存器也置 PSLVERROR 且寄存器不变。PREADY 恒 1。

## 2 axi_mux_guard

| 偏移 | 名称 | 读写 | 复位值 | 内容 |
| --- | --- | --- | --- | --- |
| 0x000 | ID | 只读 | 32'h4D55_5847 | ASCII "MUXG" |
| 0x004 | VERSION | 只读 | 2 | 接口版本（2：MAX 改为运行中取最大） |
| 0x008 | CTRL | 读写 | 3 | bit0 ARB_TYPE（1 优先级）、bit1 GUARD_EN |
| 0x00C | CHANNEL_MASK | 读写 | 7 | 三位用户掩码：bit0 S0、bit1 S1、bit2 S2 |
| 0x010 | AGE_LIMIT | 读写 | 64 | 保护触发阈值，单位 core_clk 周期 |
| 0x014 | STATUS | 只读 | 0 | bit0 protect_ar、bit1 protect_aw |
| 0x018 | GUARD_HITS | 只读 | 0 | 两个 protect 的 0→1 次数之和 |
| 0x01C | S2_MAX_WAIT_AR | 只读 | 0 | SoC AR 等待的最长连续周期数（含未握手那笔） |
| 0x020 | S2_MAX_WAIT_AW | 只读 | 0 | SoC AW 等待的最长连续周期数（含未握手那笔） |
| 0x024 | CLEAR | 只写 | — | bit0 写 1 清 GUARD_HITS 与两个 MAX |

保护逻辑：每方向一个等待计数器（valid 高 ready 低加一、饱和；握手或 valid 撤销清零），
达到 AGE_LIMIT 锁存 protect，保持到该方向握手或撤销。
送 Axi_Mux 的 `Cfg_Cnl_Mask = CHANNEL_MASK & {1'b1, ~(protect_ar|protect_aw), 1'b1}`；
S0、S2 永不被清，GUARD_EN=0 时掩码恒全 1（只用于注错）。
掩码只参与新通道搜索，已锁存的授权与在途事务不受影响，不撤 valid 不违反 AXI。
两个 MAX 每拍更新：只在握手拍取值时，SoC 永不握手的那笔看不到（版本 2 修订的原因）。

## 3 axi_probe_master（已从顶层撤下）

| 偏移 | 名称 | 读写 | 内容 |
| --- | --- | --- | --- |
| 0x00 | CTRL | 写 | bit0 START 开始一轮；bit1 ABORT 立即回空闲（撤 valid，只用于救场） |
| 0x04 | STATUS | 只读 | bit0 BUSY、bit1 DONE、bit2 TIMEOUT、bit3 RESP_ERR、bit4 LAST_ERR（rlast 位置不对） |
| 0x08 | ADDR | 读写 | 第一笔事务起始地址，16 的倍数 |
| 0x0C | LEN | 读写 | [7:0] 送 awlen/arlen，每笔 LEN+1 拍，每拍 128 位 |
| 0x10 | COUNT | 读写 | 本轮事务数 |
| 0x14 | MODE | 读写 | bit0 WRITE、bit1 INC（地址每笔加 (LEN+1)*16）、bit2 CHECK（读比对图样） |
| 0x18 | SEED | 读写 | 数据图样种子 |
| 0x1C | TIMEOUT | 读写 | 连续这么多个周期无握手判超时，复位值 0x0010_0000 |
| 0x20 | TX_DONE | 只读 | 已完成事务数 |
| 0x24 | CYCLES | 只读 | START 到最后一笔完成的周期数 |
| 0x28 | LAT_SUM | 只读 | 每笔从地址 valid 到首拍数据/写响应的周期数之和 |
| 0x2C | ERRS | 只读 | [15:0] 非 OKAY 计数；[31:16] 读比对失配拍数 |
| 0x30 | RDATA0/0x34 RDATA1 | 只读 | 最近一笔读事务第一拍的 [31:0]/[63:32] |
| 0x38 | ARB | 读写 | bit0 送 Cfg_Arb_Type；bit[2:1] 送 Cfg_Cnl_Mask，复位值 3'b110 |
| 0x3C | ID | 只读 | 32'h4158_4950（ASCII "AXIP"） |
| 其余 | — | — | 读回 0，读写都置 PSLVERROR |

数据图样：地址 a 拍的 `v = SEED + a`，拍数据 `{v^A5A5A5A5, v^5A5A5A5A, ~v, v}`；
AXI 小端，CPU 在 a 读到 v、a+4 读到 ~v，便于核对。
配置只在空闲时可写；超时后保持 valid 不撤销（AXI 要求），只停计数置 TIMEOUT，
此时 Axi_Mux 被占、CPU 访问不了 DDR，只能 ABORT 或重新加载比特流。

## 4 scanout_stub（已从顶层撤下）

扫描输出的 AXI 替身：按行节拍发一笔 160 拍（128 位、640 像素）读并计时；
只发起读，写通道由顶层接成不发起。

| 偏移 | 名称 | 读写 | 复位值 | 内容 |
| --- | --- | --- | --- | --- |
| 0x000 | ID | 只读 | 32'h5343_414E | ASCII "SCAN" |
| 0x004 | VERSION | 只读 | 1 | 接口版本 |
| 0x008 | CTRL | 读写 | 0 | bit0 ENABLE、bit1 CLEAR（写 1 清全部统计） |
| 0x00C | BASE | 读写 | 0x0200_0000 | 读的起始地址（帧缓冲 0） |
| 0x010 | LINE_PERIOD | 读写 | 3175 | 行周期（core_clk 周期，31.7 µs） |
| 0x014 | FIELD_LINES | 读写 | 525 | 场行数 |
| 0x018 | MAX_FIRST_WAIT | 只读 | 0 | AR 握手到首个 R 拍的最大周期数 |
| 0x01C | MAX_FULL_TXN | 只读 | 0 | AR 握手到 rlast 的最大周期数 |
| 0x020 | MAX_R_GAP | 只读 | 0 | 相邻 R 拍之间的最大周期数 |
| 0x024 | TXN_COUNT | 只读 | 0 | 完成的读事务数 |
| 0x028 | LINE_COUNT | 只读 | 0 | 场内行序号 |
| 0x02C | R_BEAT_COUNT | 只读 | 0 | 收到的 R 拍数 |
| 0x030 | SKIP_COUNT | 只读 | 0 | 该发行时上一笔未完成而跳过的行数 |
| 0x034 | RESPERR_COUNT | 只读 | 0 | 非 OKAY 读响应计数 |
| 0x038 | STATUS | 只读 | 0 | bit0 BUSY、bit1 AR_PENDING |
| 0x03C | HIST_FIRST[12] | 只读 | 0 | 首拍等待直方图 |
| 0x06C | HIST_FULL[12] | 只读 | 0 | 整笔时间直方图 |
| 0x09C | HIST_GAP[12] | 只读 | 0 | R 拍间隙直方图 |

直方图分桶：桶 k 计 [2^k, 2^(k+1)−1]，桶 11 计 ≥2048；0 与 1 同落桶 0。
时间量单位 core_clk 周期，从 AR 握手拍算起，下一拍就返回数据记 0；
R 拍间隙按相邻两次 rvalid 握手之间的周期数记。每方向在途不超过 1。
