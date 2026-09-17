# tools：辅助脚本

与硬件、软件构建无关的小工具放这里。

| 文件 | 用途 |
| --- | --- |
| `bin2hex.py` | 把程序 bin 转成 Efinity Programmer 用的 hex 文本（每字节一行、两位大写十六进制、LF 换行），写 Flash 前用 |
| `localize_paths.py` | 展开、还原与检查仓库里的绝对路径 |
| `gen_soc.py` | 从命令行生成 Sapphire SoC IP，可写构型参数，不必开 IP Manager 图形界面 |
| `board_run.py` | 烧比特流、先开串口、加载程序、回收串口输出，一条命令走完 |
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
2. 两份官方 cfg 的 `cpu0.yaml` 查找都是写死往上 3 级，而实际运行目录离
   `Main/sdk/cpu0.yaml` 有 5 级。本文件改用它们的 `LEGACY_IDE` 分支，
   由环境变量 `workspace_loc` 直接给出目录，`board_run.py` 会设好。

这两条不改 `sdk/` 下的官方文件：那是 IP 生成物，重新生成会被覆盖。
