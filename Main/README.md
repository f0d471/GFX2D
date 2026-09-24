# gfx2d：2D 渲染加速引擎

2026 嵌入式芯片与系统设计竞赛，易灵思 FPGA 创新设计赛道，赛题二：
基于 RISC-V 与 FPGA 异构架构的 2D 图形渲染加速引擎。板卡为钛金 Ti60F225 开发板。

Efinity 工程名 `gfx2d_ti60`，顶层模块 `gfx2d_top`。

## 现在的状态

| 部分 | 状态 |
| --- | --- |
| SoC（Sapphire RISC-V，100MHz）+ DDR3（256MB） | 已上板，memTest 通过。SoC 经 `Axi_Mux` 访问 DDR3。**构型定在 Standard**（Lite 的对比与不采用的理由见 platform 第 04 章） |
| DDR3 上的第二个 AXI 主机 | `Axi_Mux` 已接入，暂由测量主机 `axi_probe_master` 占用。实测突发上限 256 拍，读事务固定开销约 27 个周期、写约 7 个 |
| HDMI 输出 640x480@60 | 已上板，八色竖条显示正确（接入 `Axi_Mux` 后的比特流 2026-09-17 复核通过）。画面来自彩条发生器，还没有接帧缓冲 |
| 程序从 Flash 引导（512KB 镜像） | 已上板验证，两个 CRC 与对照组都符合预期 |
| APB 寄存器通路 | 已上板验证，探针寄存器四项全对；`0x1000~0x1FFF` 分给测量主机 |
| Cache 一致性（E3） | 已上板实测并验收（2026-09-18）：外部主机改写后 CPU 读到旧值，失效之后读到新值；共享区约定放宽为「读回前必须失效」，失效要晚于外部写入完成。判据 `sw/apps/test/cacheCoherencyTest`、机理实验 `sw/apps/probe/cacheMechTest`，见 `../Engineering/TechReports/platform/05-CPU数据缓存与第二个主机写入的一致性.md` |
| 像素规格（E7 §2.1） | 已定稿并已确认（2026-09-18）：写进 `doc/赛题二总体设计.md` 3.5 节，舍入、位宽、舍入顺序与预乘量化误差有穷举校验（`tools/check/check_round255.c`）。E7 整体未验收，软件执行器未开工 |
| 命令语义规格（SW2） | **已定稿、确认并复核闭环（2026-09-20）**：v1 为 DDR 直通、XRGB 目的，不实现 tint；新增 ARGB8888_STRAIGHT；重叠 BLIT 报错；字段矩阵、COUNT/END、错误优先级和副作用写入 `doc/赛题二总体设计.md` 3.4 节及 `../Engineering/TechReports/spec/02-命令语义.md`。九组一致性用例落成 159 条向量（ABI 1），校验工具 `tools/check/check_cmd_semantics.py` 全部通过六项注错和交换格式回读；五项语义选择已定案，多命令表、FETCH、保留位与零面积边界已补齐 |
| 扫描输出（display，M1 显示） | **已验收（2026-09-21）**：`rtl/display/` 从 DDR3 帧缓冲取数，经 512 拍异步 FIFO 输出 640×480@60 HDMI；`sim/display` 正常、注错与整帧用例通过，四步编译 PASS。`displayTest` 板上 `fail 0`，每场 76800 拍、欠载 0，整屏清屏加双缓冲切换 59.9 fps。场同步中断源已接入，应用侧使能与清除用例留给使用它的工作点；E5 的三组探针压力矩阵见 platform 第 08 章 |
| DDR3 帧缓冲与双缓冲 | 已由 display 工作点接入并上板验收，CPU 软件绘制可写后缓冲并在场边界切换 |
| 2D 渲染引擎 | 尚未接入；Axi_Mux 的 S1 暂由 `axi_probe_master` 占用，引擎接入后须遵守 E5 的在途契约并复测扫描 FIFO |
| Axi_Mux 三端口仲裁、防饿死与扫描 FIFO 水位（E5） | **已整体验收（2026-09-24）**：S0 接真实扫描输出、S1 由探针主机占用并预留给引擎、S2 接 SoC；`axi_mux_guard` VERSION 2 负责仲裁配置与 CPU 地址等待保护。引擎读、写方向各最多一笔在途，写方向不能依赖 guard 兜底。2.4 在 `idle/read/write` 三组各连续采 120 场，五个有效完整会话均 `fail 0`，最低水位 347/331/327 拍、欠载 0，维持 512 拍 FIFO；一次 `no_frame` 已定位为快照跨域握手缺陷并修复，新比特流上三次完整启动复测均通过。真实引擎接入后仍须在最重命令表下复测。过程与数据见 `../Engineering/TechReports/platform/06-Axi_Mux三端口仲裁与SoC防饿死保护.md`、`../Engineering/TechReports/platform/08-扫描FIFO的压力矩阵与深度定值.md` |
| 软件平台分层与双目标构建（SW1） | **已验收**（2026-09-19）：`sw/bsp/`（`ti60/` 与 `host/`）、`sw/lib/`、两套构建 `sw/mk/{target,host}.mk` 与判据脚本 `tools/check/check_sw_layers.sh` 就位。PC 与板上 60 帧整帧 CRC 逐帧相同，分层与注错判据全过。见 `../Engineering/TechReports/sw/01-软件平台分层与双目标构建.md` |
| 命令表构造层与软件执行器（SW3） | **主机侧已验收**（2026-09-21）：`sw/lib/gfx/`（`gfx_abi.h` 数据布局、`check.c` 十步检查、`list.c` 构造层、`exec_sw.c` 执行器、`cases.c` 十一个整帧用例）与判据 `sw/lib/test/`（独立参考实现 `ref_exec`、手写 45 条向量、交换格式重放器）就位。78 项断言在 64 位与 `-m32` 下输出逐字节相同，ASan + UBSan 无报告；工具导出的 159 条向量与手写 45 条在两个实现上逐项一致；六个方程的注错各自只命中用到它的用例；同一份源码用 riscv-none-elf-gcc 13.4.0 零告警编过。整帧 CRC 期望值入库 `sw/lib/test/golden.txt`，E7 与引擎仿真拿它当金标准。**板上未跑**，那是 E7 的账。见 `../Engineering/TechReports/sw/02-命令表构造层与软件执行器.md` |
| PC 平台层（SW4） | **已归档**（2026-09-21）：`sw/bsp/host/board.c` 的场节拍按扫描输出的既成事实重写（场计数自由运行 + present 等下一个场边界），`board.h` 补 `board_frame_count`/`board_swap_count`/`board_should_quit` 三个服务，缩放用最近邻、无窗跑同一个二进制；`sw/lib/core/replay` 是录像格式的唯一真源，`tools/check/replay2c.py` 在 `.g2dr`、C 数组与录制文本之间转换。判据程序 `sw/apps/test/platformTest` 与 `tools/check/check_sw4_pc.sh` 全过：十一用例 CRC 对 `golden.txt`、600 帧节拍（每帧恰好一场）、掉帧注错、1800 帧回放两遍逐帧相同、有窗与无窗逐行相同、注错构建见红。板上那三个查询也已上板跑过（`platformTest` 板上产物 `fail =0`：十一用例 CRC 与 `golden.txt` 逐行相同、600 帧 `dfield=dswap=600` 且每帧恰一场、`drop` 的 2 减 1、60 帧回放两遍 `diff=0`；板上节拍抖动 1~2 微秒，PC 上约 300 微秒）。只剩关窗退出与 F1~F3 要人动手。见 `../Engineering/TechReports/sw/03-PC平台层与帧节拍.md` |
| 基础 Demo（SW6，赛题基础要求 3） | **待验收**（2026-09-24）：`sw/apps/game/gfx2dGame` 从 SW1 骨架换成基础 Demo——场景 A 是大量方块随机移动（N=100~5000、Q12.4、四个固定出屏样例、COPY／ALPHA／ADD 各走 FILL 与 BLIT），场景 B 是三张整屏图快速切换（初始化时预合成进 DDR 离屏层，每帧一次整屏 BLIT COPY）；默认演示模式，`-DSW6_JUDGE=1` 走判据模式回放两段入库录像。`tools/check/check_sw6_demo.sh` 十项全过：录像可重生成、判据模式 1226 帧 `fail 0` 且与入库期望日志逐行相同、有窗与无窗逐字节相同、两遍逐帧相同、HUD 的 `hcrc` 与 Python 侧按字库文本独立重画的整块矩形一致、三个 `scene` 参考值与每个 B 帧对上、四个方程各自注错见红、双缓冲不变量的反例见红、`SAN=1` 无报告、两种板上构建零告警。**板上对账已完成**：判据模式快速档（34,850 字节）经 `board_run.py --bin` 跑 480 帧 `fail 0`，从 `pak` 行起 489 行与 PC 期望日志逐行相同，原始串口日志 `sw/apps/game/gfx2dGame/logs/sw6_board_quick.log`。演示模式的 HDMI 画面与软硬件帧率对比归 SW9。见 `../Engineering/TechReports/sw/05-基础Demo.md`，条目 `../Engineering/Todo/sw/SW6-基础Demo.md` |
| SoC IP 定型（升级 3.4.0、接按键、预留中断与摄像头） | 已上板并验收（2026-09-19）：efx_soc 3.4.0 重新生成，开 GPIO1 接 KEY0~3、开用户中断 A/B/C、保留 I2C0 给摄像头、关 `axiA` 与 `io_ddrMasters_0`；工具链换成 gcc 13.4.0。调试通路连续 6 次会话、memTest、APB 探针、`ddrAliasTest`、`ddrBurstProbe`、E3 三件套、`bootSizeTest`（含对照组）与 gfx2dGame 60 帧 CRC 全过；HDMI 八色竖条与 SW0~SW3 按键已确认。见 `../Engineering/TechReports/platform/07-SoC-IP定型与工具链升级.md` |

