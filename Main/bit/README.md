# bit：比特流与 Flash 备份

`snapshots/` 的 B0/B3/B5 配套 `2d` 固件，B7 配套 `full`。软件由应用目录构建，
每档的 content.json、firmware.json 与 manifest 记录所选集合和程序/素材身份；
硬件 source_commit 与软件源码状态分开。仅换软件不重做无变化位流的综合。
详见 [快照说明](snapshots/README.md) 和 [构建工具](../tools/board/README.md)。

## official/

官方参考工程 `09_Ti60F225_hardjtag_demo` 目录里预编译好的比特流，文件名沿用官方的 `ddr_demo_ti60`：

| 文件 | 说明 |
| --- | --- |
| `ddr_demo_ti60.bit` | JTAG 加载用 |
| `ddr_demo_ti60.hex` | 写 Flash 用 |

它是 SoC + DDR3 的原始设计，不含 HDMI，bootloader 是官方的 124KB 版本。留着有两个用处：下载链路出问题时用它排除自己的设计；做 bootloader 对照实验时当 124KB 那一组。

两个文件都是文本，每字节一行十六进制，所以文件大小约为实际比特流的三倍（1.95MB 文本对应约 636KB 比特流）。

## 当前位流与软件

`snapshots/` 保存入库的四档位流、配套BIN和身份清单。Efinity 的最后一次构建位于
`fpga/gfx2d_ti60/outflow/`，它可能属于实验构型，不能仅凭文件名替代选定快照。

| 入口 | 用途 |
| --- | --- |
| `snapshots/B0/`、`B3/`、`B5/` | 当前2D固件：demo/stg/press，加菜单与系统信息 |
| `snapshots/B7/` | 当前完整固件：再加入R4 |
| `board_release_console_2d_20261030/` | 独立PC/板端2D包，包内配套位流为B0 |
| `board_release_console_full_20261030/` | 独立PC/板端完整包，包内配套位流为B7 |

B3/B5 使用各自快照的位流及相同2D BIN。应用集合以content.json为准；程序与硬件身份分别
登记，不按构型编号推断能力。两个独立包是本地忽略目录，快照不能替代包内PC EXE/SDL2。

双击包内 `Run-PC.cmd` 或 `Run-PC-Fullscreen.cmd` 启动Windows程序，不需要WSL或板卡。
菜单上下选择、Enter/Space确认、Esc返回，数字键是所选集合的可选快捷入口；
C切场景、Shift慢速/阶梯、Tab切绘制后端。F11/Alt+Enter全屏，F4切平滑/像素放大。
完整包R4按窗口比例适配宽屏，`Run-PC.ps1 -FixedAspect`保留固定画布对照。

`Run-Board.cmd`校验包内身份，自动定位UART，JTAG加载该包bit/BIN并连接面板，不写Flash。
快照用 `tools/board/board_run.py --bit <快照.bit> --bin <配套.bin>` 显式部署。
板端仍使用Flash中已有素材，程序从 `0x400000` 读取；改素材时需要独立验证。

旧发布目录已退役，当前入口与检查不依赖旧包。诊断程序保留在
`sw/apps/test/rastTest` 与 `featureTest`，检查工具按选定源码重新构建。
`r4-frozen-board`默认读取快照B7身份，并要求匹配同一位流SHA256的综合树与完整报告；
它只验收冻结硬件上的当前软件，不代表当前RTL重新综合。普通`r4-board`保留RTL一致性要求。

## flash_backup/

这是历史整片Flash备份，独立于位流快照和运行包，不在本轮退役范围。现有文件为：

| 路径 | 内容 |
| --- | --- |
| `flash_20261004_W25Q64_before_SW9_acceptance.hex` | 2026-10-04验收前的整片镜像 |
| `sw9_acceptance_20261004/flash_after.hex` | 对应验收写入后的整片回读 |
| `sw9_acceptance_20261004/flash_evidence.json` | 镜像、程序、素材摘要与未改写区域核对 |

两份hex内容不同，每份文本约24MiB、解码为8MiB。这些是历史回滚点，不代表当前快照已经
写入Flash。恢复方法见`doc/开发环境.md`第8节；日常JTAG加载不改写这些文件或Flash内容。
