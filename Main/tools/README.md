# tools：辅助脚本

与硬件、软件构建无关的小工具放这里。

| 文件 | 用途 |
| --- | --- |
| `bin2hex.py` | 把程序 bin 转成 Efinity Programmer 用的 hex 文本（每字节一行、两位大写十六进制、LF 换行），写 Flash 前用 |
| `localize_paths.py` | 展开、还原与检查仓库里的绝对路径 |
| `gen_soc.py` | 从命令行生成 Sapphire SoC IP，可写构型参数，不必开 IP Manager 图形界面 |
| `board_run.py` | 烧比特流、先开串口、加载程序、回收串口输出，一条命令走完 |
| `serial_capture.ps1` | 在 Windows 侧读串口并写日志，与 `run_openocd.sh` 配对使用 |
| `run_openocd.sh` | 在 WSL 侧调 Windows 的 `openocd.exe`，`reset halt` 后加载程序并 `resume` |
| `check_round255.c` | 穷举校验 ÷255 的舍入、中间量位宽、「舍入/饱和顺序」与预乘路径的量化误差，E7 规格（设计文档 3.5 节）的依据 |
| `verify_round255.c` | 复核程序：把容易混淆的候选模型（预乘 / 非预乘 ALPHA、CONST、按字面实现的系数）分开穷举，独立复现 `check_round255.c` 的数字 |
| `check_sw_layers.sh` | 软件分层与结构判据：`lib/` 的可见范围、注错能不能编过、同名目标文件、软浮点符号、`sdk/` 无改动，共五项含三处注错。见下 |
| `openocd/gfx2d_debug.cfg` | 板上调试用的 OpenOCD 配置，修掉官方 cfg 在本板上的两处对不上 |

五个脚本都只用标准库（`board_run.py` 额外用 Efinity 自带 Python 里的 pyserial 3.5）。
本机如果没有 Windows 版 python，用 `wsl -e python3 <脚本>` 调用；
需要 Access 串口与调用 Efinity 工具的两个脚本要用 Efinity 自带的解释器，见下。

## localize_paths.py

有五个文件由厂商工具生成，里面必须写绝对路径：`fpga/gfx2d_ti60/ip/soc/settings.json`
与 gpioDemo 的 `.cproject`、三个 `.launch`。仓库里存的是占位符形式。

```
python3 tools/localize_paths.py --apply      占位符 -> 本机路径，克隆之后跑一次
python3 tools/localize_paths.py --restore    本机路径 -> 占位符，提交之前跑一次
python3 tools/localize_paths.py --check      扫描整个仓库，发现机器专属路径就以非零退出码结束
```

`--check` 找的是盘符路径、`/home/<用户>/` 和 `/mnt/<盘符>/` 三类写法。
厂商代码与 IP 生成物不在扫描范围内，它们带的是生成时那台机器的工具安装位置。
唯一允许出现实际路径的文档是 `doc/开发环境.md` 第 11 节，脚本里已登记豁免。

## check_sw_layers.sh

软件分层的结构判据，提交前跑（见 [`../sw/README.md`](../sw/README.md)）：

```
wsl -e bash -c 'RISCV_BIN=<RISCV_BIN> bash Main/tools/check_sw_layers.sh'
```

| 项 | 内容 |
| --- | --- |
| 1 | `lib/` 在只有 `-Isw/lib` 的条件下编译通过（主机 gcc） |
| 2 | 注错：`lib/` 里 `#include "board.h"` 必须编不过；应用里 `#include <soc.h>` 必须编不过。另有一个对照组，合法的 `board.h` 与 `lib` 头文件必须编得过 |
| 3 | 注错：`lib/core` 与 `bsp/ti60` 各放一个同名的 `x.c`，板上构建必须产出两份 `.o` 且链接出两份符号 |
| 4 | `nm` 扫 `lib/` 与应用的目标文件没有软浮点符号；注错：临时放一个 `float` 运算必须被扫出来 |
| 5 | `git diff --quiet HEAD -- Main/sdk`，放行 `Main/sdk/README.md`（见脚本内注释） |
| 6 | 改了头文件必须重编**并重链**（`touch bsp/include/memmap.h` 后要出现 `CC main.c` 与 `LD`）；对照：没有改动时两者都不出现 |
| 7 | 缓存行区间折算：主机探针包含 `bsp/include/dcache_line.h` 跑八个算例，含 `0x3F` + 2 → `0x00..0x40` |

