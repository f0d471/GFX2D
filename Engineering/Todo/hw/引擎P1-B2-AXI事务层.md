# 引擎 P1 · B2：AXI 事务层（搬运层）

状态：**未开工**。没有硬性前置，排在 B1 之后；B3、B4 要用它的描述符与流接口（总览 2.4 节 I2、I3、I4），
B5 的命令取数要用 I7。路线、层间接口与批次顺序见 [引擎P1总览.md](引擎P1总览.md)。

## 1 要解决什么

**这一层做什么。** 引擎唯一的 AXI 主机，接 `Axi_Mux` 的 S1：读写两个 DMA 核、读侧命令取数与像素读的仲裁、
每个方向最多一笔在途的门控、W 通道的带内掩码、复位时的排空与超时。上面接几何层的描述符（I2）与控制层的取数、排空请求（I7），
旁边接计算阵列的读写流（I3、I4）。

**现在是什么样。** 两个核已经选定（D3）并在仓库外做过前期研究：综合、在途门控、带内掩码、校准到 E4 板上数的定时从机，
数据在第 4 节。前期研究的搬运层骨架、从机与测试台在仓库外的临时目录里，没有入库。
仓库里的从机 `sim/axi_probe/axi_ram_model.v` 缺四样（总览 2.3 节 D13）。

**不做会怎样。** 这一层出错的代价最大：S1 上一笔断在半截，`Axi_Mux` 会一直等它，扫描输出与 SoC 跟着停，只能重新加载比特流。
总览第 1 节的在途契约、地址与突发约束都落在这里。

## 2 怎么切分

### 2.1 已定的取舍

