# board：上板

| 文件 | 用途 |
| --- | --- |
| `board_run.py` | 烧比特流、先开串口、复位并加载程序、回收串口输出，一条命令走完；也做调试通路检查与寄存器读写 |
| `gfx2d_debug.cfg` | 本板用的 OpenOCD 配置，修掉官方 cfg 在本板上的两处对不上，`board_run.py` 用它 |
| `bin2hex.py` | 把程序 bin 转成 Efinity Programmer 写 Flash 用的 hex 文本 |

## board_run.py

```
set PYTHONHOME=<EFINITY>\python311
<EFINITY>\python311\bin\python.exe tools\board\board_run.py --bin <程序.bin> --stop-on done
```

| 用法 | 做什么 |
| --- | --- |
| `--bin <程序.bin>` | 烧比特流，开串口，`reset halt`、`load_image`、`resume`，收串口到 `--stop-on` 的文本或 `--seconds` 超时 |
| `--check-only` | 调试通路检查：连开两次 OpenOCD 会话各读一次 PC（`doc/开发环境.md` 0.6 节第 5 步） |
| `--cmd "<OpenOCD 命令>"` | 不加载程序，复位后执行命令，可给多个。回归表的 APB 探针：`--cmd "mdw 0xf8100000 3"`；按键：`--cmd "mdw 0xf8016000"` |
| `--list-ports` | 列串口 |

| 参数 | 说明 |
| --- | --- |
| `--no-program` | 跳过烧比特流。默认烧 `Main/fpga/gfx2d_ti60/outflow/gfx2d_ti60.bit`，`--bit` 换别的 |
| `--port` | 串口号，默认 `COM11`（`doc/开发环境.md` 第 11 节的参考值） |
| `--stop-on` | 收到该文本就结束采集；没收到时退出码为 1。**只从 `load_image` + `resume` 之后开始匹配**，取程序末尾的标记（见下面第 4 条坑） |
| `--log` | 串口日志存哪，默认 `%TEMP%\board_run.log` |
| `--addr` | 加载地址，默认 `0x1000` |
| `--dry-run` | 只打印将执行的步骤 |

不用从特定目录运行，也不用 `--repo`：仓库位置由脚本自己的位置推出。
Efinity 目录依次取 `--efinity`、`EFINITY_HOME`、PATH 里的 `efx_run.bat`，最后从解释器位置推
（用 Efinity 自带的 python 跑时总能推出来）；`openocd.exe` 依次取 `--openocd`、`EFINITY_IDE_HOME`、`--ide`、
与 Efinity 同级的 `efinity-riscv-ide-*`。

**退出码。** 0 表示每一步都有正面证据：烧写输出里有 `finished with JTAG programming`；
OpenOCD 考察到目标，`load_image` 报的写入字节数等于 bin 的大小；串口收到了数据，给了 `--stop-on` 时还收到了该文本。
缺任何一项都是 1，参数或环境问题是 2。退出码**不判断程序自己的 `fail N`**，那一行要看输出。

它替你绕开的坑，都是实测踩过的：

1. **先开串口再加载。** 串口没打开时板子发出的数据被直接丢弃，驱动不缓存。
2. **加载前 `reset halt`。** 不复位时前一次实验可能把总线留在坏状态，`load_image` 照样报成功，程序一行都不跑。
3. **烧写不能只看退出码。** `ftdi_pgm.bat` 只在 `EFINITY_HOME` 未定义时才自己调 `setup.bat`；
   父进程设过它时，内部 python 以 `Fatal Python error` 退出而退出码仍是 0，接着就用 FPGA 里原有的比特流跑了程序。
   所以烧写前把 Efinity 的环境变量从子进程里去掉，并要求输出里有成功标记。
4. **串口号错了也不报错。** 打开一个存在但不对的 COM 口，读到 0 字节；这种情况按失败处理。
5. **Flash 里上一次固化的程序会混进这一次的输出。** 第 1 条要求串口先于加载打开，
   而烧完比特流板子就从 Flash 自启动，那份程序照样打 `done`、`fail 0`。
   直接拿它匹配 `--stop-on`，会在要跑的程序一行都没输出的时候报成功、退出码 0
   （2026-09-21 实测：Flash 里是 `bootSizeTest`，`--stop-on done` 当场命中）。
   所以匹配窗口从 `load_image` + `resume` 之后才开始，日志里插一行
   `# --- 程序已加载，以下才是它的输出 ---` 作分界。
   代价：resume 到 OpenOCD 退出之间（不到一秒）的输出也在窗口外，
   `--stop-on` 要取程序末尾的标记，不要取开头的横幅。

## gfx2d_debug.cfg

`sdk/` 下的官方 `debug.cfg`、`debug_ti.cfg` 是 IP 生成物，在本板上有两处对不上，本文件修掉它们而不动官方文件：

1. `debug.cfg` 的 `-irlen 4` 与本板不符，本板 IR 宽度是 5（`ftdi_pgm.bat --scan_usb` 报 `ir_width 5`），
   用 4 报 `IR capture error at bit 4, saw 0x25 not 0x..3`。
2. 官方 cfg 往上最多找 3 级目录里的 `cpu0.yaml`，程序在 `sw/apps/<类>/<程序>/` 下时找不到。
   本文件走官方 cfg 里的 `LEGACY_IDE` 分支，由 `workspace_loc` 直接给出 `Main/sdk`。

`workspace_loc` 用 OpenOCD 的 `-c "set ::env(workspace_loc) {...}"` 在 Tcl 里设，而不是设环境变量：
从 WSL 调 `openocd.exe` 时自定义环境变量传不进 Windows 进程，`-c` 两边都成立。手工调用的写法见文件头部。

## bin2hex.py

```
python3 tools/board/bin2hex.py <输入.bin> <输出.hex>
```

Efinity 的 `jtag_bridge` 写 Flash 只接受 hex 文本（每字节一行、两位大写十六进制、LF 换行），不接受裸 bin。
转换后把 hex 解码回字节与输入逐字节比较，不一致时退出码为 1。写 Flash 的命令见 `doc/开发环境.md` 第 8 节。