`RISCV_BIN` 是工具链前缀（第 1 节占位符）。**第 2、3、4、6 项跑真实的板上构建**，
产物落临时目录；第 1、7 项用主机 gcc。判据与被测构建必须用同一组参数，
理由见 [`../../Engineering/TechReports/sw/01-软件平台分层与双目标构建.md`](../../Engineering/TechReports/sw/01-软件平台分层与双目标构建.md) 4.6 节。
注错文件只在运行时临时生成，脚本退出（含中途失败）一律删干净；
第 6 项会 `touch` 一次 `bsp/include/memmap.h`（只改时间戳，不改内容）；
编译产物一律落临时目录，不动 `sw/apps/game/gfx2dGame/build/`。

## bin2hex.py

```
python3 tools/bin2hex.py <输入.bin> <输出.hex>
```

转换后会把 hex 解码回字节与输入逐字节比较，不一致时退出码为 1。

烧写命令见 `doc/开发环境.md` 第 8 节。Efinity 的 `jtag_bridge` 模式只接受 hex 文本，不接受裸 bin，所以写 Flash 前必须先转换。

## gen_soc.py

IP 生成只有图形界面入口，要批量生成几套构型做对比时点界面既慢又不留痕。
这个脚本复现 IP Manager 对厂商生成器 `soc_gen.py` 的调用，把构型写成命令行参数：

```
<EFINITY>\python311\bin\python.exe tools/gen_soc.py --project Main/fpga/gfx2d_ti60 \
    --override SOC_MODE=1 DDRCLK_DOMAIN=0 MULDIV_EXT=1 BARREL_SHIFTER=1
```

`--dry-run`（只打印参数与命令行，不生成）与 `--gen-name` 也在支持之列。
基准参数从 `<project>/ip/soc/settings.json` 读，`--override` 按需覆盖；
覆盖一个不存在的键会直接报错，避免键名写错而静默无效。

它替生成器补掉三件事，缺一件就生成不了，原因都写在脚本头部：
Verilog 字面量（`1'b1`、`32'd1048576`）会让生成器的 `eval` 抛 `SyntaxError`；
手工编辑过的 `settings.json` 丢失了 IP 定义里 `resolve="immediate"` 的自动参数
（生成模板引用它们，缺一个就抛 `ValueError: Unknown format code 'x'`）；
地址类参数在模板里要求 int 而配置里是字符串。

**生成会覆盖 `<project>/ip/soc/` 与软件包。** 生成前先提交或备份，
生成后按 `sdk/README.md` 的「已登记的修改」逐条恢复。

## board_run.py 与 openocd/gfx2d_debug.cfg

上板跑程序要按顺序做四件事，其中两步容易出错：串口没开时板子发出的数据会被丢弃
（`doc/开发环境.md` 0.6 节），以及 `sdk/` 里那两份官方 OpenOCD 配置在本板上不能直接用。

```
<EFINITY>\python311\bin\python.exe tools/board_run.py --repo . \
    --bin <程序.bin> --seconds 60 --stop-on done
```

| 参数 | 说明 |
| --- | --- |
| `--bit` / `--no-program` | 指定或跳过要烧的比特流；默认烧 `Main/fpga/gfx2d_ti60/outflow/gfx2d_ti60.bit` |
| `--port` | 串口号，默认 `COM11`（`doc/开发环境.md` 第 11 节的参考值） |
| `--stop-on` | 收到该文本就提前结束采集，适合程序末尾有 `done` 的情形 |
| `--check-only` | 只做连通性检查：连续开两次 OpenOCD 会话各读一次 PC |
| `--list-ports` / `--dry-run` | 列串口 / 只打印将执行的命令 |

`openocd/gfx2d_debug.cfg` 修掉官方配置在本板上的两处对不上，两处都是实测确认的：

1. `debug.cfg` 的 `jtag newtap ... -irlen 4` 与本板不符，本板 IR 宽度是 5
   （`ftdi_pgm.bat --scan_usb` 报 `ir_width 5`），用 4 会报
   `IR capture error at bit 4, saw 0x25 not 0x..3`，连不上 CPU。
