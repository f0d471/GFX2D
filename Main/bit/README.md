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

## flash_backup/

板上 Flash 的整片备份，不入版本库（每份 24MB）。

| 文件 | 说明 |
| --- | --- |
| `flash_20260916_W25Q64_before_bootSizeTest.hex` | 2026-09-16 写 bootSizeTest 之前的出厂内容。`0x000000`~`0x0C74E2` 是摄像头转 HDMI 的演示比特流，其余全为 `0xFF` |
| `flash_20261003_W25Q64_before_SW9.hex` | 2026-10-03 写板上最终交互程序前的整片内容；解码为 8 MiB，素材分区与当前 `assets.pak` 逐字节相同。文本文件 SHA256 `4ade2808b5d71a375e281efeefbd40b923eb0dc54d55361935ce560639a2bf47` |

备份格式与 Programmer 的 hex 相同，可以原样写回。恢复方法与读写命令见 `doc/开发环境.md` 第 8 节。
