# gfx2d：2D 渲染加速引擎

2026 嵌入式芯片与系统设计竞赛，易灵思 FPGA 创新设计赛道，赛题二：
基于 RISC-V 与 FPGA 异构架构的 2D 图形渲染加速引擎。板卡为钛金 Ti60F225 开发板。

Efinity 工程名 `gfx2d_ti60`，顶层模块 `gfx2d_top`。

## 现在的状态

产品 PC 与 Ti60 使用同一应用菜单与应用管理器：基础2D演示、弹幕、压力测试、三维、
系统信息按所选内容集合提供；全局快捷键与确认/返回语义一致。板端使用独立CLINT 1kHz FreeRTOS
tick，PC使用宿主事件循环。当前软件与输入契约见[应用与输入契约](doc/应用与输入契约.md)，
构建与检查见[软件登记](sw/README.md)。旧 `gfx2dGame` 保留开发/判据入口。
板端基线为[四档快照](bit/snapshots/README.md)：B0/B3/B5配2D固件，B7配完整固件。
独立PC/板端运行包为 `bit/board_release_console_2d_20261030/` 和
`bit/board_release_console_full_20261030/`，包内manifest和verification登记所选内容与验证边界；
板端启动器仅JTAG重载，不改Flash。

当前开发默认构型为 B0；B7 提供固定功能三维条带光栅。当前源码、冻结快照、
本地运行包和板卡状态分别核对。接口见 `rtl/engine/engine-manual.md`，
位流与固件身份见 `bit/snapshots/`。

软件维护范围为构建、BSP、参考执行、素材装载、RTOS、框架与 console。
应用作为演示负载，玩法、美术与手感不作为长期维护工作点。

以下早期整机资源/性能数字保留其历史构型与验收边界。

整机已实现直通引擎、片上纹理（TEX）、条带模式（BAND，ABI v2）、游戏条带后端
与硬件分桶（PB）绘制前端。早期PB构型为 60064 XLR / 172 RAM / 23 DSP，
`core_clk` setup/hold +0.104/+0.022 ns，所有时钟非负，`CAPS=0x0000346F`，
Flash 内容未改写。游戏条带后端接入、`N_max` 与 16 KiB cache 对照已于
2026-10-09 验收：4KB 同位流 `N_max` 直通/TEX 1891/2237，条带 424/505；
16KB 条带候选 479/597，均各 3600 场零掉帧。条带当前受 CPU 构造成本限制。

条带模式 2026-10-09 实现帧级 BAND：ABI v2 条带表、生产分桶与软件执行器、
寄存器与驱动、错误排空和模式往返，该构型整机 41476 XLR/163 RAM/16 DSP，
`core_clk` setup/hold +0.756/+0.026 ns，`CAPS.BAND=1`。软件两套各 1 万张与
16/32 行完整 RTL 同帧对拍通过；真实条带 CRC、错误恢复和旧 engineTest
兼容性均 fail 0。

绘制前端方面：通用实例命令 v1-I（32 B 共享包加 4/8 B 属性数组、显式错误实例
索引和追加实例计数，`CAPS` 位 12 INST）与硬件分桶引用列表（原始绘制只提交
一次，硬件建紧凑保序 PB，`CTRL` 的 `bin_mode` 位选择、与旧命令层二选一）
均已交付，保留旧 v1/TEX/v2 路径；条带搬运重叠评估后暂不做。
硬件结构见 `rtl/engine/engine-manual.md`。