**D3 搬运层直接用 verilog-axi 的 `axi_dma_rd.v` 与 `axi_dma_wr.v`，源码一字不改。**
来源：[alexforencich/verilog-axi](https://github.com/alexforencich/verilog-axi)，MIT 许可，
取仓库归档前最后一个提交 `516bd5dadc3365b7f9e225d2af8fe0b8d804fe53`（2025-02-27）。
仓库已归档，后继项目 taxi 是 CERN-OHL-S（强传染）许可，不用。
两个文件放 `rtl/vendor/verilog-axi/`，连同 LICENSE 原样入库，登记进 `rtl/README.md` 的 vendor 表。

已核对（2026-09-26，首稿与前期研究两轮）：

- 两个文件都是 Verilog-2001，不例化任何子模块；WSL 里 iverilog 12 用 `-g2005` 单独编译都通过
  （`axi_dma_wr.v` 有五条 `@*` 敏感整个数组的告警，不影响功能）。Efinity 2026.1 整机编译四步通过，
  两个核合计 1515 XLR、16 块 RAM，合计 59 条厂商代码常见的告警（第 4 节第 1 问）。
- 接口是描述符（地址、字节长度、标签）进、AXI-Stream 出（读）或进（写），状态从独立的状态口回来；
  写状态的 `error` 字段来自 `bresp`，读状态的 `error` 字段来自 `rresp`。
- 参数取：`AXI_DATA_WIDTH = 128`、`AXI_ADDR_WIDTH = 32`、`AXI_ID_WIDTH = 4`、`AXI_MAX_BURST_LEN = 192`、
  `AXIS_KEEP_ENABLE = 1`、`AXIS_LAST_ENABLE = 1`、`AXIS_ID/DEST/USER_ENABLE = 0`、`LEN_WIDTH = 12`、`TAG_WIDTH = 4`、
  `ENABLE_SG = 0`、**`ENABLE_UNALIGNED = 0`**（D15；首稿取 1，前期研究量到三处代价之后改成 0）。
  前期研究的仿真与编译用的就是这一组。
- 写核中间拍的 `wstrb` 恒为全 1、不看流的 `tkeep`（`axi_dma_wr.v` 第 556 行），只有最后一拍看 `tkeep`；
  按像素跳过写走 W 通道带内掩码（D16）。
- 两个核复位时不收尾：读核把 `rready` 清 0（第 612 行），写核清空输出 FIFO（第 933~937 行）；复位分域与排空见 D17。
  两个核都没有中止功能：`axi_dma_wr` 的 `abort` 只在端口表里，`axi_dma_rd` 没有这个端口。
- 两个核内部各有一个 32 深的输出 FIFO，各占 8 块 RAM；深度写在模块体内的 `parameter` 里，
  Efinity 当它是 localparam，不改源码缩不了（D23 的 RAM 账算进去了）。
- **一个描述符如果要拆成多笔突发（跨 4KB，或超过 `AXI_MAX_BURST_LEN`），读核在上一笔地址握手之后就发下一笔，
  不等数据回完**（`axi_dma_rd.v` 第 362~404 行的 `AXI_STATE_START`）；读核在地址发出后就可以接下一个描述符。
  写核同样：拆出的第二笔 `awvalid` 不等第一笔的写响应（`axi_dma_wr.v` 第 509 行 `!first_cycle_reg` 分支；
  第 4 节的整屏 FILL 注错用例在写方向量到 9120 个违例周期）。
  所以在途契约不能交给核自己保证，定为两条规矩：
  1. 几何层保证每个描述符按 16 字节对齐、不跨 4KB、不超过 `AXI_MAX_BURST_LEN` 拍，核内永不拆分，一个描述符恰好一笔突发；
  2. 搬运层在上一个描述符的状态回来之前不发下一个（读、写各自门控），状态在数据全部交接之后才出。
- 已知缺陷（上游 issue #12）：`AXI_MAX_BURST_LEN × 16 >= 4096` 时 4KB 判断的一个表达式恒真。
  行最长 161 拍，所以取 `AXI_MAX_BURST_LEN = 192`，不触发这个表达式；加上规矩 1，核内的 4KB 分支也不会走到。

规矩 1 由 B3 实现；本层的判据要能抓到它被违反（2.4 节第 1 条的「一个描述符恰好一笔」）。

**D12 在途不放宽。** 每个方向一笔。放宽到两笔能不能省下读的固定开销，取决于加密的 DDR 控制器会不会提前接受地址，
这件事没有数；要放宽得先量、再重做 E5 的判据，本条不做。

**D16 按像素跳过写：W 通道带内标记。** 需要按像素不写的有三处：色键命中、行首行尾不属于本行的像素、
排空时补的哑拍（D17）。写核中间拍不看 `tkeep`（D3 已核对）。

定案：像素通路输出像素的 X 字节（spec 第 01 章规定写回时恒为 0）最高位置 1 表示「不写」；
写核与 `Axi_Mux` 之间加一层组合逻辑，按每个像素 X 字节的最高位把该像素的 4 位 `wstrb` 清零，再把 X 字节清零后上总线。
核的源码不改；前期研究的仿真覆盖了中间拍、首末拍与整行全跳过。
没有采用：改 `axi_dma_wr.v` 第 556 行让中间拍也与 `tkeep` 相与（违反 D3「源码一字不改」，`tkeep` 在核里还参与帧尾判断，
第 358、571、838 行要重验）；色键 BLIT 也读目的、在像素通路里换回原值（D7 第二行的避让失效，机体类每行约多 35 周期）。

本层做「按标记清 `wstrb`」这一半；标记由 B4 的像素通路打。

**D17 复位分域与排空。** 两个核复位时不收尾（D3 已核对）。引擎在事务中途被复位时，读方向迟到的 R 拍没人接，
写方向的突发断在半截，`Axi_Mux` 按信息 FIFO 的顺序一直等这一笔，扫描输出与 SoC 跟着停住。
`soc_system_reset` 在每次 `reset halt` 时都会拉起（display 第 01 章第四节）；扫描输出不受影响，
是因为它的 `r_ready` 恒为 1（`display_fetch.v` 第 207 行）。

定案：搬运层（两个核、门控、排空、W 通道带内掩码）的复位接 `ddr_rstn` 取反，与 `Axi_Mux` 同源，只在上电与 PLL 失锁时复位；
控制层挂 `soc_system_reset` 与软复位。控制层复位或软复位时向搬运层发「排空」：读方向收完在途那一笔并丢弃数据，
写方向用带内标记的哑拍把欠的拍补完、再等写响应；两个方向的状态都回来之后，控制层才清状态、接受新提交。
`STATUS` 里给一位「排空中」。
像素通路与行缓冲挂在控制层复位下，所以两个核的流接口前各有一个归搬运层的二选一：
平时接像素通路；排空期间读流恒 ready、数据丢弃，写流由搬运层自己按「描述符拍数减已送拍数」补上全打「不写」标记的哑拍。
搬运层要为此记住在途写描述符的拍数与已送拍数（复核时补）。
没有采用：全部挂 `soc_system_reset`，约定引擎忙时不做 `reset halt`，违反时只能重新加载比特流。

**D19 错误码、超时与排空的语义。** 两个核没有中止功能；读核照常把出错那笔的数据推出来，写核的错误随写响应回来。
按门控，每个方向的状态都在下一个描述符发出之前回来，所以写方向出错时，下一条命令的第一笔写还没有发出。

定案：

- 码值：10 `BUS_READ`（像素数据读回的 `rresp` 非 OKAY），11 `BUS_WRITE`（写响应非 OKAY），12 `TIMEOUT`；
  命令取数出错仍是 9 `FETCH`。错误归属用描述符随带的命令索引，因为写方向的状态可能晚于下一条命令的读。
- 语义：检查类错误维持「失败命令没有像素副作用」；总线类错误时失败命令可能已写了部分行，不回滚，
  读错的那一行不写，之后的命令没有像素副作用；已经发出的下一条命令的读由排空收掉。完成时补进 spec 第 02 章。
- 超时只报告、不放弃：某方向有描述符在途且连续 `T` 个周期没有任何握手时，置 `TIMEOUT` 粘滞位、发中断 B，之后照常等状态；
  欠总线的拍由 D17 的排空补齐。从机真的不再响应时整条总线已经停住，CPU 也访问不了 DDR，只能重新加载比特流，
  与 `axi_probe_master` 的超时相同。`T` 做成寄存器，复位值按一笔 192 拍的读在扫描输出满负载下的最坏耗时留足余量，开工时定数。

本层产生三种事件并带上命令索引（I7）；码值落到寄存器、粘滞位与中断在 B5。

### 2.2 开工前要补的研究

第 4 节的前期研究已经回答了综合、门控、带内掩码与节拍（第 1、2、6、10 问），这一批补下面几项，结果写进 TechReports `engine/` 第 02 章。

1. **排空与超时的状态机。** 读龙芯仓 MTX 的实现（该仓 TechReports mtx 第 02 章）与 iDMA 后端的错误处理（论文与源码），
   定出排空的状态、与控制层的握手（I7）、超时计数何时清零、排空期间再来一次软复位怎么办。
2. **描述符附带信息放哪。** 目标缓冲号与命令索引（D19）是放进核的 4 位 `tag`，还是搬运层自己寄存；
   每个方向只有一笔在途，搬运层用一个寄存器就能记住，`tag` 位宽不够放命令索引。
3. **读侧仲裁。** 命令取数与像素读共用读核（D8，命令取数优先），读流按目标分给命令缓冲或行缓冲；定仲裁的时机与读流的分发。
4. **verilog-axi 自带的测试覆盖了什么。** 读上游仓库里两个核的测试（cocotb），列出它们测过的反压、4KB、错误响应，
   与本层测试台要自己补的部分。
5. **前期研究骨架的复核。** 仓库外的骨架、从机与测试台逐段读一遍再搬进 `sim/engine/`，前期研究只求出数，没有按入库的标准写。
6. **从机补四样的写法**（D13）：4 MiB 存储、`wstrb`、按地址区间回 `SLVERR`、定时模式；新增参数的默认值保持现有行为。

研究之后要定的（编号 D24 起）：排空状态机、`T` 的复位值、附带信息的存放、读侧仲裁的细节。

### 2.3 RTL

| 文件 | 内容 |
| --- | --- |
| `rtl/engine/engine_mover.v` | 两个 verilog-axi 核（参数见 D3）、读侧仲裁（命令取数优先）、在途门控、W 通道带内掩码（D16）、两个流接口前的排空二选一与超时（D17、D19）；整块挂搬运层复位 |
| `rtl/vendor/verilog-axi/` | 两个核与上游 LICENSE，原样入库 |

### 2.4 仿真

新目录 `sim/engine/mover/`。从机是补齐四样之后的 `sim/axi_probe/axi_ram_model.v`；`Axi_Mux` 用仓内源码加 `EFX_SRL8` 厂商模型；
测试台扮演几何层（发描述符）、计算阵列（收读流、送写流）与控制层（发排空请求）。

1. **协议**：契约按原文判——上一笔没结束（读看 `rlast` 握手、写看 `bvalid` 握手）时下一笔的 `arvalid`/`awvalid` 拉高一个周期即违例，
   违例恒为 0；不按握手次数数在途笔数（从机一次只收一个地址时那个数恒为 1，是空转判据，第 4 节第 2 问）。
   另外：每笔地址 16 字节对齐、不跨 4KB、每个描述符恰好一笔突发；`WLAST` 与 `AWLEN` 对得上；
   读数据阶段 S1 的 `rready` 恒为 1；随机反压下不死锁。
2. **复位与排空**：读突发中途、写突发中途各做一次软复位与一次控制层复位（模拟 `reset halt`）：排空完成，总线上没有断掉的突发
   （R 拍全部收完、`WLAST` 对账、写响应全部收到），之后能再次提交。
3. **错误与超时**：像素读回 `SLVERR` 与写响应 `SLVERR` 各一例，状态里的错误与命令索引正确；
   从机迟到超过门限一例：报超时，迟到的响应到达后排空回到空闲、能再次提交，迟到的响应不被新描述符认领。
4. **注错**：去掉在途门控、W 通道带内掩码旁路（`wstrb` 直通）、排空旁路（复位时直接清核），各自必须见红。
5. **从机回归**：补齐四样之后 `sim/axi_probe/` 与 `sim/axi_mux_guard/` 的判据不变。

### 2.5 单独综合

本层单独综合，XLR 与 RAM 和第 4 节第 1 问的数（两个核 1515 XLR、16 块）对账；整机编译在 B6。
两个核的厂商告警（每份编译约 59 条，位宽截断与「模块体内的 parameter 变成 localparam」两类）在告警检查里单列，不算新增告警。

### 2.6 文档

`rtl/README.md` 登记 `engine/` 与 `vendor/verilog-axi/`；`sim/README.md` 登记 `engine/` 与 `axi_ram_model.v` 的新参数；
D19 的码值与总线类错误的副作用在 B5 完成时补进 spec 第 02 章。

### 借鉴来源

| 来源 | 形态与许可 | 借鉴什么 | 用在 | 不借鉴什么 |
| --- | --- | --- | --- | --- |
| [verilog-axi](https://github.com/alexforencich/verilog-axi) `axi_dma_rd.v`、`axi_dma_wr.v` | Verilog-2001，MIT | 代码原样用：突发、4KB 判断、错误状态、输出 FIFO 与反压 | 本层 | 核内拆分多笔突发的能力（D3 规矩 1 让它不发生）；`ENABLE_SG`；`ENABLE_UNALIGNED`（D15 取 0）；中间拍靠 `tkeep` 出掩码（核不支持，D16） |
| Loongarch_NPU_SoC 仓的 MTX 共享搬运层（该仓 TechReports mtx 第 02 章） | 同作者的另一个仓库，未公开 | 读方向超时后吃到 `rlast`、写方向用字节使能全 0 的哑拍补完、写响应超时不放弃（否则迟到的响应被下一条命令认领） | D17 的排空、D19 的超时 | 代码（总线与接口不同） |
| `sim/axi_probe/axi_ram_model.v` | 本仓 | 随机反压、响应延迟、两种注错 | 2.4 节的从机，补四样（D13） | — |

2.2 节第 1、4 项读过的实现研究时补进本表。

## 3 判据

| 项 | 判据 |
| --- | --- |
| 在途契约 | 上一笔没结束时下一笔的 `arvalid`/`awvalid` 拉高的周期数恒为 0（按 valid 判，不按握手计数）；每笔地址 16 字节对齐、不跨 4KB、一个描述符恰好一笔；读数据阶段 `rready` 恒为 1；去掉门控的注错版必须见红 |
| 写掩码（机制） | 带内标记的像素 `wstrb` 清零、X 字节清零后上总线；中间拍稀疏、首末拍、整行全跳过与排空哑拍都不写；带内掩码旁路的注错版必须见红 |
| 错误与超时 | 读、写 `SLVERR` 各一例，错误与命令索引正确；迟到超过门限一例报超时，迟到响应到达后能再次提交且不被新描述符认领 |
| 复位（仿真） | 读、写突发中途软复位与控制层复位各一例：排空完成、总线上没有断掉的突发、之后能再次提交；排空旁路的注错版必须见红 |
| 从机回归 | `sim/axi_probe/` 与 `sim/axi_mux_guard/` 的判据不变 |
| 单独综合 | XLR 与 RAM 和两个核的 1515 XLR、16 块对账 |
| 文档 | TechReports `engine/` 第 02 章写完研究、实现与数据 |

## 4 开发日志

### 2026-09-28 从原条目拆出

D3、D12、D16、D17、D19，原 2.3 节 `engine_mover.v` 与 vendor 两行，原 2.4 节第 4、7 条与第 6、8 条里属于本层的部分，
原第 3 节「在途契约」「写掩码」「错误通路」「复位」四行里属于本层的部分，原借鉴来源 verilog-axi、MTX、`axi_ram_model.v` 三行，
以及原条目前期研究的做法与第 1、2、6、10 问，搬入本条。2.2 节的研究清单是新增；D13 的从机补齐在本条做，原文在总览 2.3 节。
下面「2026-09-26 前期研究」一节原样保留，节号指原条目的节；原文的「2.4 节第 4 条」即本条 2.4 节第 1 条，「2.4 节第 5 条」即本条 2.4 节第 3 条。

### 2026-09-26 前期研究（原条目 2.2 节第 1、2、6、10 问）

**做法。** 两个核取 `516bd5d`，源码不改。实验全部在仓库外的临时目录里做，仓库里只改了本条目。

- 仿真：WSL 里 iverilog 12。自写三样东西：从机模型（128 位、带 `wstrb`、1 MiB，随机反压与定时两种节拍）、
  搬运层骨架（两个核、描述符门控、W 通道带内掩码、在途契约监视）、三个测试台。
  `Axi_Mux` 用仓内源码加 Efinity 的 `EFX_SRL8` 仿真模型（platform 第 03 章研究 5）。
- 编译：把 `Main/rtl` 与 `Main/fpga/gfx2d_ti60` 复制到临时目录，顶层把 `u_axi_probe` 换成一个端口相同的综合探针
  （两个核、门控、带内掩码、深 256 与深 512 的 128 位行缓冲各一个、4 个位置的像素旋转与色键比较，全部经 APB 可观测），
  `ENABLE_UNALIGNED` 取 0 与 1 各编一份，Efinity 2026.1 四步。

原来的十个问题：

1. Efinity 2026.1 能否综合 `axi_dma_rd.v`/`axi_dma_wr.v`：资源、有没有推断出 RAM、`core_clk` 下的时序；两者的 `rst` 是高有效同步复位，与顶层复位的接法。
2. 描述符门控在仿真里的实测：随机反压下读、写各自的在途笔数最大值是否恒为 1；去掉门控的注错版必须见红。
3. 行缓冲的深度与片上 RAM（B4）。
4. 读侧最后一拍的 `tkeep` 与行内像素的对应关系（B4）。
5. 命令批量的条数与命令缓冲的存放；`PERF_*` 的快照时机（B5）。
6. 数据通路总线错误与超时的错误码；超时之后怎么排空：两个核都没有中止功能，
   参考 `axi_probe_master` 的 `ABORT` 实测（platform 第 03 章 3.3 节）与龙芯 MTX 的「写侧等响应超时不排空」结论，定出不破坏协议的恢复方式。
7. `apb_probe` 的去留（B6）。
8. 用到 S1 探针的板上程序清单（B6）。
9. 测试台的期望值怎么进 Verilog（B5）。
10. P1 每颗 16×16 ALPHA 弹幕的周期数，用 E4 的事务模型在仿真里预估，与总览第 1 节的约 1100 对照。

#### 第 1 问：综合、资源、时序、复位

两份都是 map、interface、pnr、pgm 四步 PASS，没有 error。

| 构型 | 整机 XLR | 片上 RAM | `u_rd` XLR / 块 | `u_wr` XLR / 块 | S1 上的模块合计 | `core_clk` setup | `core_clk` hold |
| --- | --: | --: | --: | --: | --: | --: | --: |
| 主线（探针主机） | 21935 | 71 | — | — | 1164 / 0 | 3.516 | 0.026 |
| `ENABLE_UNALIGNED = 0` | 22583 | 101 | 388.5 / 8 | 1126.5 / 8 | 2021 / 30 | 3.827 | 0.026 |
| `ENABLE_UNALIGNED = 1` | 24763 | 101 | 1404.5 / 8 | 1793.5 / 8 | 4063 / 30 | 3.648 | 0.026 |

- 两个核合计 1515 XLR 对 3198 XLR，差的 1683 是两个 16 位置的字节移位器（读核第 270 行、写核第 343~358 行）。
- 每个核 8 块 RAM，全是内部 32 深的输出 FIFO：数据 128 位映射成 6 块 512×20 加 1 块 1024×8，`tkeep`/`wstrb` 16 位再 1 块。
  深度 `OUTPUT_FIFO_ADDR_WIDTH = 5` 写在模块体内，Efinity 报 `parameter 'OUTPUT_FIFO_ADDR_WIDTH' becomes localparam`，
  不改源码改不了。
- 时序：`ENABLE_UNALIGNED = 0` 那份 `core_clk` 最差路径在 SoC 的 APB 响应（3.827 ns），S1 上的模块不在前列；
  `= 1` 那份最差路径是「512 深行缓冲读出 → 旋转与色键 → 写核字节移位器 → 写核输出 FIFO」，3.648 ns。hold 三份都是 0.026 ns。
- 告警：两个核在 `= 0`/`= 1` 下各出 59/67 条，其中位宽截断 29/37 条、小存储映射成逻辑 5 条、
  「模块体内的 parameter 变成 localparam」25 条，都在厂商代码里，与 DDR 控制器那批告警同类。
- 复位：两个核的 `rst` 高有效、同步，只复位控制寄存器（数据寄存器不复位）。读核复位时 `m_axi_rready_reg` 清 0（第 612 行），
  写核复位时输出 FIFO 的读写指针清 0、`m_axi_wvalid_reg` 清 0（第 933~937 行），没有收尾动作。
  这就是 D17 的由来：`soc_system_reset` 在每次 `reset halt` 时都会拉起，而 `Axi_Mux` 用的是 `ddr_rstn`（顶层第 725 行），
  事务中途复位两个核会把 `Axi_Mux` 留在等这一笔的状态。

#### 第 2 问：门控与在途契约

测试台每次跑 400 个操作：从低 512 KiB 随机取源地址（4 字节对齐）、1~640 个像素（每 7 个操作里有一个只有 1~6 个像素），
截到不跨 4KB；目的在高 512 KiB，同样截断；每个像素以约 3/8 的概率打「不写」标记，每 5 个操作里有一个全不打、一个全打。
读、写两个方向并发：写第 k 个操作时，读已经在取第 k+1 个。从机每个 ready/valid 以 3/8 的概率拉低；
`rndready` 两档让读流的消费者只有 1/4 的周期 ready。判据：读回的像素逐个对影子内存，结束后目的区 131072 个字逐字对影子内存；
每个描述符恰好一笔突发、不跨 4KB、`WLAST` 位置对；契约按原文判，上一笔没结束时下一笔的 valid 拉高一个周期记一次违例。

```text
unal1: RESULT: PASS  | STAT ops=400 cycles=32537 ar=400 aw=400 max_inflight=1/1 viol=0/0 unal_addr=578 cross4k=0 wlast_err=0
unal0: RESULT: PASS  | STAT ops=400 cycles=32533 ar=400 aw=400 max_inflight=1/1 viol=0/0 unal_addr=0 cross4k=0 wlast_err=0
unal1_rndready: RESULT: PASS  | STAT ops=400 cycles=72005 ar=400 aw=400 max_inflight=1/1 viol=0/0 unal_addr=578 cross4k=0 wlast_err=0
unal0_rndready: RESULT: PASS  | STAT ops=400 cycles=70003 ar=400 aw=400 max_inflight=1/1 viol=0/0 unal_addr=0 cross4k=0 wlast_err=0
unal1_seed2: RESULT: PASS  | STAT ops=400 cycles=31193 ar=400 aw=400 max_inflight=1/1 viol=0/0 unal_addr=602 cross4k=0 wlast_err=0
inj_nogate_u1: RESULT: FAIL (1)  | STAT ops=400 cycles=31291 ar=400 aw=400 max_inflight=1/1 viol=28574/8269 unal_addr=578 cross4k=0 wlast_err=0
inj_nogate_u0: RESULT: FAIL (1)  | STAT ops=400 cycles=31291 ar=400 aw=400 max_inflight=1/1 viol=28725/8212 unal_addr=0 cross4k=0 wlast_err=0
inj_tkeep_u1: RESULT: FAIL (1)  | STAT ops=400 cycles=32537 ar=400 aw=400 max_inflight=1/1 viol=0/0 unal_addr=578 cross4k=0 wlast_err=0
SIM OK
```

读法：

- 五个正常配置全过，门控下两个方向的违例都是 0。
- 去掉门控的两个注错版，读方向 2.8 万多个违例周期、写方向 8 千多个，判据见红。
- **但 `max_inflight` 在注错版里仍然是 1。** 从机一次只收一个地址，第二个 `arvalid` 拉起来了也握不上手，
  按握手计数的在途笔数永远到不了 2。第一版测试台正是这样写的，两个去门控的注错版都报了 PASS；
  改成按 valid 判之后才见红。原条目判据里「在途计数器恒不超过 1」就是第一版的写法，定案时已据此改写。
- `inj_tkeep_u1` 把「不写」从带内标记改成在写流的 `tkeep` 上挖洞，结束比对报 `22825 dst words differ, first at 0x00081188 got 00c7e87f want c7205dca`：
  应当保持原值 `c7205dca` 的像素被写成了源像素 `00c7e87f`。这是 D7 注与 D16 的依据。
- `rndready` 两档总周期从 3.3 万涨到 7.2 万、数据仍全对：消费者慢时读核在输出 FIFO 半满后拉低 `rready`。
  到了真总线上，`Axi_Mux` 按读地址的顺序回数据，S1 的 `rready` 一低，排在后面的扫描输出与 SoC 的读数据也跟着停，
  所以读流的消费者（行缓冲写口）必须恒 ready，读描述符只在目标行缓冲有整行空间时发出（D23 的 A 满足这一条）。
- `unal_addr`：`ENABLE_UNALIGNED = 1` 时 800 笔突发里 578 笔（另一个种子 602 笔）的起始地址不按 16 字节对齐，直接发上总线；
  `= 0` 时是 0。

#### 第 6 问：错误码、超时与排空

- 两个核都没有中止：`axi_dma_wr` 的 `abort` 只出现在端口表（第 139 行），`axi_dma_rd` 没有这个端口。
  `enable` 只挡新描述符，不影响在途的一笔。
- 错误上报：读核把一笔里见到的 SLVERR/DECERR 记下，在最后一拍输出时随状态给出（码 4/5，第 518~533 行），出错那笔的数据照常推出来；
  写核的错误随写响应回来（码 6/7，第 755~784 行）。读状态在最后一个输出拍给出，所以门控按状态放行，
  等价于「`rlast` 握手之后才发下一笔」，只晚一两个周期。
- 参考：`axi_probe_master` 超时后不撤 valid，只能 `ABORT`（撤 valid）或重新加载比特流救场（platform 第 03 章 3.3 节）；
  龙芯仓 MTX 的做法是读方向吃到 `rlast`、写方向用字节使能全 0 的哑拍补完，写响应超时不放弃，
  否则迟到的写响应会被下一条命令认领。本仓的门控保证下一个写描述符在状态回来之前不发，只要从不放弃，迟到的响应就只会配给原来那一笔。
- 由此得到 D19 的码值、语义与超时处理，以及 D17 的排空；超时的判据据此改写（本条 2.4 节第 3 条）。

#### 第 10 问：每颗弹幕的周期数

**先把从机校准到 E4 的板上数。** 用仓内 `axi_probe_master` 经真实 `Axi_Mux`（X3、优先级模式）打定时从机，
重复 platform 第 03 章 5.4 节的突发扫描（每档 16~64 笔），调从机的读首拍延迟、读空拍间隔、写空拍间隔、写响应延迟。
取定 `RD_LAT=23 RD_BUB=12 WR_BUB=6 B_LAT=1`：

```text
CAL RD_LAT=23 RD_BUB=12 WR_BUB=6 B_LAT=1
N=1  write 9.02 (lat 8.00)  read 29.02 (first 28.00)
N=4  write 12.02 (lat 11.00)  read 32.02 (first 28.00)
N=16  write 26.02 (lat 25.00)  read 45.02 (first 28.00)
N=256  write 306.06 (lat 305.00)  read 305.06 (first 28.00)
FIT write t_tx=7.60 k=1.166   read t_tx=27.64 k=1.083   (board: 7.25/1.183, 27.41/1.086)
```

板上同几档是写 8.31/11.66/26.56/311.16、读 29.02/32.18/44.67/306.84。读方向九档里 64 拍以内每档差不到 1 个周期，
128 与 256 拍各差 1.8 个周期（1.1% 与 0.6%）；写方向 16 拍以内差不到 1 个周期，256 拍差 5.1 个周期（1.6%）。
另试过三组写参数：`B_LAT=3 WR_BUB=5` 每笔偏慢约 2.7 个周期，`B_LAT=0` 配 `WR_BUB=5` 或 6 在 1~4 拍上偏快 0.3~0.6 个周期。
弹幕的行只有 4~5 拍，所以取这一组。

**再在校准过的从机上跑行调度。** 被测是搬运层骨架（两个核、门控、带内掩码）经真实 `Axi_Mux` S1；测试台扮演几何层与像素通路：
每行「读源 → 读目的 → 写目的」，写目的与下一行的读并行，像素通路在该行目的读完之后 `PIX_LAT` 拍开始出写数据；
每 `FETCH_B` 条命令插一笔 `2 × FETCH_B` 拍的命令读，命令之间几何层空 `CMD_GAP` 拍。每档 200 颗弹幕连续跑：

```text
UNAL=0 MODE=0 SRC_OFF=0 DST_OFF=0 FETCH_B=16 PIX_LAT=4 CMD_GAP=2: cmds=200 rows=3200 reads=6413 cycles=218450 per_cmd=1092.2 per_row=68.3 viol=0/0
UNAL=0 MODE=0 SRC_OFF=0 DST_OFF=1 FETCH_B=16 PIX_LAT=4 CMD_GAP=2: cmds=200 rows=3200 reads=6413 cycles=221651 per_cmd=1108.3 per_row=69.3 viol=0/0
UNAL=1 MODE=0 SRC_OFF=0 DST_OFF=1 FETCH_B=16 PIX_LAT=4 CMD_GAP=2: cmds=200 rows=3200 reads=6413 cycles=221651 per_cmd=1108.3 per_row=69.3 viol=0/0
UNAL=0 MODE=0 SRC_OFF=1 DST_OFF=1 FETCH_B=16 PIX_LAT=4 CMD_GAP=2: cmds=200 rows=3200 reads=6413 cycles=224851 per_cmd=1124.3 per_row=70.3 viol=0/0
UNAL=0 MODE=0 SRC_OFF=0 DST_OFF=1 FETCH_B=16 PIX_LAT=12 CMD_GAP=2: cmds=200 rows=3200 reads=6413 cycles=221659 per_cmd=1108.3 per_row=69.3 viol=0/0
UNAL=0 MODE=0 SRC_OFF=0 DST_OFF=1 FETCH_B=0 PIX_LAT=4 CMD_GAP=0: cmds=200 rows=3200 reads=6400 cycles=220819 per_cmd=1104.1 per_row=69.0 viol=0/0
UNAL=0 MODE=0 SRC_OFF=0 DST_OFF=1 FETCH_B=0 PIX_LAT=4 CMD_GAP=2: cmds=200 rows=3200 reads=6400 cycles=220819 per_cmd=1104.1 per_row=69.0 viol=0/0
UNAL=0 MODE=0 SRC_OFF=0 DST_OFF=1 FETCH_B=1 PIX_LAT=4 CMD_GAP=2: cmds=200 rows=3200 reads=6600 cycles=227219 per_cmd=1136.1 per_row=71.0 viol=0/0
UNAL=0 MODE=0 SRC_OFF=0 DST_OFF=1 FETCH_B=2 PIX_LAT=4 CMD_GAP=2: cmds=200 rows=3200 reads=6500 cycles=224219 per_cmd=1121.1 per_row=70.1 viol=0/0
UNAL=0 MODE=0 SRC_OFF=0 DST_OFF=1 FETCH_B=4 PIX_LAT=4 CMD_GAP=2: cmds=200 rows=3200 reads=6450 cycles=222719 per_cmd=1113.6 per_row=69.6 viol=0/0
UNAL=0 MODE=0 SRC_OFF=0 DST_OFF=1 FETCH_B=8 PIX_LAT=4 CMD_GAP=2: cmds=200 rows=3200 reads=6425 cycles=221994 per_cmd=1110.0 per_row=69.4 viol=0/0
UNAL=0 MODE=0 SRC_OFF=0 DST_OFF=1 FETCH_B=32 PIX_LAT=4 CMD_GAP=2: cmds=200 rows=3200 reads=6407 cycles=221512 per_cmd=1107.6 per_row=69.2 viol=0/0
UNAL=0 MODE=0 SRC_OFF=0 DST_OFF=1 FETCH_B=64 PIX_LAT=4 CMD_GAP=2: cmds=200 rows=3200 reads=6404 cycles=221491 per_cmd=1107.5 per_row=69.2 viol=0/0
UNAL=0 MODE=1 SRC_OFF=0 DST_OFF=1 FETCH_B=16 PIX_LAT=4 CMD_GAP=2: cmds=200 rows=3200 reads=3213 cycles=109651 per_cmd=548.3 per_row=34.3 viol=0/0
UNAL=0 MODE=2 SRC_OFF=0 DST_OFF=1 FETCH_B=16 PIX_LAT=4 CMD_GAP=2: cmds=200 rows=3200 reads=13 cycles=48000 per_cmd=240.0 per_row=15.0 viol=0/0
UNAL=0 MODE=3 SRC_OFF=0 DST_OFF=0 FETCH_B=0 PIX_LAT=4 CMD_GAP=2: cmds=480 rows=480 reads=0 cycles=60480 per_cmd=126.0 per_row=126.0 viol=0/0
```

`MODE` 0~3 依次是 16×16 ALPHA BLIT、16×16 COPY BLIT、16×16 FILL COPY、整屏 FILL 的一行（400 像素）；
`SRC_OFF`/`DST_OFF` 是源、目的 x 对 4 的余数，余数非 0 时一行 5 拍，否则 4 拍。

读法：

- 16×16 ALPHA 每颗 1092~1124 周期，取决于源、目的是否对齐；总览第 1 节手算的「约 1100」（两边都 5 拍）落在这个区间里，
  实测 5+5 拍比手算多 2%。不取命令那一档每行 69.0 周期，两笔读（4 拍与 5 拍）按 E4 的拟合是 31.75 + 32.84 = 64.6，
  余下约 4.4 个周期，是两个核「描述符到地址」与「最后一拍到状态」的延迟加门控的一拍，每笔读约 2.2 个周期。
- `ENABLE_UNALIGNED` 取 0 或 1 周期数完全相同，D15 的选择不影响性能。
- `CMD_GAP` 0 与 2 结果相同，`PIX_LAT` 4 与 12 结果相同：命令之间几个周期的几何层开销被读的门控等待（每笔约 28 周期）盖住，
  像素流水线的延迟被「写与下一行的读并行」盖住。
- 命令批量：每条单取 1136.1，每批 2 条 1121.1，4 条以上都在 1107~1114。
- 整屏 FILL 一行 126.0 周期，总览第 1 节的 125.6 是它的手算值。
- 硬件侧上限按总览第 1 节的算法：145 万 ÷ 1108 ≈ 1310 颗，与原估算的约 1300 一致。

**注错：一行跨 4KB。** 同一个测试台把整屏 FILL 的起点挪到 x = 700，每行 1600 字节跨过 4KB 边界，写核把它拆成两笔：

```text
UNAL=0 MODE=3 SRC_OFF=0 DST_OFF=0 FETCH_B=0 PIX_LAT=4 CMD_GAP=2: cmds=480 rows=480 reads=0 cycles=63360 per_cmd=132.0 per_row=132.0 viol=0/9120
```

写方向 9120 个违例周期：拆出的第二笔 `awvalid` 在第一笔写响应回来之前就拉起了。这条补进了 D3 的第 3 点，
也说明 D3 规矩 1（几何层保证不跨 4KB）对写核同样必要。