编译占用 XLR 21807（35.87%）、片上 RAM 71 块（27.7%）、DSP 4 个，约束告警 0 条，无负 slack，`core_clk` setup 3.898 / hold 0.026 ns
（2026-09-21，扫描输出接入之后：相对上一版 XLR +154、片上 RAM +6 块——那 6 块就是扫描输出的 128 位 × 512 深异步 FIFO，
在设计文档 2.3 节 7 块的预算内；hold 没有被挤薄。像素域 `hdmi_tx_slow_clk` setup 33.807 / hold 0.071 ns。
上一版是 2026-09-19 SoC IP 定型之后的 XLR 21653、RAM 65 块、setup 3.405；
定型前是 XLR 21753、setup 3.780，差值归因见 platform 第 07 章；
E5 端口重分配前是 XLR 16151、setup 2.994，重分配后 21065、3.467，见 platform 第 06 章 5.1 节）。
这些数字相对第 01 章基线的变化见 `../Engineering/TechReports/platform/03-DDR3上的第二个主机与突发预算.md` 5.2 节；
SoC 构型的对比与资源归因见 `../Engineering/TechReports/platform/04-SoC构型对比与资源归因.md`；
五项上板判据的原始输出，以及基线是怎么搭起来的，见
[`../Engineering/TechReports/platform/01-从零搭起可上板的硬件基线.md`](../Engineering/TechReports/platform/01-从零搭起可上板的硬件基线.md)。
未完成的事项见 [`../Engineering/Todo/`](../Engineering/Todo/)。

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

