# bit：比特流与 Flash 备份

## official/

官方参考工程 `09_Ti60F225_hardjtag_demo` 目录里预编译好的比特流，文件名沿用官方的 `ddr_demo_ti60`：

| 文件 | 说明 |
| --- | --- |
| `ddr_demo_ti60.bit` | JTAG 加载用 |
| `ddr_demo_ti60.hex` | 写 Flash 用 |

它是 SoC + DDR3 的原始设计，不含 HDMI，bootloader 是官方的 124KB 版本。留着有两个用处：下载链路出问题时用它排除自己的设计；做 bootloader 对照实验时当 124KB 那一组。

两个文件都是文本，每字节一行十六进制，所以文件大小约为实际比特流的三倍（1.95MB 文本对应约 636KB 比特流）。

## 本项目的比特流

不入版本库。Efinity 编译后在 `fpga/gfx2d_ti60/outflow/gfx2d_ti60.bit`，`.gitignore` 已排除 `outflow/`。

## board_release_*/

每次整机编译留一份的上板复现包：比特流加配套程序，并随包带上 OpenOCD 配置与 `cpu0.yaml`，
所以脱离本仓库也能把游戏跑起来。不入版本库（`.gitignore` 按 `Main/bit/board_release_*/` 排除）。

| 目录 | 内容 | 实测 |
| --- | --- | --- |
| `board_release_20261015/` | 干净工作区（提交 `6c2d59c`）编译：`gfx2d_ti60.bit`/`.hex`、`gfx2dGame-O2-perf.bin`（硬件 DDR 后端、240 帧判据构建）、`gfx2d_debug.cfg`、`cpu0.yaml`、上板原始日志 | 2026-10-15 上板 `fail 0`、60 fps、掉帧 0、`faults 0`、扫描 FIFO 最低 336 拍；整机 60064 XLR / 172 RAM / 23 DSP，`core_clk` setup/hold +0.104/+0.022 ns |

程序从板上 Flash 的 `0x400000` 读素材，素材被改写后日志里 `pak` 行的 CRC 会对不上。
复现步骤（含不依赖工程的裸命令）见各目录自己的 `README.md`；文件摘要以各目录的
`manifest.json` 为唯一清单，本文件不重复抄录。

## 本轮旧版与新版游戏

两个目录都保留完整程序、素材、位流、源码快照和独立工具，不随Main源码继续修改而改变游戏负载：

| 目录 | PC入口 | 板端入口 | 说明 |
| --- | --- | --- | --- |
| [board_release_game_wide_20261025/](board_release_game_wide_20261025/) | `Run-PC.cmd` / `Run-PC-Fullscreen.cmd` | `Run-Board.cmd` | PC模式4自适应宽屏；`Run-PC.ps1 -FixedAspect`保留640×480；板端仍为固定B7路径 |
| [board_release_game_current_20261024/](board_release_game_current_20261024/) | `Run-PC.cmd` | `Run-Board.cmd` | 冻结旧版；PC启动模式2，按4进三维；板端直接模式4/L2/HW；保留原窗口功能 |
| [board_release_game_next_20261024/](board_release_game_next_20261024/) | `Run-PC.cmd` / `Run-PC-Fullscreen.cmd` | `Run-Board.cmd` | 软件视觉升级；PC与板端直接三维L2，PC为实际CPU光栅、板端为B7硬件 |

PC双击入口即可，不需要WSL、Python或板卡。新版F11/Alt+Enter切全屏、Esc退回，F4切平滑/像素放大，
F1/F2/F3改窗口大小；保持4:3。WASD/方向键移动，Space炸弹，三维内Shift切L1/L2、4切走廊/茶杯。

板端需要本机Efinity/IDE、驱动与USB连接；入口自动定位C通道UART，校验包内文件、重新加载bit/BIN并打开面板。
先关闭占用同一串口的旧面板，再启动另一个版本；板卡每次只运行一个版本。这里走JTAG，不写Flash、
不改变上电默认程序。两个包均保留同一assets.pak，板端使用Flash里已验证的原素材；三维atlas嵌入各自BIN。

`manifest.json`登记文件身份；`runtime/`保存本次加载/面板记录。旧包只修了PS5.1启动校验器，原游戏
EXE/BIN/bit/素材/source.zip未改，原身份保留在manifest.original.json。新版带验证摘要、原生Windows/实板
对照图和ELF；正式性能BIN与发布无限BIN的身份、参数分别登记，不冒充同一个构建。

## flash_backup/

板上 Flash 的整片备份，不入版本库（每份 24MB）。

| 文件 | 说明 |
| --- | --- |
| `flash_20260916_W25Q64_before_bootSizeTest.hex` | 2026-09-16 写 bootSizeTest 之前的出厂内容。`0x000000`~`0x0C74E2` 是摄像头转 HDMI 的演示比特流，其余全为 `0xFF` |
| `flash_20261003_W25Q64_before_SW9.hex` | 2026-10-03 写板上最终交互程序前的整片内容；解码为 8 MiB，素材分区与当前 `assets.pak` 逐字节相同。文本文件 SHA256 `4ade2808b5d71a375e281efeefbd40b923eb0dc54d55361935ce560639a2bf47` |

备份格式与 Programmer 的 hex 相同，可以原样写回。恢复方法与读写命令见 `doc/开发环境.md` 第 8 节。
