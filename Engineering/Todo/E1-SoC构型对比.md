# E1 Sapphire Standard 与 Lite 构型对比

状态：未开工（spec 与前期调研已完成，尚未生成任何 IP 与比特流）。不阻塞引擎开工，
越晚做的代价越大：引擎与扫描输出成形之后再换构型，整套回归要重跑一次。
与 [`soc资源裁剪.md`](soc资源裁剪.md) 的关系：那条是执行清单，第一条就是本实验；
本文件只回答「要不要换成 Lite」，不含裁剪的动作项。

## 1 要解决什么

**片上 RAM 是这个设计里最先用完的资源。** Ti60 全片 256 块，当前工程占 65 块（25.4%），
设计文档 2.3 节给引擎留了 108 块，加起来 173 块。逻辑单元不紧（XLR 26.6%），
DSP 只用 4 个。

65 块里有 **17 块（占全片 6.6%）是跨时钟 FIFO**，来自 SoC 内部的
`system_ddr_ddrLogic_cc_fifo`（`BmbCcFifo`），数据引自本仓 `outflow/gfx2d_ti60.hier_util.rpt`
第 179~187 行：

| 子项 | mem | xlr |
| --- | --: | --: |
| `system_ddr_ddrLogic_cc_fifo` 合计 | 17 | 148 |
| 　`io_input_cmd_queue` | 10 | 75 |
| 　`io_output_rsp_queue` | 7 | 73 |

它存在的理由是 DDR 落在专用内存时钟域。而顶层 `rtl/top/gfx2d_top.v` 第 379~380 行
把 `io_systemClk` 与 `io_memoryClk` 都接在 `core_clk` 上，这个 FIFO 两端是同一个时钟，
它在这里不搬运任何东西。

Sapphire 手册（UG-RISCV-SAPPHIRE v6.0，Cache/Memory 页）写明「External Memory Clock Domain：
Unified System Clock」只在 Lite 版可选。**这一条已在 IP 生成器里核实**：
`<EFINITY>/ipm/ip/efx_soc/efx_soc/ipm/ip_component.xml` 第 8982~8992 行的
`DDRCLK_DOMAIN` 参数带 `isPresentGUI = (${SOC_MODE} == 1) && (${DDR} == 1)`，
即只有 SOC_MODE 为 1（Lite）时才在界面上出现；枚举取值在同一文件第 3657~3660 行的
`DdrClkChoice`：`0` Dedicated Memory Clock、`1` Unified System Clock。
同一文件第 1872、1900 行是 `io_memoryClk`、`io_memoryReset` 两个端口的 `isPresent` 条件
`((${SOC_MODE} == 0)||(${DDRCLK_DOMAIN} == 0))`：Standard 构型下不论 `DDRCLK_DOMAIN` 取什么，
独立的内存时钟端口都会生成，这与当前工程 `DDRCLK_DOMAIN = 1` 却仍有跨时钟 FIFO 相符。
反过来，Lite 组生成后没有这两个端口，复制工程里的顶层例化要删掉 `io_memoryClk` 一行（`gfx2d_top.v` 第 380 行）
以及 `io_memoryReset` 的连接，否则综合报端口不存在。

同一份文件第 8952、8964 行给出 Lite 的代价：`MULDIV_EXT`（整数乘除）与
`BARREL_SHIFTER`（桶形移位器）在 Lite 下才由用户选择，Standard 版是内置的。
工程当前这两项都是 0（`fpga/gfx2d_ti60/ip/soc/settings.json`），是因为它们在
Standard 下不生效，换 Lite 时必须显式打开：没有硬件乘除时，按 `rv32im` 编译的程序
遇到乘除指令会触发非法指令异常，而不只是变慢。
第 8970 行的 `REDUCED_CSR`（CSR 精简）同样只在 Lite 下生效，当前值是 `1'b1`，
换 Lite 后会真的精简 CSR，要在对比表里显式列出并决定取值。

Lite 去掉跨时钟 FIFO 预计省 17 块 RAM，但 Lite 还会改动 CPU 与总线的其他部分，
**整机净省多少要以编译结果为准**，不能直接写 17。
本实验要回答三件事：整机 RAM 实际省多少、为此付出多少 CPU 性能，
以及 Lite 不提供的 FPU、自定义指令、压缩指令（第 5291、5243、5267 行，都只在 `SOC_MODE == 0` 时出现）
对本设计有没有影响。

## 2 怎么切分

### 第一步，Lite 构型与 Standard 逐项对比（本实验的主线）

在仓库之外复制一份工程，生成 Lite 版 SoC，与现有 Standard 版对比四个量：
CoreMark、片上 RAM 块数、XLR、时序。这一步不改仓库里的任何文件，产物不入库。

