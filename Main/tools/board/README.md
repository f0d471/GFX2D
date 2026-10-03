# board：上板

| 文件 | 用途 |
| --- | --- |
| `board_run.py` | 烧比特流、先开串口、复位并加载程序、回收串口输出，一条命令走完；也做调试通路检查、寄存器读写与「从 Flash 启动」的采集 |
| `input_forward.py` | SW9 的两字节按键协议、Windows 键盘与 XInput 手柄采样；由 `board_run.py --forward-input` 调用 |
| `gfx2d_debug.cfg` | 本板用的 OpenOCD 配置，修掉官方 cfg 在本板上的两处对不上，`board_run.py` 用它 |
| `bin2hex.py` | 把程序 bin 转成 Efinity Programmer 写 Flash 用的 hex 文本 |
| `make_flash_image.py` | 比特流 + 程序 + 素材包合成一个 Flash 镜像，并做回环校验（不烧录） |

## board_run.py

```
set PYTHONHOME=<EFINITY>\python311
<EFINITY>\python311\bin\python.exe tools\board\board_run.py --bin <程序.bin> --stop-on done
```

| 用法 | 做什么 |
| --- | --- |
| `--bin <程序.bin>` | 烧比特流，开串口，`reset halt`、`load_image`、`resume`，收串口到 `--stop-on` 的文本或 `--seconds` 超时 |
| `--flash-boot` | **从 Flash 启动**：先开串口，再烧比特流；SoC 复位后 bootloader 从 `0x380000` 把程序拷进 DDR 自己跑，本模式不 `load_image`。程序与素材包都在 Flash 里时用它（SW5、SW9 的板上判据） |
| `--check-only` | 调试通路检查：连开两次 OpenOCD 会话各读一次 PC（`doc/开发环境.md` 0.6 节第 5 步） |
| `--cmd "<OpenOCD 命令>"` | 不加载程序，复位后执行命令，可给多个。回归表的 APB 探针：`--cmd "mdw 0xf8100000 3"`；按键：`--cmd "mdw 0xf8016000"` |
| `--list-ports` | 列串口 |
| `--forward-input` | 在同一串口会话转发启动时前台窗口的键盘与第一个 XInput 手柄；状态变化即发包，每 100 ms 心跳，退出前发全松 |
| `--input-script "100:128,101:0"` | 按相对加载完成时间发送诊断按键位图；用于检验慢帧里的短按、切模式与切后端 |
| `--input-script-after "csvh,"` | 脚本从本次程序输出指定文本后开始计时；游戏加载素材较慢时，用表头作起点，避免把多个按键挤在第一帧 |

`--flash-boot` 与 `--bin` 的区别只有一处：`--bin` 走 JTAG 把程序写进 DDR，
`--flash-boot` 让板子自己从 Flash 拷。**判 Flash 启动路径必须用后者**——
`--bin` 会把要验的那一段（bootloader 从 Flash 拷贝）绕过去。
它不用 OpenOCD：配置完 FPGA 的那一刻 SoC 就复位并开始打印，所以串口必须先开。

SW9 的转发位图与 `bsp/include/board.h` 一致：方向键或 WASD 为方向，空格为炸弹，
左 Shift 为慢速，Tab 为软硬件切换，数字键 1～3 为三种模式；手柄方向键/左摇杆、A、左肩键、Start
对应方向、炸弹、慢速、切换。实体 KEY0～KEY3（丝印 SW0～SW3）依次为左、下、上、右。
每个串口包是两个字节：首字节 `0x80 | k[6:0]`；次字节高位为 0，`[6:3]` 为
`(k ^ (k >> 4) ^ (k >> 8)) & 0xf`，`[2:0]` 为 `k[9:7]`。板上每帧读空接收 FIFO，
保留同帧按下沿，300 ms 无有效包即释放转发键；实体键不受此超时影响。

```
set PYTHONHOME=<EFINITY>\python311
<EFINITY>\python311\bin\python.exe tools\board\board_run.py --flash-boot ^
    --stop-on done --seconds 90 --log Main\sw\apps\test\assetTest\logs\board_normal.log
```

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

## make_flash_image.py

```
python3 Main/tools/board/make_flash_image.py --app <程序.bin> [--out <镜像.hex>] [--full]
```

按设计文档 2.2 节把三段合成一个镜像：比特流 `0x000000`（2 MiB）、程序 `0x380000`（512 KiB）、
素材包 `0x400000`（4 MiB）；`0x200000`–`0x37FFFF` 是预留区，填 `0xFF`。

- 比特流输入是 Efinity 出来的 **`.hex`（active），不是 `.bit`（passive）**。
  同一编译的两份输出解码后差 20 字节，差异全在 ASCII 头里（`.bit` 写 `Mode: passive`），
  写 Flash 用的是 active 那一份；工具会检查头部的 `Mode: active` 并拒绝 passive 输入。
- 输出范围按「分区起点 + 输入实际字节数」算并向上取整到 4 KiB 扇区，
  **不按最后一个非 `0xFF` 字节截断**——有效输入本身可以以 `0xFF` 结尾。`--full` 出整片 8 MiB。
- 四个分区起点都断言扇区对齐；容量逐项检查；合成后解回字节与三份输入逐字节比，
  区外必须是 `0xFF`。素材超过 4 MiB 分区或超过 `--max-asset` 的运行预算时报错退出。
- **本工具不烧录。** 写 Flash 用 `ftdi_pgm.bat`（`doc/开发环境.md` 第 8 节），写之前先整片读出备份：

```
%PGM% -m jtag -u <JTAG> <EFINITY>\pgm\fli\titanium\u10660A79.bit
%PGM% -m jtag_bridge --jtag_bridge_mode all --verify_method hostx1 --address 0x0 ^
      -u <JTAG> <镜像.hex>
```

实测（2026-09-23，6.047 MiB 镜像）：擦除 32 秒、写入 34 秒、读回校验 23 秒，末尾 `Flash verify successful`。
写完之后 FPGA 停在桥接镜像上；再按上面的 `--flash-boot` 烧一次自己的比特流，
SoC 复位后就会从 Flash 启动。