| 部分 | 状态 |
| --- | --- |
| SoC（Sapphire RISC-V，100MHz）+ DDR3（256MB） | 已上板，memTest 通过。SoC 经 `Axi_Mux` 访问 DDR3。**构型定在 Standard**（统一时钟下调试通路不可用；Lite 无 FPU 与自定义指令，17 块跨时钟 FIFO 接受） |
| DDR3 上的第二个 AXI 主机 | `Axi_Mux` 已接入；S1 当前接渲染引擎。256 拍突发上限及读写固定开销是接入前的探针实测（读 `t_tx` 27.4、写 7.3 个周期） |
| HDMI 输出 640x480@60 | 当前从 DDR3 帧缓冲经扫描 FIFO 输出，双缓冲与场边界切换已上板验证；早期八色竖条为历史基线 |
| 程序从 Flash 引导（512KB 镜像） | 已上板验证，两个 CRC 与对照组都符合预期 |
| APB 寄存器通路 | 引擎寄存器映射在 APB 第 0 页；板上回读 `ID=0x47465801`、`CAPS=0x0000046F`（条带启用）与 `LIST_BASE`。早期探针寄存器测试为历史基线 |
| Cache 一致性 | 已上板实测并验收（2026-09-18）：外部主机改写后 CPU 读到旧值，失效之后读到新值；共享区约定放宽为「读回前必须失效」，失效要晚于外部写入完成。判据 `sw/apps/test/cacheCoherencyTest`、机理实验 `sw/apps/probe/cacheMechTest`，机理与约定见 `doc/赛题二总体设计.md` 2.1 节 |
| 像素规格 | 已定稿并已确认（2026-09-18）：写进 `doc/赛题二总体设计.md` 3.5 节，舍入、位宽、舍入顺序与预乘量化误差有穷举校验（`tools/check/check_round255.c`）。修正版 `execSwTest` 三次在板上对齐命令语义十一个整帧 CRC、均 `fail 0` |
| 命令语义规格 | **已定稿、确认并复核闭环（2026-09-20）**：v1 为 DDR 直通、XRGB 目的，不实现 tint；新增 ARGB8888_STRAIGHT；重叠 BLIT 报错；字段矩阵、COUNT/END、错误优先级和副作用写入 `doc/赛题二总体设计.md` 3.4 节。九组一致性用例落成 159 条向量（ABI 1），校验工具 `tools/check/check_cmd_semantics.py` 全部通过六项注错和交换格式回读；五项语义选择已定案，多命令表、FETCH、保留位与零面积边界已补齐 |
| 扫描输出（display） | **已验收（2026-09-21）**：`rtl/display/` 从 DDR3 帧缓冲取数，经 512 拍异步 FIFO 输出 640×480@60 HDMI；`sim/display` 正常、注错与整帧用例通过，四步编译 PASS。`displayTest` 板上 `fail 0`，每场 76800 拍、欠载 0，整屏清屏加双缓冲切换 59.9 fps。场同步中断源已接入，应用侧使能与清除用例留给使用它的工作点；探针压力矩阵三组最低水位 347/331/327 拍、欠载 0，512 拍深度维持，见 `doc/赛题二总体设计.md` 3.3 节 |
| DDR3 帧缓冲与双缓冲 | 已由 display 工作点接入并上板验收，CPU 软件绘制可写后缓冲并在场边界切换 |
| 软件执行器像素吞吐 | **已收尾（2026-09-27）**：4KB 固定生产基线精确 S1 整表约 80.46 M 周期（805 ms），18 项输出独立复算全对；旧六方程注错 CRC 已重判，本次两个 VERIFY_ONLY 构建分别 `fail 0`、`fail 8`，均完整结束。默认生产 BIN 与原基线逐位相同 |
| CPU 每弹幕开销 | **已收尾（2026-09-27）**：两份修改版 DDR 探针在 4KB/16KB 各一次均 `fail 0`/`done`；同一份 69,034 字节程序在两档各一次九组、每组 1000 帧，逐帧状态与命令摘要均 0 错。N=2000 更新加提交 3.510→2.338 ms（−33.4%），同产物新旧读数最大变化 <0.08%。主线已恢复 4KB/1 路，16KB/4 路继续作为候选 |
| 2D 渲染引擎 | **直通六层已上板验收（2026-10-03）**：S1、APB 第 0 页和中断 B 已接上；当时整机 36009 XLR / 105 RAM / 16 DSP，`core_clk` setup +1.067 / hold +0.026 ns，四步 PASS。整帧仿真 16/16；板上复现十一帧 CRC、32 位 `LIST_COUNT`、定向命令、注错、缓存一致性、显示和 memTest。压力表最低水位 335 拍、欠载 0，16 颗 ALPHA 每颗约 1184 拍 |
| Axi_Mux 三端口仲裁、防饿死与扫描 FIFO 水位 | **已整体验收（2026-09-24）**：S0 接扫描输出，S1 接引擎，S2 接 SoC；`axi_mux_guard` VERSION 2 负责仲裁配置与 CPU 地址等待保护。探针压力矩阵最低水位 347/331/327 拍、欠载 0；引擎压力表复测最低水位 335、欠载 0。512 拍 FIFO 维持原值 |
| 软件平台分层与双目标构建 | **已验收**（2026-09-19）：`sw/bsp/`（`ti60/` 与 `host/`）、`sw/lib/`、两套构建 `sw/mk/{target,host}.mk` 与判据脚本 `tools/check/check_sw_layers.sh` 就位。PC 与板上 60 帧整帧 CRC 逐帧相同，分层与注错判据全过 |
| 命令表构造层与软件执行器 | **主机侧已验收**（2026-09-21）：`sw/lib/gfx/`（`gfx_abi.h` 数据布局、`check.c` 十步检查、`list.c` 构造层、`exec_sw.c` 执行器、`cases.c` 十一个整帧用例）与判据 `sw/lib/test/`（独立参考实现 `ref_exec`、手写 45 条向量、交换格式重放器）就位。78 项断言在 64 位与 `-m32` 下输出逐字节相同，ASan + UBSan 无报告；工具导出的 159 条向量与手写 45 条在两个实现上逐项一致；六个方程的注错各自只命中用到它的用例；同一份源码用 riscv-none-elf-gcc 13.4.0 零告警编过。整帧 CRC 期望值入库 `sw/lib/test/golden.txt`，吞吐测量与引擎仿真拿它当金标准。同一执行器随后在板上逐帧对账，并完成分方程吞吐测量 |
| PC 平台层 | **已验收**（2026-09-21）：`sw/bsp/host/board.c` 的场节拍按扫描输出的既成事实重写（场计数自由运行 + present 等下一个场边界），`board.h` 补 `board_frame_count`/`board_swap_count`/`board_should_quit` 三个服务，缩放用最近邻、无窗跑同一个二进制；`sw/lib/core/replay` 是录像格式的唯一真源，`tools/check/replay2c.py` 在 `.g2dr`、C 数组与录制文本之间转换。判据程序 `sw/apps/test/platformTest` 与 `tools/check/check_sw4_pc.sh` 全过：十一用例 CRC 对 `golden.txt`、600 帧节拍（每帧恰好一场）、掉帧注错、1800 帧回放两遍逐帧相同、有窗与无窗逐行相同、注错构建见红。板上那三个查询也已上板跑过（`platformTest` 板上产物 `fail =0`：十一用例 CRC 与 `golden.txt` 逐行相同、600 帧 `dfield=dswap=600` 且每帧恰一场、`drop` 的 2 减 1、60 帧回放两遍 `diff=0`；板上节拍抖动 1~2 微秒，PC 上约 300 微秒） |
| 基础 Demo（赛题基础要求 3） | **已验收**（2026-09-28）：`sw/apps/game/gfx2dGame` 的基础 Demo——场景 A 是大量方块随机移动（N=100~5000、Q12.4、四个固定出屏样例、COPY／ALPHA／ADD 各走 FILL 与 BLIT），场景 B 是三张整屏图快速切换（初始化时预合成进 DDR 离屏层，每帧一次整屏 BLIT COPY）；默认演示模式，`-DSW6_JUDGE=1` 走判据模式回放两段入库录像。`tools/check/check_sw6_demo.sh` 十项全过：录像可重生成、判据模式 1226 帧 `fail 0` 且与入库期望日志逐行相同、有窗与无窗逐字节相同、两遍逐帧相同、HUD 的 `hcrc` 与 Python 侧按字库文本独立重画的整块矩形一致、三个 `scene` 参考值与每个 B 帧对上、四个方程各自注错见红、双缓冲不变量的反例见红、`SAN=1` 无报告、两种板上构建零告警。**板上对账已完成**：2026-09-28 在 20 项新素材包与 Spleen 字库下重跑，判据模式快速档（77,158 字节）经 `board_run.py --bin` 跑 480 帧 `fail 0`，从 `pak` 行起 489 行与 PC 期望日志逐行相同，原始串口日志 `sw/apps/game/gfx2dGame/logs/sw6_board_quick.log`。演示模式的 HDMI 画面与软硬件帧率对比随游戏集成验证 |
| 演示模式、压力测试与统计埋点 | **已验收**（2026-09-28）：A~D 四批完成。按方程的普查 `sw/lib/gfx/census` 与模拟时钟、帧时间直方图 `sw/lib/core/framehist`、日志队列 `sw/lib/core/logq` 与 `board_tx_room()`；`N_max` 搜索状态机 `sw/lib/core/nmax`（倍增加二分、等涨潮、确认与退步）；默认构建是模式框架（1 基础 Demo、2 游戏、3 压力测试，和弦切换）、每秒一行统计 CSV、自动驾驶；模式 3 在终符上搜索（模拟时钟下 16×16 `N_max` 2002、8×8 5321，只验证流程）与固定旋钮 `-DGFX2D_PRESS_KNOB`；出图 `tools/stats/plot_stats.py` 与第一版统计（S1 规模每帧 872,384 像素，是设计文档估算的 1.28 倍）。`tools/check/check_sw8_stats.sh` 全过、各注错见红。板上的软件与硬件数字见「板上移植」行 |
| 板上移植 | **已验收**（2026-10-04）：比特流上接通两路中断、真实硬件执行器、双命令表流水、Flash 自启动与键盘/手柄输入。修复后软件前 600 帧、同帧 CRC 与注错、性能和 Flash 判据通过；完整硬件 9208 帧新回放暂缓，旧日志仅为历史证据。普通游戏 240 帧短测预热后 60 fps、软件约 2.5 fps；硬件 `N_max` 为 16×16 的 1014、8×8 的 1365，软件均 `BELOW_N0`；S1 约 30.6 fps，扫描欠载 0。完整关卡流水性能和 HDMI/实体键现场观察仍未覆盖 |
| SoC IP 定型（升级 3.4.0、接按键、预留中断与摄像头） | 已上板并验收（2026-09-19）：efx_soc 3.4.0 重新生成，开 GPIO1 接 KEY0~3、开用户中断 A/B/C、保留 I2C0 给摄像头、关 `axiA` 与 `io_ddrMasters_0`；工具链换成 gcc 13.4.0。调试通路连续 6 次会话、memTest、APB 探针、`ddrAliasTest`、`ddrBurstProbe`、缓存一致性三件套、`bootSizeTest`（含对照组）与 gfx2dGame 60 帧 CRC 全过；HDMI 八色竖条与板上四个按键（KEY0~3）已确认 |

