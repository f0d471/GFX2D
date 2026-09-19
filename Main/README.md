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
| 像素规格（E7 §2.1） | 已定稿并经用户确认（2026-09-18）：写进 `doc/赛题二总体设计.md` 3.5 节，舍入、位宽、舍入顺序与预乘量化误差有穷举校验（`tools/check/check_round255.c`）。E7 整体未验收，软件执行器未开工 |
| 渲染引擎、帧缓冲、双缓冲 | 还没有开始 |
| Axi_Mux 端口重分配与防饿死（E5 §2.1~2.3） | 已上板验证（2026-09-19）：`Axi_Mux` 用 X3，S0=扫描替身、S1=探针、S2=SoC；永久模块 `axi_mux_guard`（`0xF810_F000`）给出仲裁方式、三通道掩码与 SoC 地址等待保护，`scanout_stub`（`0xF810_E000`）按行节拍发 160 拍读并记录首拍/整笔/拍间隙。三方并发下 CPU 拷贝 1.70 倍（探针读）、1.92 倍（探针写），扫描没有跳行，S0 掩码反例与 `AGE_LIMIT=1` 注错都符合预期，`ddrBurstProbe` 连跑三轮 `fail 0`；guard VERSION 2（2026-09-19 复核：两个 MAX 每拍更新，写方向「CPU 被停住」的结论是测试台错误，已撤回）。**2.1~2.3 已验收（2026-09-19）**，见 `../Engineering/TechReports/platform/06-Axi_Mux三端口仲裁与SoC防饿死保护.md` 与 `../Engineering/Todo/hw/E5-Axi_Mux优先级与扫描FIFO水位.md`。**2.4 真实扫描 FIFO 水位待扫描输出存在后再做** |
| 软件平台分层与双目标构建（SW1） | **已验收**（2026-09-19）：`sw/bsp/`（`ti60/` 与 `host/`）、`sw/lib/`、两套构建 `sw/mk/{target,host}.mk` 与判据脚本 `tools/check/check_sw_layers.sh` 就位。PC 与板上 60 帧整帧 CRC 逐帧相同，分层与注错判据全过。见 `../Engineering/TechReports/sw/01-软件平台分层与双目标构建.md` |
| SoC IP 定型（升级 3.4.0、接按键、预留中断与摄像头） | 已上板并验收（2026-09-19）：efx_soc 3.4.0 重新生成，开 GPIO1 接 KEY0~3、开用户中断 A/B/C、保留 I2C0 给摄像头、关 `axiA` 与 `io_ddrMasters_0`；工具链换成 gcc 13.4.0。调试通路连续 6 次会话、memTest、APB 探针、`ddrAliasTest`、`ddrBurstProbe`、E3 三件套、`bootSizeTest`（含对照组）与 gfx2dGame 60 帧 CRC 全过；HDMI 八色竖条与 SW0~SW3 按键已由用户确认。见 `../Engineering/TechReports/platform/07-SoC-IP定型与工具链升级.md` |

编译占用 XLR 21653（35.61%）、片上 RAM 65 块（25.4%）、DSP 4 个，约束告警 0 条，无负 slack，`core_clk` setup 3.405 / hold 0.026 ns
（2026-09-19，SoC IP 定型之后；定型前是 XLR 21753、setup 3.780，差值归因见 platform 第 07 章；E5 端口重分配前是 XLR 16151、setup 2.994，重分配后 21065、3.467，见 platform 第 06 章 5.1 节）。
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
用户中断 A = 16（扫描输出场同步）、B = 17（引擎）、C = 22（摄像头），三条的源模块接入前在顶层接 0。
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
  改了 `sw/` 下走新构建的程序（`gfx2dGame` 与以后的应用）还要过 `sw/README.md` 里的两套构建、
  主机侧单元测试与 `tools/check/check_sw_layers.sh`。
- **数字要标明来源**：估算与推测必须写清楚，实测要给命令和结果。
- **状态与过程分开**：README 与 `doc/` 写当前是什么样，
  开发过程、踩过的坑、为什么这么选写在 `../Engineering/` 下。
