# gfx2d：2D 渲染加速引擎

2026 嵌入式芯片与系统设计竞赛，易灵思 FPGA 创新设计赛道，赛题二：
基于 RISC-V 与 FPGA 异构架构的 2D 图形渲染加速引擎。板卡为钛金 Ti60F225 开发板。

| 目录 | 内容 | 入口 |
| --- | --- | --- |
| `Main/` | 工程本体：RTL、Efinity 工程、SoC 的 BSP、程序、仿真、方案与环境手册 | [`Main/README.md`](Main/README.md) |
| `Engineering/` | 开发过程：未完成事项、缺陷、技术报告 | [`Engineering/README.md`](Engineering/README.md) |

接手顺序：先看 [`Main/doc/赛题二总体设计.md`](Main/doc/赛题二总体设计.md)（打算怎么做），
再看 [`Engineering/TechReports/platform/01-从零搭起可上板的硬件基线.md`](Engineering/TechReports/platform/01-从零搭起可上板的硬件基线.md)（现在的基线是怎么来的），
动手前看 [`Main/doc/开发环境.md`](Main/doc/开发环境.md)（工具与命令）。

## 需要另外准备的东西

**硬件**：Ti60F225 开发板一块（板载 FT4232 下载器，一根 USB 线供电加调试）、
HDMI 显示器与线。

**工具链**：Efinity 2026.1、Efinity RISC-V 嵌入式 IDE、Java 8、libusbK 驱动、
WSL 里的 iverilog 与 python3、xPack 的 riscv-none-embed-gcc 8.3.0。
版本要求、获取方式与安装后的检查见 [`Main/doc/开发环境.md`](Main/doc/开发环境.md) 第 2 节。

**厂商资料包**：不在本仓库里。它约 7 GB，著作权属于厂商与竞赛组织方，
取自随板资料、竞赛群和 Efinix 官网文档页。把它解压到仓库根目录下的 `Efinix/`，
文档里形如 `Efinix/02_.../xxx` 的引用就能对上（`.gitignore` 已经排除这个目录）。
仓库里的文档实际引用到的是下面几份，只准备这些也够：

| 路径 | 用来做什么 |
| --- | --- |
| `01_赛题指南/易灵思FPGA 创新设计赛道选题指南.pdf` | 赛题原文，第 7~9 页 |
| `02_.../TI60F225I3-V4 DEMO板软硬件设计说明-260813-1.pdf` | 管脚、HDMI 接口、Flash 固化流程 |
| `02_.../Ti60F225A_V4_SCH.pdf` | 原理图。FT4232 的 JTAG 通道以它为准，官方文档那句与它矛盾 |
| `02_.../08_ti60f225_soc_demo/09_Ti60F225_hardjtag_demo/` | SoC 加 DDR3 的官方工程，本仓基线的来源，也是上板时的对照组 |
| `02_.../03_hdmi_tx_demo/` | `Main/rtl/hdmi/` 里三个官方文件的原件，核对 MD5 用 |
| `02_.../10_Ti60f225_sc431hai2hdmi_demo/` | 帧缓冲与 HDMI 输出的参考实现 |
| `02_.../驱动安装及说明/zadig-2.8.exe` | 给下载器换 libusbK 驱动 |
| `02_.../软件安装包/jre-8u381-windows-x64.exe` | Java 8，生成或升级 SoC IP 时要用 |
| `03_.../03_芯片数据手册/钛金Ti60datasheet-v2_0.pdf` | 片上 RAM 与 DSP 的规格 |
| `04_TinyML/tinyml-main/.../src/tinyml_soc_top.v` | 多个 AXI 主机共享 DDR3 的官方接法 |
| `07_官网补充文档/` | Sapphire SoC、DDR3 控制器、AXI 互连的用户手册 |

官方预编译的比特流已经收在 [`Main/bit/official/`](Main/bit/official/)，不必另找。

## 克隆之后

有五个文件是厂商工具生成的，里面必须写绝对路径，仓库里存的是占位符，展开一次：

```
python3 Main/tools/localize_paths.py --apply
```

Windows 上没有 python 时用 WSL 调用：`wsl -e python3 Main/tools/localize_paths.py --apply`。

## 提交之前

```
python3 Main/tools/localize_paths.py --restore   本机路径还原成占位符
python3 Main/tools/localize_paths.py --check     扫描机器专属的绝对路径
```

改了 RTL 或程序，还要跑两个仿真与一次 Efinity 编译，判据见
[`Main/README.md`](Main/README.md) 的「约定」。

## 一条仓库级规则

**不写绝对路径。** 文档用 `Main/doc/开发环境.md` 第 1 节的占位符，
实际取值只出现在该文件第 11 节。厂商生成物里必须写路径的几个文件用
`Main/tools/localize_paths.py` 展开与还原。编译与仿真产物一律不入库。