历次整机构型登记如下，作为逐次对照：

| 日期 | 构型 | XLR | 片上 RAM | DSP | `core_clk` setup/hold (ns) | 备注 |
| --- | --- | ---: | ---: | ---: | --- | --- |
| 2026-10-09 | PB 前端并入（当前） | 60064 | 172 | 23 | +0.104 / +0.022 | 所有时钟非负，`CAPS=0x0000346F` |
| 2026-10-09 | 条带 + 游戏后端（帧级 BAND） | 41476 | 163 | 16 | +0.756 / +0.026 | `CAPS.BAND=1` |
| 2026-10-08 | 片上纹理（TEX，4KB/1 路） | 36443 | 121 | 16 | +0.699 / +0.026 | 注错机制已改为 `ifdef`，自写 RTL 位宽截断告警已清；像素域 `hdmi_tx_slow_clk` setup +34.066 ns；接口缺脚为已知 6 项；SHA256 `c41f25182088895cad2c5b21e868d232350d33850f7470b5cc80fe69a9a0dfec`；四步 PASS |
| 2026-10-02 | 直通六层上板（4KB/1 路） | 36009 | 105 | 16 | +1.067 / +0.026 | SHA256 `b2118f54501cde5d6f3c8c8143aa45a578fa04aa32a797e37f4a2268d915cf63` |
| 2026-09-25 | 直通中途对照组 | 21935 | 71 | — | +3.516 / +0.026 | 非当时的本地编译，仅作对照 |
| 2026-09-21 | 扫描输出接入 | 21807（35.87%） | 71（27.7%） | 4 | 3.898 / 0.026 | 约束告警 0 条、无负 slack；相对上一版 XLR +154、RAM +6 块（扫描输出的 128 位 × 512 深异步 FIFO，在 `doc/赛题二总体设计.md` 2.3 节 7 块的预算内，hold 未被挤薄）；像素域 `hdmi_tx_slow_clk` setup 33.807 / hold 0.071 ns |
| 2026-09-19 | SoC IP 定型后 | 21653 | 65 | — | 3.405 / — | 定型前为 XLR 21753、setup 3.780 |
| 2026-09-19 前 | Axi_Mux 端口重分配前 → 后 | 16151 → 21065 | — | — | 2.994 → 3.467 | 分配给扫描输出与引擎之后 |

