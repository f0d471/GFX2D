# sdk：SoC 的板级支持包与官方示例

整个目录是 Sapphire SoC IP 生成的输出，**在 IP Manager 里重新生成 SoC 时会被覆盖**。我们自己写的程序不放这里，放 `sw/`。

本目录里做过的修改在下面「已登记的修改」一节，重新生成后必须逐条恢复，否则 bootloader 和链接脚本会退回官方默认值。

## 目录

| 路径 | 内容 | 是否用到 |
| --- | --- | --- |
| `bsp/efinix/EfxSapphireSoc/include/` | `soc.h`（地址映射、中断号）、驱动头文件、`print.h` | 用到 |
| `bsp/efinix/EfxSapphireSoc/linker/` | 链接脚本：`default.ld`、`freertos.ld`、`bootloader.ld`、两个 `_i` 版本（程序放片上 RAM） | 用到 |
| `bsp/efinix/EfxSapphireSoc/openocd/` | OpenOCD 配置：`ftdi_ti.cfg`（下载器）、`debug_ti.cfg`（目标）、`flash_ti.cfg`，以及寄存器描述 xml | 用到 |
| `bsp/efinix/EfxSapphireSoc/app/lwip/`、`app/fatfs/` | 网络协议栈与文件系统，共 262 个文件 | 没用到，是生成物，保留 |
| `cpu0.yaml` | VexRiscv 的 CPU 描述，OpenOCD 启动时要读 | 用到 |
| `software/standalone/` | 官方示例：uart、gpio、spi、i2c、timer、fpu、smp、coremark、dhrystone、memTest 等 | memTest 用到，其余作参考 |
| `software/standalone/bootloader/` | bootloader 源码，上电后从 Flash 拷程序到 DDR 再跳转 | 用到，已修改 |
| `software/standalone/common/` | `start.S`、三个 makefile 片段（`bsp.mk`、`riscv64-unknown-elf.mk`、`standalone.mk`） | 用到，`sw/` 下的程序也引用它们 |
| `software/freeRTOS/` | FreeRTOS 示例 | 没用到 |
| `tool/` | `binGen.py`（生成片上 RAM 初始文件）、编译好的 bootloader bin/hex | 用到 |

## 已登记的修改

| 项 | 官方值 | 现值 | 文件 |
| --- | --- | --- | --- |
| bootloader 拷贝长度 `USER_SOFTWARE_SIZE` | `0x01F000`（124KB） | `0x080000`（512KB） | `software/standalone/bootloader/src/bootloaderConfig.h` |
| 程序区长度 | 124K | 4092K | `bsp/.../linker/default.ld`、`freertos.ld` |
| 镜像超限断言 | 无 | 超过 512KB 时链接报错 | 同上 |
| 编译好的 bootloader | 无 | `tool/bootloader/bootloader_512K.bin` 与 `.hex` | 新增 |
| gpioDemo 的 BSP 路径 | 官方机器的绝对路径 | 本机路径 | `software/standalone/gpio/gpioDemo/.cproject`、三个 `.launch` |

改了 `bootloaderConfig.h` 就要重新编译 bootloader，并同步更新 `tool/bootloader/bootloader_512K.hex` 和 SoC IP 里的 8 个片上 RAM 初始文件（`fpga/gfx2d_ti60/ip/soc/` 与 `ip/soc/source/hardware/netlist/` 各 4 个）。做法与校验见 `../README.md` 的「Bootloader 与程序区」。

## 重新生成 SoC IP 时

1. 生成前提交或备份，生成器会覆盖 `ip/soc/` 和软件包；
2. 确认 Cache/Memory 页勾了 Custom On-Chip RAM Application，路径指向 `sdk/tool/bootloader/bootloader_512K.hex`，Application Region Size 选 4MB；
3. 生成后把新软件包同步到本目录，再把上表逐条改回去；
4. 2026.1 的软件包模板 `-march` 带 `_zicsr_zifencei`，同步后可以改用 IDE 自带的 gcc 13.4，届时 `sw/README.md` 里的编译命令要一起改。

## OpenOCD

- `ftdi_ti.cfg`：VID/PID `0403:6011`，`channel 1`。板上 FT4232 的 B 通道才是 JTAG，设计说明里写的 channel 0 与原理图矛盾，以这里为准。
- `debug_ti.cfg`：从当前目录往上最多找 4 级 `cpu0.yaml`。所以要在 `sw/` 或 `sdk/software/...` 下某个程序目录里启动 OpenOCD，或者手工指定路径。