本目录是工程本体。开发过程与因果链写在同级的 `../Engineering/` 下。厂商资料包不入库，取用方式见仓库根的 `../README.md`。

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
wsl -e bash -c 'cd sw/apps/game/gfx2dGame && make RISCV_BIN=<RISCV_BIN>'                  # 板上，产物 build/gfx2dGame-O2.bin
wsl -e bash -c 'cd sw && make -f mk/host.mk && ./build_host/gfx2dGame'          # PC，SDL2 窗口
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
| GPIO1（8 位，[3:0] 接 KEY0~3 即板上 SW0~SW3，低有效；[4] 预留摄像头复位） | `0xF801_6000` | `0x100` |
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
  提交前跑 `check_sw_layers.sh`，其中一项就是 `git diff --quiet HEAD -- Main/sdk`。
- **分层由编译结构保证**：`sw/lib/` 编的时候只有 `-Isw/lib`，应用编的时候没有 sdk 的头文件路径。
  越界不是靠人遵守，是编不过。见 `sw/README.md` 与 `../Engineering/TechReports/sw/01-软件平台分层与双目标构建.md`。
- **vendor 目录不改**：`rtl/vendor/` 下是厂商与第三方代码，必须改时在 `rtl/README.md` 里登记。
- **生成物不手改**：`sdk/` 与 `fpga/gfx2d_ti60/ip/soc/` 是 IP 生成的，改动登记在 `sdk/README.md`，
  重新生成后要逐条恢复。
- **不写绝对路径**：文档用 `doc/开发环境.md` 第 1 节的占位符，
  生成物里必须写路径的几个文件用 `tools/check/localize_paths.py` 展开。
  提交前跑一次 `--restore` 与 `--check`。
- **判据要能注错见红**：每个仿真除了正常用例，必须有把逻辑改错后会 FAIL 的用例，
  否则不算验证过。
- **提交前**：`hdmi`、`apb_probe` 两个仿真各 3/3，`axi_probe` 5/5，`axi_mux_guard` 17/17，Efinity 编译四步 PASS 且无负 slack，`--check` 通过。
  改了 `sw/` 下走新构建的程序（`gfx2dGame`、`cup3d`、`assetTest` 等）还要过 `sw/README.md` 里的两套构建、
  主机侧单元测试与 `tools/check/check_sw_layers.sh`；改了 `gfx2dGame`、`lib/gfx` 的执行器或素材还要过
  `tools/check/check_sw6_demo.sh`（十项，含四个方程的注错见红与 Sanitizer）。
- **数字要标明来源**：估算与推测必须写清楚，实测要给命令和结果。
- **状态与过程分开**：README 与 `doc/` 写当前是什么样，
  开发过程、踩过的坑、为什么这么选写在 `../Engineering/` 下。