片上纹理（TEX）引入的增量做过逐模块归因：相对直通构型（36009 XLR / 105 RAM / 16 DSP）
**+434 XLR / +16 RAM**。2026-10-08 用直通构型的提交 `a832531` 单独编译一次做归因
（复现为 36009 / 105 / 16，与登记值一致，对照组可靠；两版都取
`outflow/gfx2d_ti60.hier_util.rpt` 的 engine 子树）：

| 模块 | 直通 | TEX 版 | Δ | 归因 |
| --- | ---: | ---: | ---: | --- |
| `u_engine/u_cmd`（含 `u_ssrc`、`u_sdst` 各 +164/+169） | 3225 | 3577 | **+352** | TEX 结构两条检查、源空间分支、双取数来源 |
| `u_engine/u_tex` | 0 | 116 | **+116** | 新增的片上纹理 |
| `u_engine/u_mover` | 1166 | 1251 | +85 | TEX 描述符出口、状态合并 |
| `u_engine/u_geom` | 2315 | 2346 | +31 | `rd_tex` 传下去 |
| `u_engine` 其余（`u_pix`、`u_regs`、12 个例化） | — | — | −34 | 寄存器与像素阵列的净减 |
| `u_display`（`u_fetch`、`u_out`） | 3336 | 3322 | −14 | 无相关改动，布局差异 |
| `u_soc` / `u_ddr3` / `u_axi_mux` / `u_mux_guard` / `u_hdmi_tx` 等 | — | — | −85 | 均无源码改动，布局差异 |

