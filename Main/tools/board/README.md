# board：上板

原生PC模式4默认按窗口/当前显示器比例渲染宽屏。打包器保留`Run-PC.ps1 -FixedAspect`
供640×480对照；`-Fullscreen`为桌面全屏。旧冻结包不因工具更新而原地改写。

| 文件 | 用途 |
| --- | --- |
| `board_run.py` | 烧比特流、先开串口、复位并加载程序、回收串口输出，一条命令走完；也做调试通路检查、寄存器读写与「从 Flash 启动」的采集 |
| `input_forward.py` | 板上游戏的两字节按键协议、Windows 键盘与 XInput 手柄采样；由 `board_run.py --forward-input` 调用 |
| `game_control.py` | 正在运行的游戏的电脑控制面板；固定操作说明、实时模式/后端/FPS、切换提示、硬件后端恢复和重开按钮 |
| `play.ps1` | 游戏控制面板的 PowerShell 启动入口；定位 Efinity Python，日志与状态写入项目内的构建目录 |
| `build_pc.py` | WSL MinGW+固定SDL2版本构建原生Windows游戏或窗口集成检查；构建依赖缓存于忽略目录 |
| `package_game.py` | 冻结PC EXE/DLL、素材、板端bit/BIN、独立部署工具及源码快照到`bit/board_release_*/` |
| `release_launch.py` | 冻结包校验、自动定位当前UART、JTAG重载和连接包内面板；不写Flash |
| `gfx2d_debug.cfg` | 本板用的 OpenOCD 配置，修掉官方 cfg 在本板上的两处对不上，`board_run.py` 用它 |
| `bin2hex.py` | 把程序 bin 转成 Efinity Programmer 写 Flash 用的 hex 文本 |
| `make_flash_image.py` | 比特流 + 程序 + 素材包合成一个 Flash 镜像，并做回环校验（不烧录） |

## board_run.py

验证范围以 `Main/doc/验证流程.md` 为准。代表性部署与引擎功能用
`tools/check/validate.py --checks board --profile quick --run`；该入口自动定位当前 UART。
串口号和 Flash 内容必须按当前设备确认，历史登记值只作参考。

```
set PYTHONHOME=<EFINITY>\python311
<EFINITY>\python311\bin\python.exe tools\board\board_run.py --bin <程序.bin> --stop-on done
```

| 用法 | 做什么 |
| --- | --- |
| `--bin <程序.bin>` | 烧比特流，开串口，`reset halt`、`load_image`、`resume`，收串口到 `--stop-on` 的文本或 `--seconds` 超时 |
| `--flash-boot` | **从 Flash 启动**：先开串口，再烧比特流；SoC 复位后 bootloader 从 `0x380000` 把程序拷进 DDR 自己跑，本模式不 `load_image`。程序与素材包都在 Flash 里时用它（素材装载与游戏移植的板上判据） |
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

转发位图与 `bsp/include/board.h` 一致：方向键或 WASD 为方向，空格为炸弹，
左 Shift 为慢速，Tab 为软硬件切换，数字键 1～4 为四种模式；手柄方向键/左摇杆、A、左肩键、Start
对应方向、炸弹、慢速、切换。实体 KEY0～KEY3（板上丝印的拨码开关）依次为左、下、上、右。
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
| `--log` | 串口日志存哪，默认 `%TEMP%\board_run.log`；采集前自动创建父目录，创建失败在烧写前返回环境错误 |
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
6. **采集中途 USB 掉线。** FT4232H 被系统意外移除时串口句柄失效，读串口抛 `SerialException`。
   日志末尾记一行 `# serial lost`，退出码 1，报「采集中途串口失效」，
   不再报成「N 秒内没有收到」（2026-10-03 实测：9208 帧回放跑到第 8412 帧时掉线）。

## game_control.py

游戏程序按 SW → HW/DDR → HW/TEX → SW 循环。控制面板显示三个后端，
请求跨两步切换时等待每次板上反馈后再发下一步；旧程序没有 TEX 时停止继续请求。
CSV 弹数按 n_bullet 列解析。运行 `tools/check/test_game_control.py` 可离线检查，
GUI 不承担板上功能判分。

在仓库根目录启动面板：

```powershell
& ./Main/tools/board/play.ps1
```

启动器自动查找 Efinity 安装目录，也可通过 `EFINITY_HOME` 或 `-EfinityHome <EFINITY>` 指定。
`play.ps1` 使用 UTF-8 BOM 编码，兼容 Windows PowerShell 5.1 和 PowerShell 7；编辑时保留 BOM。
`-Port COM11` 指定串口；`-CheckOnly` 只检查解释器、依赖和命令行参数，不连接板子。
串口日志和 `panel_status.json` 写入 `Main/sw/apps/game/gfx2dGame/build_control/`，由现有
`build_*/` 规则忽略。关闭面板后恢复启动前的 Python 环境变量。

