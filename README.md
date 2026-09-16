# gfx2d：2D 渲染加速引擎

2026 嵌入式芯片与系统设计竞赛，易灵思 FPGA 创新设计赛道，赛题二：基于 RISC-V 与 FPGA 异构架构的 2D 图形渲染加速引擎。板卡为钛金 Ti60F225 开发板。

工程名 `gfx2d_ti60`，顶层模块 `gfx2d_top`。

## 现在的状态

| 部分 | 状态 |
| --- | --- |
| SoC（Sapphire RISC-V，100MHz）+ DDR3（256MB） | 已上板，memTest 通过 |
| HDMI 输出 640x480@60 | 已上板，八色竖条显示正确。画面来自彩条发生器，还没有接帧缓冲 |
| 程序从 Flash 引导（512KB 镜像） | 已上板验证 |
| APB 寄存器通路 | 已修好并接入探针寄存器，等上板读写验证 |
| 渲染引擎、帧缓冲、双缓冲 | 还没有开始 |
| SoC 资源裁剪 | 还没有做 |

详细清单、可裁剪项与下一步见 `doc/开工前准备情况.md`。

## 目录

| 目录 | 内容 | 说明 |
| --- | --- | --- |
| `rtl/` | 硬件设计源码 | 顶层、APB 从设备、HDMI、厂商 IP，见 `rtl/README.md` |
| `fpga/` | Efinity 工程 | 工程文件、接口设计、约束、SoC IP，见 `fpga/README.md` |
| `sdk/` | SoC 的 BSP 与官方示例 | IP 生成物，改动要登记，见 `sdk/README.md` |
| `sw/` | 自己写的 RISC-V 程序 | 见 `sw/README.md` |
| `sim/` | 仿真 | 见 `sim/README.md` |
| `bit/` | 官方比特流与 Flash 备份 | 见 `bit/README.md` |
| `tools/` | 辅助脚本 | 见 `tools/README.md` |
| `doc/` | 方案、实测记录、准备情况 | 见 `doc/README.md` |

每个目录的 README 负责说明本目录下的文件从哪来、能不能改、有没有被工程引用。

## 快速开始

编译硬件：

```
call C:\Efinity\2026.1\bin\setup.bat
cd fpga\gfx2d_ti60
efx_run.bat gfx2d_ti60.xml
```

加载到板上（掉电丢失，不动 Flash）：

```
C:\Efinity\2026.1\pgm\bin\ftdi_pgm.bat -m jtag -u ftdi://0x0403:0x6011:0:ff/1 fpga\gfx2d_ti60\outflow\gfx2d_ti60.bit
```

跑仿真：

```
wsl -e bash -c 'cd /mnt/e/Workspace/Efinix/Main && bash sim/hdmi/run.sh && bash sim/apb_probe/run.sh'
```

编译并用调试器运行一个程序，见 `sw/README.md`；写 Flash 与整片备份，见 `doc/板卡实测记录.md` 第 8 节。

## 地址映射

CPU 看到的地址，取自 `sdk/bsp/efinix/EfxSapphireSoc/include/soc.h`：

| 区域 | 基址 | 大小 |
| --- | --- | --- |
| DDR3 | `0x0000_1000` | 窗口 `0xE000_0000`，实际颗粒 256MB |
| AXI 从设备窗口（未使用） | `0xE100_0000` | 16MB |
| UART0 | `0xF801_0000` | `0x40` |
| SPI0 | `0xF801_4000` | `0x1000` |
| GPIO0（4 位，接 LED0~3） | `0xF801_5000` | `0x100` |
| I2C0（未使用） | `0xF801_6000` | `0x100` |
| APB Slave 0 → `apb_probe` | `0xF810_0000` | 64KB |
| CLINT | `0xF8B0_0000` | `0x1_0000` |
| PLIC | `0xF8C0_0000` | `0x40_0000` |
| 片上 RAM（bootloader） | `0xF900_0000` | 4KB |

中断号：UART0 = 1，SPI0 = 4，I2C0 = 8，用户中断 A = 16，AXI 从设备窗口 = 30。

DDR3 与 Flash 的分区规划见 `doc/赛题二总体设计.md` 2.1、2.2 节。

## 约定

- **vendor 目录不改**：`rtl/vendor/` 下是厂商与第三方代码，必须改时在 `rtl/README.md` 里登记。
- **生成物不手改**：`sdk/` 与 `fpga/gfx2d_ti60/ip/soc/` 是 IP 生成的，改动登记在 `sdk/README.md`，重新生成后要逐条恢复。
- **判据要能注错见红**：每个仿真除了正常用例，必须有把逻辑改错后会 FAIL 的用例，否则不算验证过。
- **提交前**：两个仿真各 3/3，Efinity 编译四步 PASS 且无负 slack。
- **数字要标明来源**：估算与推测必须写清楚，实测要给命令和结果。