其中片上 RAM 的 +16 块就是 `u_tex` 的四个 1024×32 bank，是该模块单独占的，没有分摊。

注错机制由 `parameter` 换成 `ifdef` 的映射差异是 +479 XLR；2026-10-08 顺手清掉自写 RTL
的四条位宽截断告警（`engine_cmd.v` 301/503/505、`engine_regs.v` 145）之后，
整机 XLR 从 36716 降到 36443（**−273**）、`core_clk` setup 从 +0.591 升到 +0.699 ns。
所以 TEX 版相对直通构型的 +434 里，形态差异已不再是主项。

## 目录

| 目录 | 内容 | 说明 |
| --- | --- | --- |
| `rtl/` | 硬件设计源码 | 顶层、APB 从设备、HDMI、厂商 IP，见 `rtl/README.md` |
| `fpga/` | Efinity 工程 | 工程文件、接口设计、约束、SoC IP，见 `fpga/README.md` |
| `sdk/` | SoC 的 BSP 与官方示例 | IP 生成物，改动要登记，见 `sdk/README.md` |
| `sw/` | 本项目编写的 RISC-V 程序 | 见 `sw/README.md` |
| `sim/` | 仿真 | 见 `sim/README.md` |
| `bit/` | 官方比特流与 Flash 备份 | 见 `bit/README.md` |
| `tools/` | 辅助脚本 | 见 `tools/README.md` |
| `doc/` | 方案与开发环境手册 | 见 `doc/README.md` |

本目录是工程本体，自包含：设计依据与实测数据写在 `doc/` 与各模块随码文档
（`rtl/engine/engine-manual.md` 等），不依赖仓库内其他目录。厂商资料包不入库。

每个目录的 README 登记本目录下的文件从哪来、能不能改、有没有被工程引用，
反映的是当前状态。

## 快速开始

工具怎么装、占位符怎么取值，见 [`doc/开发环境.md`](doc/开发环境.md)。
下面的 `<EFINITY>` 等占位符按该文件第 1 节替换。

克隆之后先展开生成物里的路径（只需一次）：

```
wsl -e python3 tools/check/localize_paths.py --apply
```

编译硬件：

```
call <EFINITY>\bin\setup.bat
cd fpga\gfx2d_ti60
efx_run.bat gfx2d_ti60.xml
```

加载到板上（掉电丢失，不动 Flash）：

```
<EFINITY>\pgm\bin\ftdi_pgm.bat -m jtag -u <JTAG> fpga\gfx2d_ti60\outflow\gfx2d_ti60.bit
```

跑仿真：

```
wsl -e bash -c 'bash sim/hdmi/run.sh && bash sim/apb_probe/run.sh && EFINITY_HOME=<EFINITY 的 WSL 路径> bash sim/axi_probe/run.sh && EFINITY_HOME=<EFINITY 的 WSL 路径> bash sim/axi_mux_guard/run.sh'
```

编译并用调试器运行一个程序见 `sw/README.md`，写 Flash 与整片备份见 `doc/开发环境.md` 第 8 节。

编译软件（两套目标，见 `sw/README.md`）：

```
wsl -e bash -c 'cd sw/apps/console && make RISCV_BIN=<RISCV_BIN>'              # 板端产品，默认菜单
wsl -e bash -c 'cd sw/apps/console && make PLATFORM=host && ./build_host/console-perf' # PC产品，同一菜单
wsl -e bash -c 'cd sw/apps/game/gfx2dGame && make PLATFORM=host'               # 旧开发/判据入口
wsl -e bash -c 'cd sw && make -f mk/host.mk test'                              # 主机侧单元测试，64 位与 -m32 比对
wsl -e bash -c 'RISCV_BIN=<RISCV_BIN> bash tools/check/check_sw_layers.sh'            # 分层判据，含三处注错
```

