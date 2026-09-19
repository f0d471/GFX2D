# sdk：SoC 的板级支持包与官方示例

整个目录是 Sapphire SoC IP 生成的输出，**在 IP Manager 里重新生成 SoC 时会被覆盖**。本目录**只读**：本项目编写的程序不放这里，放 `sw/`。

项目对 sdk 的需求（板级封装、驱动包装、缓存失效的包装、地址映射）一律放在**叠在 sdk 之上的项目板级层** `sw/bsp/`（`ti60/` 与 `host/` 两个实现），由它转出去，不改 sdk 本身，
这样重新生成 IP 之后 `git diff --quiet HEAD -- Main/sdk` 仍然成立。分层与判据见
[`../sw/README.md`](../sw/README.md) 与 [`../../Engineering/TechReports/sw/01-软件平台分层与双目标构建.md`](../../Engineering/TechReports/sw/01-软件平台分层与双目标构建.md)。

本目录的内容由厂商 IP 生成，其中还带着 lwIP、FreeRTOS、CoreMark 等第三方组件，各自的授权以其文件头或同目录的许可证文件为准。

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
| gpioDemo 的 BSP 路径 | 生成时那台机器的绝对路径 | 占位符，由 `tools/check/localize_paths.py` 展开 | `software/standalone/gpio/gpioDemo/.cproject`、三个 `.launch` |
| 下载器描述字符串 | `ftdi device_desc "Titanium Ti60F225 Development Kit"` 生效 | 注释掉，只按 VID/PID 找下载器 | `bsp/efinix/EfxSapphireSoc/openocd/ftdi_ti.cfg` |

`gpioDemo` 的 `.cproject` 里还留着生成它的那台机器上 RISC-V IDE 2025.2 的工具链路径，
本项目不用 IDE 编译，所以没有动它。要在 IDE 里导入这个官方示例时，
由 IDE 重新指向本机的工具链。

程序区长度 4092K 由 SoC 配置里的 `LDSize` 决定，生成器直接写进链接脚本，重新生成后不用手改；
要恢复的只有断言那几行。下载器描述字符串一项是 2026-09-19 升级 3.4.0 时补登记的：
3.3.0 的软件包入库时这一行就是注释掉的，3.4.0 模板把它打开了；板上一直按注释掉的版本工作，
它与本板下载器的描述对不对得上没有核实过，所以保持原样。

改了 `bootloaderConfig.h` 就要重新编译 bootloader，并同步更新 `tool/bootloader/bootloader_512K.hex` 和 SoC IP 里的 8 个片上 RAM 初始文件（`fpga/gfx2d_ti60/ip/soc/` 与 `ip/soc/source/hardware/netlist/` 各 4 个）。做法与校验见 `../doc/开发环境.md` 第 9 节。

## 重新生成 SoC IP 时

当前软件包由 efx_soc 3.4.0（Efinity 2026.1）生成，2026-09-19。生成走 IP Manager 的 Python API，
不用图形界面，工具是 `../tools/soc/ipm_gen_soc.py`，做法与原因见
`../../Engineering/TechReports/platform/07-SoC-IP定型与工具链升级.md`。

1. 工作区干净，当前提交就是回退点；
2. `python3 tools/check/localize_paths.py --apply` 展开 `settings.json` 里 bootloader hex 的路径；
3. 在 Efinity 的环境里运行生成（`call <EFINITY>\bin\setup.bat` 之后用它自带的 python），
   参数改动用 `--override` 给出，先 `--dry-run` 看参数对照；主线生成加 `--sync-sdk Main/sdk`；
4. 脚本生成后自动做三件事：补回 `settings.json` 里被 API 写空的源文件清单、删掉 `ip/soc/` 下的示例目录、
   把软件包复制到本目录并删掉 `embedded_sw/`。用法见 `../tools/soc/README.md`；
5. 把上表逐条改回去，再看一次 `git diff Main/sdk`，剩下的每一处都要说得出来源；
6. `localize_paths.py --restore`，然后按 `../doc/开发环境.md` 0.4 与 0.6 节编译、上板回归。

## OpenOCD

- `ftdi_ti.cfg`：VID/PID `0403:6011`，`channel 1`。板上 FT4232 的 B 通道才是 JTAG，设计说明里写的 channel 0 与原理图矛盾，以这里为准。
- `debug_ti.cfg`：从当前目录往上最多找 4 级 `cpu0.yaml`。所以要在 `sw/` 或 `sdk/software/...` 下某个程序目录里启动 OpenOCD，或者手工指定路径。