直接调用 Python：

```powershell
$env:PYTHONHOME = '<EFINITY>\python311'
& '<EFINITY>\python311\bin\python.exe' Main/tools/board/game_control.py --ensure-hw
```

面板连接已运行的游戏，读取串口状态并复用 `input_forward.py` 转发按键。当前模式、绘图后端与 FPS
以板上日志为准；切到 SW 时后端栏和提示条变红。`--ensure-hw` 在每次连接后根据实际反馈恢复 HW，
随后仍可主动切到 SW 作对比。“回到 HW”按钮只在当前为 SW 时发出一次切换；重开按钮先进入
模式 1，收到板上反馈后再进入模式 2。可用 `--port`、`--baud` 指定串口；`--log` 追加原始串口日志，
`--status-file` 写出当前状态 JSON。串口被占用或 USB 断开时，面板显示原因并提供重新连接按钮。

| 操作 | 效果 |
| --- | --- |
| WASD / 方向键 | 移动；可斜向移动 |
| 左 Shift | 按住慢速移动、显示判定点 |
| 空格 | 每次按下使用一颗炸弹；射击自动进行 |
| 1 / 2 / 3 / 4 | 图形演示 / 弹幕游戏 / 压力测试 / 三维走廊；三维内再按4切茶杯 |
| Tab / 手柄 Start | 切换 HW、SW；SW 使用板上 CPU 绘图，帧率较低 |
| 上+下保持 8 帧 | 下一个模式；W+S 同样触发 |
| 左+右保持 8 帧 | 切换后端；A+D 同样触发，在 60 FPS 下约 0.13 秒就会触发 |
| 组合键再次触发 | 一对键都松开后才能再次触发 |
| Esc / 转发复选框 | 停止或恢复键盘与手柄转发；板上游戏继续运行 |
| 重开本局 | 先切 1、等待确认、再切 2；也可按相同顺序手动重开 |
| 手柄方向键/左摇杆、A、LB | 移动、炸弹、慢速；支持第一个 XInput 手柄 |
| 实体拨码开关 KEY0～KEY3 | 左 / 下 / 上 / 右；不受电脑转发开关影响 |

键盘仅在面板位于前台时转发；手柄不受前台限制，关闭转发开关后两者都停止。关闭面板前发送
全松包，游戏继续运行。模式 2 连续 600 帧无输入后自动驾驶，任意游戏按键接管回来。
标题显示 120 帧后自动开始；初始 3 残机、3 炸弹。有炸弹时受击自动消耗一颗，没有炸弹时扣残机并
补回 3 炸弹；残机用完回标题，通关后自动进入新一局。

## gfx2d_debug.cfg

三维模式使用数字4（现有10位包的三个模式位组合），不扩展串口包。模式4内再按4切换
走廊/茶杯，Tab切SW/HW、Shift切L1/L2；控制面板有对应按钮。软件路径较慢，条带间继续接收
系统键，操作在安全帧边界生效。`R4STAT`反馈实际呈现FPS、渲染FPS、记录数和阶梯/后端/场景。
JTAG部署与Flash固化仍是两个操作，检阅程序不自动改写Flash。
三维走廊无输入时自动驾驶，方向键接管；自动射击，Space 使用炸弹清弹。
面板显示实际场景、L1/L2、三角形数与呈现帧率，反馈来自板上统计。

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

## 条带后端板测矩阵

run_g4_matrix.py 在 Windows/Efinity Python 下串行调用 board_run.py：
16k-follow 为候选同帧/S1/条带诊断/地址回绕；16k-cache 为候选缓存回归与
旧探针不可用诊断；4k-perf 恢复主线后测同位流 TEX、S1、游戏和缓存注错；
replay 完成四后端切换、缓存正常/失效注错/反例并跑完整 9208 帧条带回放。

先用 check/check_g4_integration.py --build-target 构建对应程序，并用
--register 登记即将部署的对象；16KB 构型由 soc/make_cache_variant.py 生成。
stripTest 与 ddrAliasTest 使用各自 makefile 构建。矩阵在各阶段首项显式
加载所对应 bit，后续应用复用当前配置。原始串口与控制日志在
Main/sim/engine/g4/logs/；失败不继续下一项，不以进程退出码代替 fail/done。

控制面板支持 SW、HW/DDR、HW/TEX、HW/STRIP 四个反馈名；每次切换等待
板上确认，最多四次请求，旧固件不支持目标后端时停止。输入脚本等待
mode 2 enter 后再计时，不能在启动素材装载期间把多次按键合并成一次。


采集完整测试结束时可用 --stop-line done，只匹配已收到换行的完整一行，
不会在 nmax done 提前退出。--stop-on 保留子串含义，两者互斥。分块读取判据
在 sim/engine/frontend/test_board_stop_line.py，覆盖 done 行跨两个读取块的情况。