2. 两份官方 cfg 的 `cpu0.yaml` 查找都是写死往上 3 级，而 `Main/sdk/cpu0.yaml` 不在实际运行目录的
   上级链路上（程序在 `sw/apps/<类>/<程序>/` 下，往上 4 级到 `Main/` 再进 `sdk/`）。本文件改用它们的 `LEGACY_IDE` 分支，
   由环境变量 `workspace_loc` 直接给出目录，`board_run.py` 会设好。

这两条不改 `sdk/` 下的官方文件：那是 IP 生成物，重新生成会被覆盖。

**烧写步骤不能只看退出码。** `ftdi_pgm.bat` 只在 `EFINITY_HOME` 未定义时才自己调
`bin\setup.bat`；父进程里设过它、却没设 `PYTHONHOME` 之类时，厂商脚本跳过 setup.bat，
内部 python 以 `Fatal Python error`（或 `KeyError: EFXPGM_HOME`）退出，
**退出码仍然是 0**。原先 `board_run.py` 据退出码把这种情况当成功，
接着用 FPGA 里原有的比特流跑程序，串口输出看不出任何异常（2026-09-19 复核发现）。
现在烧写这一步会先把 Efinity 的环境变量从子进程里去掉、让厂商脚本自己配好，
并且要求输出里出现成功标记 `finished with JTAG programming` 才算通过；
失败时把完整输出打出来再退出。

## serial_capture.ps1 与 run_openocd.sh

本机上直接调 Efinity 自带的 Python 会失败，两个环境各缺一半：
`<EFINITY>/python311` 的 `python.exe` 在 `bin\` 下、标准库在同级的 `..\Lib`，
Python 推不出 prefix，不设 `PYTHONHOME` 时报 `ModuleNotFoundError: No module named 'encodings'`（**不是缺标准库**）；
WSL 里有 pyserial 与串口驱动，但拿不到 Windows 的 COM 口。
补上 `PYTHONHOME` 之后 `board_run.py` 是可用的：

```
set PYTHONHOME=<EFINITY>\python311
<EFINITY>\python311\bin\python.exe tools\board_run.py --repo <REPO> --list-ports
```

上面这条 2026-09-18 复核过：解释器 3.11.10、pyserial 3.5，`--list-ports` 正常退出并列出串口。
下面这套两个脚本的分工仍然保留，它不依赖 `board_run.py`，也适用于 WSL 侧拿不到 COM 口的场合：
WSL 侧只调 `openocd.exe`，串口由 Windows 侧读。

```
# 1 先开串口采集（后台，必须早于加载程序）
pwsh -File tools/serial_capture.ps1 -Port COM11 -Log <日志> -Seconds 25 -StopOn done

# 2 再加载程序并 resume（先 JTAG 加载本工程的 .bit，见 doc/开发环境.md 0.6 节）
wsl -e bash -c 'EFINITY_IDE_HOME=/mnt/<盘符>/<IDE 安装目录> bash tools/run_openocd.sh <程序.bin> 0x1000'
```

`run_openocd.sh` 按 `EFINITY_IDE_HOME` 找 `openocd.exe`（`<IDE>/openocd/bin/`）；没设时用
`EFINITY_HOME` 推出与 Efinity 同级的 `efinity-riscv-ide-*` 目录。两者都没设就报「找不到 openocd.exe」退出。
脚本在加载前做 `reset halt`：不复位时，前一次实验把探针主机留在坏状态，程序一行都不跑，串口全空。

两个脚本各绕开一个坑，都是实测确认的：

1. **WSL 不把自定义环境变量传给 Windows 进程。** `gfx2d_debug.cfg` 第 59 行要的
   `workspace_loc` 在 WSL 侧设不进去，改用 OpenOCD 的 `-c` 在 Tcl 里填。
   另外 `openocd.exe` 是 Windows 程序，读不了 `/mnt/<盘符>/...`，`run_openocd.sh`
   会把这种参数转成 `<盘符>:/...`。
2. **`serial_capture.ps1` 必须逐字节读。** 第一版用 `BaseStream.Read`，
   那是阻塞调用，`ReadTimeout` 对它不生效，采集进程不按时退出并且一直占着 COM 口
   （第二次采集报 `Access to the path 'COM11' is denied`）。

用 `--list-ports` 那类检查仍然可以用 `board_run.py`，它只在真正上板的那一步不行。
