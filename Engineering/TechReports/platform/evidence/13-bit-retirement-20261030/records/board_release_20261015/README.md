# board_release_20261015：上板复现包

一次整机编译的比特流加配套程序，用来在没有工程源码的机器上把游戏跑起来。
本目录自带 OpenOCD 配置与 CPU 描述，除 Efinity 与配套 IDE 外不依赖仓库。

## 本次实测结果（2026-10-15，上板已通过）

| 判据 | 实测值 |
| --- | --- |
| 素材包（`pak` 行） | 2840320 字节，CRC32 `b370ba0a` |
| 预合成场景（`scene 0/1/2 scrc`） | `d90fb7b2` / `70fd3e3d` / `0211c146` |
| 绘制后端 | `hw`，即硬件 DDR 流水（`GFX2D_START_BACKEND=1`） |
| 帧率 / 掉帧 | 60 fps / `drop 0` |
| 扫描 FIFO 水位 | `fifo_min 336`、`under 0`、`zero 0` |
| 引擎故障 | `faults 0`、`swap_retry 0` |
| 判据行 | `fail 0`、`done` |

原始串口日志见 `board_game_hw_ddr_20261015.log`，判据行在文件末尾两行。

## 依赖

| 项 | 说明 |
| --- | --- |
| Efinity 2026.1 | 烧写工具 `ftdi_pgm.bat` 来自它。本包由 2026.1.132 编译 |
| Efinity RISC-V IDE 2026.1 | OpenOCD 在它的 `openocd/bin` 下 |
| libusbK 驱动 | 必须装在板载 FT4232H 的 **Interface 1**（JTAG 通道）。没装时 `--scan_usb` 报 `No USB target detected`，串口会看到四个 COM 口而不是三个 |
| 板上 Flash 里的素材包 | 程序从 Flash 的 `0x400000` 读素材。素材被改写后 `pak` 行的 CRC 会对不上，此时要重新写素材分区 |

## 复现

### 方式一：用仓库里的 board_run.py（一条命令，把串口时序都处理好了）

```
set PYTHONHOME=<EFINITY>\python311
<EFINITY>\python311\bin\python.exe <REPO>\Main\tools\board\board_run.py ^
    --bit <本目录>\gfx2d_ti60.bit ^
    --bin <本目录>\gfx2dGame-O2-perf.bin ^
    --stop-on done --log <本目录>\board_run_now.log
```

退出码 0 表示烧写、加载与串口采集每步都有正面证据；程序的判据看日志里的 `fail` 行。

### 方式二：不依赖工程（两条裸命令）

顺序不能改：**串口必须先开**，否则板子从 Flash 自启动时打出的数据会被直接丢弃。

```
:: 1) 烧比特流（约 3 秒）。最后的 /1 是 FT4232H 的接口 1，板上无 SPI 通道，Flash 只能走 JTAG Bridge
<EFINITY>\pgm\bin\ftdi_pgm.bat -m jtag -u ftdi://0x0403:0x6011:0:ff/1 <本目录>\gfx2d_ti60.bit

:: 2) 打开串口：115200 8N1，接的是 FT4232H 的接口 C（串口号随插拔会变，用 --scan_usb 或设备管理器确认）

:: 3) 复位、加载程序、运行（约 10 秒；load_image 的路径用正斜杠）
<IDE>\openocd\bin\openocd.exe ^
    -c "set ::env(workspace_loc) {<本目录>}" ^
    -f <本目录>\gfx2d_debug.cfg ^
    -c "reset halt" ^
    -c "load_image {<本目录>/gfx2dGame-O2-perf.bin} 0x1000 bin" ^
    -c "resume 0x1000" ^
    -c "shutdown"
```

第 3 步的 `reset halt` 不能省：不复位时上一次实验可能把总线留在坏状态，`load_image` 照样报成功而程序一行都不跑。
`workspace_loc` 指向本目录是为了让 OpenOCD 找到随包的 `cpu0.yaml`。

看到下面两行即复现成功：

```
fail 0
done
```

## 文件清单与校验

| 文件 | 用途 |
| --- | --- |
| `gfx2d_ti60.bit` | JTAG 加载用（passive） |
| `gfx2d_ti60.hex` | 写 Flash 用（active）。同一编译的两种输出只差 ASCII 头里的 `Mode` |
| `gfx2dGame-O2-perf.bin` | OpenOCD 加载进 DDR 的程序，加载地址 `0x1000` |
| `gfx2dGame-O2-perf.elf` / `.map` | 符号与链接映射，排错用 |
| `gfx2d_debug.cfg` | 本板用的 OpenOCD 配置（IR 宽度取 5，`workspace_loc` 分支） |
| `cpu0.yaml` | OpenOCD 需要的 CPU 描述，`gfx2d_debug.cfg` 从 `workspace_loc` 取它 |
| `board_game_hw_ddr_20261015.log` | 本次上板的原始串口日志 |
| `manifest.json` | 各文件的字节数与 SHA256、来源路径、复制校验结果 |

各文件摘要以 `manifest.json` 为唯一清单，本文件不重复抄录。

## 构建来源

| 项 | 值 |
| --- | --- |
| 比特流 | 仓库提交 `6c2d59c` 的干净工作区，`fpga/gfx2d_ti60` 命令行四步编译（map/interface/pnr/pgm 全 PASS） |
| 资源与时序 | 60064 XLR / 172 RAM / 23 DSP；`core_clk` setup +0.104 ns、hold +0.022 ns，所有时钟非负 |
| 程序 | `gfx2dGame`，`make PERF=1 EXTRA_CFLAGS='-DGFX2D_START_MODE=2 -DGFX2D_START_BACKEND=1 -DGFX2D_FRAMES=240'` |
| 程序行为 | 上电进模式 2（弹幕游戏），硬件 DDR 后端，跑满 240 帧后打印判据并结束 |