## 地址映射

CPU 看到的地址，取自 `sdk/bsp/efinix/EfxSapphireSoc/include/soc.h`：

| 区域 | 基址 | 大小 |
| --- | --- | --- |
| DDR3 | `0x0000_1000` | 窗口 `0xE000_0000`，实际颗粒 256MB。只用 `0x0FFF_FFFF` 以下，以上按 256MB 周期回绕成别名 |
| UART0 | `0xF801_0000` | `0x40` |
| SPI0 | `0xF801_4000` | `0x1000` |
| GPIO0（4 位，接 LED0~3） | `0xF801_5000` | `0x100` |
| GPIO1（8 位，[3:0] 接板上四个按键 KEY0~3，低有效；[4] 预留摄像头复位） | `0xF801_6000` | `0x100` |
| I2C0（预留给摄像头，未使用） | `0xF801_7000` | `0x100` |
| APB Slave 0 → `apb_probe` | `0xF810_0000` | 64KB，其中 `0x1000~0x1FFF` 除外 |
| APB Slave 0 → `axi_probe_master` | `0xF810_1000` | 4KB，寄存器表见 `rtl/bus/axi_probe_master.v` |
| CLINT | `0xF8B0_0000` | `0x1_0000` |
| PLIC | `0xF8C0_0000` | `0x40_0000` |
| 片上 RAM（bootloader） | `0xF900_0000` | 4KB |

中断号：UART0 = 1，SPI0 = 4，I2C0 = 8，GPIO0 = 12、13，GPIO1 = 14、15，
用户中断 A = 16（扫描输出场同步，2026-09-20 起已接到 `u_display` 的场起始中断，电平、复位后 `IRQ_EN=0`）、
B = 17（引擎）、C = 22（摄像头），B、C 的源模块接入前在顶层接 0。
`axiA` 从设备窗口（原 `0xE100_0000`，中断 30）已于 2026-09-19 随 SoC IP 定型关掉。

DDR3 与 Flash 的分区规划见 `doc/赛题二总体设计.md` 2.1、2.2 节。

## 约定

- **`sdk/` 只读**：它是 SoC IP 的生成物，重新生成时整个目录会被覆盖。项目对 sdk 的需求
  （板级封装、驱动包装、地址映射）一律放 `sw/bsp/` 覆盖，不改 sdk 本身；
  改软件结构、构建或 BSP 接口时跑 `check_sw_layers.sh`，其中一项就是 `git diff --quiet HEAD -- Main/sdk`。
- **分层由编译结构保证**：`sw/lib/` 编的时候只有 `-Isw/lib`，应用编的时候没有 sdk 的头文件路径。
  越界不是靠人遵守，是编不过。见 `sw/README.md`。
- **vendor 目录不改**：`rtl/vendor/` 下是厂商与第三方代码，必须改时在 `rtl/README.md` 里登记。
- **生成物不手改**：`sdk/` 与 `fpga/gfx2d_ti60/ip/soc/` 是 IP 生成的，改动登记在 `sdk/README.md`，
  重新生成后要逐条恢复。
- **不写绝对路径**：文档用 `doc/开发环境.md` 第 1 节的占位符，
  生成物里必须写路径的几个文件用 `tools/check/localize_paths.py` 展开。
  提交前暂存源码，跑一次 `--restore` 与 `--check --tracked`；不入库的本机工作资料可另用全目录 `--check`。
- **判据要能注错见红**：每个仿真除了正常用例，必须有把逻辑改错后会 FAIL 的用例，
  否则不算验证过。
- **提交前检查按影响选择**：[验证流程](doc/验证流程.md) 是范围、档位与证据复用的唯一规则；
  `python3 tools/check/validate.py --list` 给出唯一检查清单。先核对改动与消费者，
  执行所选项并登记 PASS / FAIL / INCOMPLETE 与不适用原因。文档改动不触发硬件回归，
  RTL 改动仍需综合与时序检查，正式采用候选仍需该候选的完整回放与相关板测。
- **数字要标明来源**：估算与推测必须写清楚，实测要给命令和结果。
- **状态与机制写全**：README 与 `doc/` 写当前是什么样，机制、结论与实测数据就地写清，
  不写开发流水，引用只落在 Main 之内。