生成前先把两套配置逐字写清楚，避免「变量混在一次回归里」。除下表列出的项，
两份 `settings.json` 必须逐字节相同：

| 参数 | Standard（现状） | Lite（对比组） | 依据 |
| --- | --- | --- | --- |
| `SOC_MODE` | `0` | `1` | 本实验的自变量 |
| `DDRCLK_DOMAIN` | `1`（Standard 下不在界面上，值不生效） | `1`（Unified System Clock） | ip_component.xml `DdrClkChoice` |
| `MULDIV_EXT` | `0`（Standard 内置） | `1` | 不打开则 Lite 没有硬件乘除 |
| `BARREL_SHIFTER` | `0`（Standard 内置） | `1` | 同上 |
| `REDUCED_CSR` | `1'b1`（Standard 下不生效） | `1'b1`，生效 | 第 8970 行；Lite 下真的精简 CSR，回归表里中断与 CLINT 相关程序要能跑 |
| `CMREXT` | `0` | `0`（不出现，Standard-only 参数） | 打开会牵动工具链与全部 CRC 期望值 |
| `Frequency` | `100` | `100` | 唯一变量是构型 |
| `LDSize` | `4092` | `4092` | 与 bootloader 512KB 一致 |
| `DCacheSize` / `ICacheSize` | `4096` / `4096` | 原样 | 缓存加大是 E8 的自变量 |
| `DDRWidth` / `DDR_IdWidth` / `DDR_AXI4` | `128` / `8` / `1'b1` | 原样 | |

判据是数字，不能只看「能跑」。CoreMark 的跑法要固定到可复现：
同一份 ELF、同一个 `-march`/`-mabi`、同一套 BSP、同样的 `ITERATIONS`、同频 100MHz。
`-march` 由 `sdk/software/standalone/common/riscv64-unknown-elf.mk` 按 BSP 里的 `RV_M`、`RV_C` 等推出，
两套 BSP 若推出不同的值，ELF 就不再相同。所以用同一份 ELF（在 Standard 的 BSP 下以 `rv32im` 编出）
去跑两个比特流，并确认两套 `soc.h` 的外设地址一致，地址不一致时这份 ELF 不能直接复用，要先说明差异。

### 第二步，量裁剪项的净收益（与 [`soc资源裁剪.md`](soc资源裁剪.md) 共用这次复制工程）

裁剪项**不能混进第一步的 Lite 对比组**，否则 RAM、XLR、时序的变化分不清来自构型还是裁剪。
在同一份复制工程里再生成一次「Lite + 关 I2C0、`axiA`、`io_ddrMasters_0`」，
与第一步的 Lite 组对比，差值就是裁剪项的净收益。两次生成都在仓库之外，不涉及登记项恢复，
代价只是多一次生成与编译。三个裁剪项的当前占用（`hier_util.rpt`，逐项可查）：

| 项 | 关掉的参数 | 当前占用 | 出处行 |
| --- | --- | --- | --- |
| I2C0 | `I2C0 = 1'b0` | XLR 370.5，其中 285.5 是该模块独有 | 第 195~198 行 |
| `axiA` 16MB 从口窗口 | `AXISlave = 1'b0` | XLR 22（`system_axiA_logic_bmbToAxiBridge`） | 第 156 行 |
| `io_ddrMasters_0` 通路 | `AXIMaster = 1'b0` | 顶层不接时钟时已被优化掉，实测差值为 XLR 853 与 5 块 RAM | platform 第 01 章 4.5 节 |

设计文档 1.2 节写「I2C0 约 350 XLR」「`axiA` 约 20 XLR 与一部分地址解码」，
上表是逐项核对后的实测值，报告里以本表为准。

### 第三步，认领 506 个 XLR 的差额（本实验独占的收尾项）

platform 第 01 章 5.1 节表里有一行「其余（死文件清理、外部主机口不接时钟等）约 −506 XLR，
**差额反推，没有逐项归因**」。这一刀重生成 IP、重跑布局，
正好借层次报告把它认领掉，不要让它一直挂在那张表里。

做法是把官方工程的布局报告（`Efinix/02_.../outflow/ddr_demo_ti60.place.rpt` 与
`edb_top.res.csv`）与本次的 `hier_util.rpt` 按模块名对照，能用来源文件说清的逐项列出，
说不清的直接写「未归因」，不要用新差额去平账。

### 第四步，出结论并同步文档

按第 3 节的判据给出「换 / 不换」的结论与理由，然后更新
`../TechReports/platform/` 新增一章、`../../Main/README.md` 的资源行、
 [`soc资源裁剪.md`](soc资源裁剪.md) 的第二步（定构型）。

本实验**不含**下面这些，它们属于别的条目或别的工作点：
关 I2C0 之后的顶层端口删除（`soc资源裁剪.md` 第四步）、
升级到 efx_soc 3.4.0 与同步软件包（`soc资源裁剪.md` 第二步、第三步）、
缓存加大（E8）、按键接进 SoC（设计文档 1.3 节，属重新生成 IP 的搭车项）。

## 3 判据

| 项 | 判据 |
| --- | --- |
| 生成前 | 工作区干净（`git status` 无未提交改动）或已备份，生成器会覆盖 `ip/soc/` 与软件包 |
| 两组可比 | 两份 `settings.json` 除第 2 节表中的参数外逐字相同；两个 CoreMark ELF 的 `-march`、`ITERATIONS`、BSP 完全相同，SHA256 一致 |
| 编译 | 四步 PASS，约束告警 0，`Missing Interface Pins` 仍是已知 6 项，本项目的位宽截断告警仍为 0 |
| 片上 RAM | Lite 组的 `hier_util.rpt` 里 `system_ddr_ddrLogic_cc_fifo` 整行消失；仍然存在说明统一时钟没真正生效，要回去查 `DDRCLK_DOMAIN` 与实际网表。其余模块的 RAM 变化逐行列出，整机净变化写实测数 |
| XLR | 与 platform 第 01 章 5.1 节的表逐行对比，裁剪项的变化能由第 2 节的表解释 |
| CoreMark | 三样都记：`Iterations/Sec`、`CoreMark 1.0` 总分、每 MHz 分数。每次运行都必须打印 `Correct operation validated`，且单次运行时间不少于 10 秒（CoreMark 的有效性规则，不够就加大 `ITERATIONS`，现在 makefile 里是 2000）。跑三遍取中位数，偏差超过 1% 要查原因 |
| 时序 | `core_clk` 的 setup 与 hold 都不为负；hold 与 Standard 逐行对比并记录（现为 0.026 ns，见 `outflow/gfx2d_ti60.timing.rpt` 第 71 行） |
| 上板 | Lite 比特流加载后跑回归表：memTest PASS、APB 探针四项、HDMI 八色竖条、`sw/ddrAliasTest` 与 `sw/ddrBurstProbe` 的 `fail 0` |
| 启动 | `sw/bootSizeTest` 两个 CRC 与 `../../Main/sw/README.md` 登记的期望值一致。本实验沿用 gcc 8.3.0 与现有 BSP，期望值不变；换了工具链就要按 `soc资源裁剪.md` 第五步重算 |
| 注错 | 在 Standard 比特流上，把同一份 CoreMark 改用 `-O0` 重编一次，分数必须明显下降（预期降到一半以下）。不下降说明跑的不是刚编出的那份 ELF，或者分数没有反映 CPU 执行 |
| 差额 | 第 2 节第三步的 506 XLR 每一行都写明来源文件或写「未归因」，不允许留空 |
| 净收益 | 报告里给出定稿一行：`Lite + 关 I2C0/axiA` 相对当前基线的 **片上 RAM 与 XLR 各自增减多少，CoreMark 降多少** |

「注错」一条是本实验的自检。CoreMark 的分数与 `-march`、缓存、编译器优化强相关，
一个抄错的命令行也能给出一个像样的数字；只有改掉一个已知有影响的选项后分数必须下降，
前面那个分数才可信。这一条对齐 platform 系列「判据要能注错见红」的规矩。
不用「关掉 `MULDIV_EXT`」做这个自检：按 `rv32im` 编译的程序在没有硬件乘除的 CPU 上会触发非法指令异常，
得到的是崩溃而不是分数下降；改用 `rv32i` 重编又换掉了 ELF，而且还要多生成一次 IP。

## 4 开发日志

**2026-09-17 spec 核对**（未开工，只改 spec）：
`DdrClkChoice` 的行号订正为第 3657 行；补上 Standard 下跨时钟 FIFO 必然生成的依据（第 1872、1900 行）；
「Lite 必省 17 块」改为「预计省 17 块，净值以编译为准」；对比表补 `REDUCED_CSR`；
第二步的裁剪项从 Lite 对比组里拆出，单独生成一次，避免两个变量混在一起；
「开关」判据改为 `-O0` 注错，原写法在无硬件乘除时会崩溃而不是降分；
CoreMark 补有效性条件；统一 `-march` 的写法；订正一处相对路径层级。

（前期调研：统一时钟只在 Lite 可选一条已在 IP 生成器源码里核实，
17 块 RAM 的归属已指到 `hier_util.rpt` 具体行。）
