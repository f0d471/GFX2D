# Main：赛题二开发主目录

基线是官方 Ti60F225 开发板资料包里的 Sapphire RISC-V SoC + DDR3 参考工程：

```
Efinix/02_Ti60F225_DemoBoard_v4_赛题1-3/08_ti60f225_soc_demo/09_Ti60F225_hardjtag_demo
```

设计文件和软件原样搬入，只调整了目录布局，并改正了几处指向官方机器的绝对路径。在此基础上接入了 HDMI 发送通路，输出 640×480@60 八色竖条；并把应用程序镜像的上限从 124KB 扩大到 512KB。DDR3 帧缓存和 BitBlt 引擎还没有加入。

## 目录结构

| 目录 | 内容 | 来源 |
| --- | --- | --- |
| `fpga/ddr_demo_ti60/` | Efinity 工程：工程文件 `ddr_demo_ti60.xml`、接口设计 `ddr_demo_ti60.peri.xml`、SoC IP（`ip/soc/`）、时序约束（`sdc/ddr3.sdc`）、APB 从设备与 AXI 多路器源码（`src/`）、Debugger 配置（`debug_*`） | 官方 `par/ddr_demo_ti60/` |
| `rtl/` | 顶层 `ddr3_example_top.v`、DDR3 软核控制器（`ddr3_controller/`）、DDR 读写测试逻辑（`memory_test/`） | 官方 `rtl/` |
| `rtl/hdmi/` | HDMI 发送通路：本目录编写的封装 `hdmi_tx.v`，以及取自官方 `03_hdmi_tx_demo` 的 `color_bar_rgb.v`、`dvi_encoder.v`、`encode.v` | 新增 |
| `sim/hdmi/` | `hdmi_tx` 的仿真测试台与运行脚本 `run.sh` | 新增 |
| `sdk/` | RISC-V 软件：`bsp/efinix/EfxSapphireSoc/`（头文件、链接脚本、OpenOCD 配置）、`software/standalone/` 与 `software/freeRTOS/` 下的示例、`tool/binGen.py` | 官方 `par/ddr_demo_ti60/embedded_sw/soc/` |
| `bit/official/` | 官方工程目录中预编译的比特流 `ddr_demo_ti60.bit` 与 `ddr_demo_ti60.hex`，不含 HDMI | 官方 `par/ddr_demo_ti60/outflow/` |
| `doc/` | `赛题二总体设计.md`：资源裁剪、DDR 与片上 RAM 划分、加速器架构、游戏与计算统计、待测实验和里程碑 | 新增 |

`fpga/ddr_demo_ti60/` 与 `rtl/` 的相对位置不能改动。工程文件以 `../../rtl/...` 引用源码，工程目录必须位于 `Main/` 下两级。

## 工程配置

以下数值取自 `ddr_demo_ti60.xml`、`ddr_demo_ti60.peri.xml`、`ip/soc/settings.json`、`ip/soc/source/soc_config` 与 `sdk/bsp/efinix/EfxSapphireSoc/include/soc.h`。

- 器件 Ti60F225，时序模型 I3；工程保存版本 Efinity 2025.2.288.3.8；顶层模块 `ddr3_example_top`。
- SoC：Sapphire（efx_soc 3.3.0），单核，系统频率 100MHz，指令与数据 Cache 各 4KB，FPU 关闭（`soc_config` 中 `--Fpu false`），自定义指令关闭。
- 顶层例化四个模块：
  - `ddr3_top`：DDR3 控制器；
  - `soc`：Sapphire SoC，DDR 口 `io_ddrA` 接到 `ddr3_top` 的 AXI 从口；
  - `apb3_top`：挂在 SoC 的 APB Slave 0 上，定义在 `fpga/ddr_demo_ti60/src/Interrupt.v`；
  - `hdmi_tx`：HDMI 发送通路，与 SoC、DDR3 之间没有数据交互。
- `src/Axi_Mux.v` 与 `rtl/memory_test/` 在工程的源文件列表里，但顶层没有例化。
- SoC IP 已经生成、但顶层没有连接的端口：
  - `axiA_*`：AXI 从设备窗口；
  - `io_ddrMasters_0_*`：外部 AXI 主机访问 DDR 的入口，数据宽度 32 位；
  - `userInterruptA`：用户中断输入；
  - `system_i2c_0_io_*`：I2C0。

PLL：

| 实例 | 资源 | 参考时钟 | 输出 |
| --- | --- | --- | --- |
| `pll_inst1` | PLL_TL0 | `clk_25m` | `core_clk` 100MHz |
| `DDR3_PLL` | PLL_TR0 | `clk_25m` | `sdram_clk`、`tx_cal_clk`、`rx_cal_clk`、`tx_cal_clk_90edge` 各 400MHz，`clk_100m` 100MHz |
| `HDMI_PLL` | PLL_BL0 | `clk_27m` | `hdmi_tx_slow_clk` 25.2MHz，`hdmi_tx_fast_clk` 126MHz（相移 90°） |

地址映射（`soc.h`）：

| 区域 | 基址 | 大小 |
| --- | --- | --- |
| DDR3（`SYSTEM_DDR_BMB`） | `0x0000_1000` | `0xE000_0000` 地址窗口 |
| AXI 从设备窗口（`SYSTEM_AXI_A_BMB`） | `0xE100_0000` | 16MB |
| UART0 | `0xF801_0000` | `0x40` |
| SPI0 | `0xF801_4000` | `0x1000` |
| GPIO0（4 位） | `0xF801_5000` | `0x100` |
| I2C0 | `0xF801_6000` | `0x100` |
| APB Slave 0（`IO_APB_SLAVE_0_INPUT`） | `0xF810_0000` | 64KB |
| CLINT | `0xF8B0_0000` | `0x1_0000` |
| PLIC | `0xF8C0_0000` | `0x40_0000` |
| 片上 RAM（bootloader） | `0xF900_0000` | 4KB |

板上 DDR3 为 MT41J128M16JT-125，容量 2Gb（256MB），实际可用范围远小于地址窗口。

PLIC 中断号：UART0 为 1，SPI0 为 4，I2C0 为 8，用户中断 A 为 16，AXI 从设备窗口为 30。

应用程序链接到 `0x0000_1000`，程序区长度 4092K，Flash 中的程序镜像上限 512KB（见「Bootloader 与程序区」）。文件名带 `_i` 的链接脚本把程序放在片上 RAM，没有改动。

## HDMI 发送通路

### 结构

```
clk_27m ──> HDMI_PLL ──> hdmi_tx_slow_clk (25.2MHz) ──> hdmi_tx ──> 4 × 10 位并行字 ──> LVDS TX ×4 ──> HDMI 口
                     └─> hdmi_tx_fast_clk (126MHz, 90°) ────────────────────────────────┘
```

`hdmi_tx.v` 内部依次为：

1. 复位同步：PLL 失锁时复位，锁定后在像素时钟下释放；
2. `color_bar_rgb`：产生行场时序和八色竖条（白、黄、青、绿、品红、红、蓝、黑）；
3. `dvi_encoder`：三个颜色通道分别做 TMDS 8b/10b 编码，时钟通道固定为 `1111100000`；
4. 输出取反：板上 HDMI TX 差分对的 P/N 与 FPGA 管脚对调，取反后还原。

串行化由 Interface Designer 中的 4 路 LVDS TX 完成，设置为半速率、串行化宽度 10、VOD large、预加重 medium low，与官方 HDMI 工程相同。输出是 DVI 信号，没有 HDMI 数据岛（不含音频和 InfoFrame），HDMI 显示器可以直接接收。

### 时序与管脚

640×480@60（VESA DMT）：

| 方向 | 有效 | 前沿 | 同步 | 后沿 | 总计 |
| --- | --- | --- | --- | --- | --- |
| 行（像素） | 640 | 16 | 96 | 48 | 800 |
| 场（行） | 480 | 10 | 2 | 33 | 525 |

同步脉冲低有效。像素时钟 25.2MHz，帧率 25.2MHz / (800 × 525) = 60.00Hz。时序参数在 `hdmi_tx` 的 parameter 中；改分辨率时，`HDMI_PLL` 的输出分频和 `sdc/ddr3.sdc` 中的时钟周期要一起改。

| LVDS TX 实例 | 资源 | 板上网络（P / N） |
| --- | --- | --- |
| `tmds_tx_clk` | GPIOR_PN_10 | TX_TMDS_CLK_P = GPIOR_N_10（H12），TX_TMDS_CLK_N = GPIOR_P_10（H13） |
| `tmds_tx_data0` | GPIOR_PN_13 | TX_TMDS_DATA_P0 = GPIOR_N_13（F15），N0 = GPIOR_P_13（G15） |
| `tmds_tx_data1` | GPIOR_PN_11 | TX_TMDS_DATA_P1 = GPIOR_N_11（G14），N1 = GPIOR_P_11（H14） |
| `tmds_tx_data2` | GPIOR_PN_12 | TX_TMDS_DATA_P2 = GPIOR_N_12（F13），N2 = GPIOR_P_12（G13） |

管脚取自《TI60F225I3-V4 DEMO板软硬件设计说明》第 22~23 页。27MHz 晶振接 GPIOL_P_10，与官方 `10_Ti60f225_sc431hai2hdmi_v6` 工程一致。

### PLL 参数

Ti60 PLL 在本地或内核反馈模式下（`钛金Ti60datasheet-v2_0.pdf`）：

- FPFD = FIN / N，要求 16~800MHz；
- FVCO = FPFD × M × O × C<sub>FBK</sub>，要求 2200~5500MHz；
- FPLL = FVCO / O，不超过 4000MHz；
- FOUT = FIN × M × C<sub>FBK</sub> / (N × C)；
- CLKOUT0 的频率不能超过参考时钟的 4 倍。

另外有两条要求：

- 相移：静态相移的单位是 0.5 个 FPLL 周期，最大 3.5 个周期。这个单位在官方工程里逐条核对过，例如 DDR 工程中 `C=6, phase_setting=3` 的写时钟就是 90°。
- 快慢时钟：LVDS TX 半速率、宽度 10 时，快时钟频率是慢时钟的 5 倍，并相移 90°。

板上 25MHz 晶振给不出 25.2MHz。FPFD 不低于 16MHz 要求 N=1，于是 M × C<sub>FBK</sub> = 5.04 × C<sub>fast</sub>，这个值要取整数，C<sub>fast</sub> 就必须是 25 的倍数。但 90° 相移要求 C<sub>fast</sub> 为偶数且不超过 14，两个条件无法同时满足。

改用 27MHz 参考后取值如下：

| 参数 | 取值 | 结果 |
| --- | --- | --- |
| N / M / O | 1 / 1 / 4 | FPFD = 27MHz，FVCO = 3024MHz，FPLL = 756MHz |
| CLKOUT0 `hdmi_pll_fb` | C = 28，本地反馈 | 27MHz（不超过 4 × 27MHz） |
| CLKOUT1 `hdmi_tx_slow_clk` | C = 30 | 25.2MHz |
| CLKOUT3 `hdmi_tx_fast_clk` | C = 6，phase_setting = 3 | 126MHz，相移 1.5 / 6 周期 = 90° |

选用 PLL_BL0 与 CLKOUT1/CLKOUT3 的做法参照官方 v6 工程：该工程在同一块板上用 PLL_BL0 + 27MHz 给 HDMI TX 供时钟，并与 PLL_TR0 上的 DDR3 共存。v6 工程还把 3A bank 设为 1.8V。

### 验证

仿真（需要 iverilog，在 WSL 中运行）：

```
bash sim/hdmi/run.sh
```

测试台把 4 路 10 位 TMDS 字按 DVI 1.0 规则解码回同步信号与像素。从复位后的第二个场同步起点开始，连续两帧逐像素检查以下内容：

- 时钟通道；
- 三个通道的控制期与数据期是否一致；
- 行周期、行同步宽度、有效区起点与宽度；
- 场周期、场同步行数、有效区起始行与行数；
- 每个有效像素的颜色。

脚本运行三个用例：

| 用例 | 期望 | 结果 |
| --- | --- | --- |
| 正常配置 | PASS | PASS，2 帧，614400 个像素 |
| 行后沿少 1 个像素（`INJECT_HBP`） | FAIL | FAIL |
| 不撤销 P/N 取反（`INJECT_NOINV`） | FAIL | FAIL |

复位释放后的第一帧首行不完整，原因有两个：

- 编码器复位期间输出 0，这不是合法的 TMDS 字；
- 官方 `color_bar_rgb` 的 hs/vs 寄存器初值为 0，对低有效同步来说等于有效。

这只影响上电或 PLL 重新锁定后的第一行，所以测试台跳过第一帧。

顶层与接口设计的检查：

- yosys 解析修改前后的顶层并比较端口：只新增 14 个 HDMI 端口，原有端口不变，`hdmi_tx` 实例的端口连接可以解析。
- `peri.xml` 新增的信号名与顶层新增端口逐一对应。不进顶层的只有 `clk_27m`、`hdmi_pll_fb`、`hdmi_tx_fast_clk`，分别是 PLL 参考时钟、PLL 本地反馈、LVDS 快时钟，与官方工程的做法相同。

Efinity 2026.1 编译已通过：Interface Designer 接受了 `peri.xml`，时序没有负 slack，`hdmi_tx_fast_clk` 为 126MHz、相移 90°（`doc/板卡实测记录.md` 第 7 节）。2026-09-16 上板：显示器上出现八色竖条，颜色顺序正确。

## Bootloader 与程序区

官方工程的应用程序只能有 124KB：bootloader 上电后从 Flash `0x380000` 拷 `0x1F000` 字节到 DDR `0x1000` 再跳转，链接脚本也把程序区限定为 124K。游戏程序放不下，所以按 Sapphire 手册的做法扩大（`Efinix/07_官网补充文档/riscv-sapphire-ug-v8.2.pdf` 第 25 页 Modify the Bootloader、第 29 页 BRAM Initial Content Updater）。

| 项 | 原值 | 现值 | 文件 |
| --- | --- | --- | --- |
| bootloader 拷贝长度 `USER_SOFTWARE_SIZE` | `0x01F000`（124KB） | `0x080000`（512KB） | `sdk/software/standalone/bootloader/src/bootloaderConfig.h` |
| 程序区（链接脚本中的 `ram`） | 124K | 4092K，即 `0x1000`~`0x3FFFFF` | `sdk/bsp/efinix/EfxSapphireSoc/linker/default.ld`、`freertos.ld` |
| 片上 RAM 初始内容 | 官方预置 bootloader | 新 bootloader | `fpga/ddr_demo_ti60/ip/soc/` 与 `ip/soc/source/hardware/netlist/` 下各 4 个 `EfxSapphireSoc.v_toplevel_system_ramA_logic_ram_symbol[0-3].bin` |
| SoC IP 配置 | `LDSize=124`，未启用 Custom On-Chip RAM Application | `LDSize=4092`，启用，`HexFile_Path` 指向 `sdk/tool/bootloader/bootloader_512K.hex` | `fpga/ddr_demo_ti60/ip/soc/settings.json` |

Flash 与 DDR 的分区见 `doc/赛题二总体设计.md` 2.1、2.2 节：程序镜像在 Flash `0x380000`~`0x3FFFFF`，素材从 `0x400000` 开始。

两个长度各管一件事：

- 512KB 是 Flash 中程序镜像（代码与已初始化数据）的上限。bootloader 每次上电都拷满 512KB，单线 SPI 下按手册「124KB 约 300ms」推算约 1.2 秒（估算）。
- 4092K 是程序在 DDR 中运行的空间，`.bss` 与栈也在里面，这部分不占 Flash。
- 镜像超过 512KB 时 bootloader 会静默截断。两个链接脚本都加了断言，超出时链接直接失败并报 `application image exceeds bootloader USER_SOFTWARE_SIZE`。个别程序需要别的上限时，链接参数加 `-Wl,--defsym,__app_image_max=<字节数>`。

新 bootloader 用 xPack `riscv-none-embed-gcc 8.3.0-2.3` 编译（RISC-V IDE 2025.2 自带的也是 gcc 8.3.0，见 gpioDemo 的 `.cproject`），makefile 和编译选项没有改，二进制 1842 字节，含 256 字节栈共占片上 RAM 的 51.56%。片上 RAM 初始文件由 `sdk/tool/binGen.py` 生成（`-f 0 -s 4096`）。

检查：

| 检查 | 结果 |
| --- | --- |
| 同一工具链、同一选项，编译修改前后的 bootloader，逐字节比较 | 长度相同，只有 1 条指令不同：`lui a4,0x1f` 变为 `lui a4,0x80` |
| `binGen.py` 读入官方片上 RAM 的原始内容，重新生成 4 个初始文件 | 与官方文件逐字节相同，说明生成方法、字节序和文件格式都对 |
| 新初始文件解码回二进制 | 与 `bootloader_512K.bin` 一致，其余字节为 0，每个文件 1024 行 |
| gpioDemo 用新链接脚本编译 | 通过；bin 2602 字节，`_edata` 相对 `0x1000` 为 2604 字节，断言按 `_edata` 判断，偏保守 |
| 注错：`__app_image_max=0x100` | 链接失败，报上面那条信息 |

没有做到的：

- 官方预置的 bootloader 找到了出处：SoC 片上 RAM 的原始内容与 Efinity 2026.1 安装目录下 `ipm/ip/efx_soc/efx_soc/generator/bootloader/bootloader_124K.hex` 逐字节相同，说明 2025.2 与 2026.1 用的是同一个文件。IP 生成器在启用 DDR 且启用 SPI0 时直接选这个预编译文件（`generator/soc_gen.py` 第 784~785 行）。它与源码怎么对应仍未查清：原版源码用 gcc 8.3.0 在 `-Og`、`-Os`、`-O3` 与带不带 `-DSMP` 的 6 种组合下编出的结果都与它（806 字节）不同，`sdk/tool/bootloader/bootloader_124K.bin`（690 字节）也对不上。所以新 bootloader 与官方预置版本不是同一份二进制，能否正常引导要上板确认。
- 上板判据：`sdk/software/standalone/application/bootSizeTest/` 带一个 300KB 的数据块，镜像约 295KB，启动后经串口打印数据块整体与最后 4KB 的 CRC32，与 PC 上算出的期望值一致即通过（期望值与步骤见 `doc/板卡实测记录.md` 7.4 节）。同一程序配官方 124KB bootloader 时数据块尾部是错误数据，可作对照。片上 RAM 已确认装的是新 bootloader（同文档 7.3 节），还差把程序写进 Flash 后引导这一步。
- 回退：`bit/official/ddr_demo_ti60.bit` 里仍是官方 124KB bootloader。

Flash 读取的线数：bootloader 可选 `SINGLE_SPI`、`DUAL_SPI`、`QUAD_SPI`，现在是单线。顶层只连了 `system_spi_0_io_data_0/1`，`data_2/3` 悬空，`peri.xml` 也只分配了两根数据线，所以四线现在用不了；双线可以试（手册表 14 写 Ti60F225 开发套件连 2 根数据线），留给设计文档里的实验 E6。

以后在 IP Manager 里重新生成 SoC 时要注意：

- 确认 Cache/Memory 页的 Custom On-Chip RAM Application 已勾选，路径指向本目录的 `sdk/tool/bootloader/bootloader_512K.hex`，Application Region Size 为 4MB（即 4092）。Efinity 安装目录的 `generator/bootloader/` 下也有一个 `bootloader_512K.hex`，那是给 512KB 片上 RAM、不接 DDR 的配置用的，不要选错；
- 重新生成的软件包里，链接脚本不带上面的断言，同步到 `sdk/bsp/` 时要保留断言；
- 改了 `bootloaderConfig.h` 就要重新编译 bootloader，并同时更新 `bootloader_512K.hex` 与 8 个初始文件。

## 使用

工具（2026-09-16 已在本机安装）：

| 工具 | 版本 | 位置 |
| --- | --- | --- |
| Efinity（易灵思自 2026.1 起的一体化版本，Primus 不再单独发布） | 2026.1.132，未打补丁 | `C:\Efinity\2026.1`，启动文件 `bin\efinity.exe`；安装程序没有建快捷方式 |
| Efinity RISC-V 嵌入式软件 IDE | 2026.1.0.7，自带 `riscv-none-elf-gcc` 13.4.0、make 4.2.1、OpenOCD | `C:\Efinity\efinity-riscv-ide-2026.1`，开始菜单有快捷方式 |
| Java | Oracle JRE 8u381，已加入系统 PATH | `C:\Program Files\Java\jre-1.8`，安装包取自资料目录 `02_.../软件安装包/` |
| VC++ 2022 x64 运行库 | 14.51 | 本机原来就有 |
| 板载下载器驱动 | Interface 1 为 libusbK 3.1.0.0，其余接口保持 FTDI 驱动 | 用 `Efinix/02_.../驱动安装及说明/zadig-2.8.exe` 安装；`pgm\bin\ftdi_pgm.bat --scan_usb` 读到 JTAG ID `0x10660a79`，见 `doc/板卡实测记录.md` |

三个安装程序由一个管理员脚本依次静默执行，退出码都是 0。两个安装包的默认目录就是上表位置，与 Sapphire 手册第 11 页一致，路径里没有空格和中文。

命令行编译已验证：把官方 `01_Ti60F225_key_demo` 复制到英文路径，运行 `bin\setup.bat` 后执行 `efx_run.bat LED_8bit_Test.xml`，map、interface、pnr、pgm 四步都是 PASS，用时 71 秒，生成了 `.bit` 与 `.hex`，没有要求许可证。烧写也有命令行入口 `pgm\bin\ftdi_pgm.bat`（`--scan_usb` 列出 USB 与 FPGA，`-m jtag` 等选择模式）。

安装包在 `D:\Download`。其中 `efinity-2026.1.132-windows-x64.tar.gz` 与 msi 是同一版本的另一种打包，`efinity-riscv-ide-2026.1.0.7-windows-x64 (1).msi` 与不带 `(1)` 的文件 SHA256 相同，都没有用到。`efinity-2026.1.132.4.5-windows-x64-pgm-patch.zip` 没有装：它是独立版 Programmer 的补丁，`run.bat` 默认目标是 `C:\Efinity\Programmer\2026.1`，只含 Programmer、Debugger 与加密工具的文件；打到完整版 Efinity 上会把版本号改成 2026.1.132.4.5，综合与布局布线部分却仍是 132。需要补丁时应下载完整版 Efinity 的补丁包。

本工程由 Efinity 2025.2 建立，换到 2026.1 有两处不兼容：

- SoC IP：工程里是 efx_soc 3.3.0，Efinity 2026.1 自带 3.4.0，打开工程会提示升级。升级会重新生成 `fpga/ddr_demo_ti60/ip/soc/` 下的文件和软件包。本目录 2026-09-16 已建 git 仓库（`main` 分支），初始提交 `f3090f0` 就是升级前的备份点，升级后可以用 `git diff` 查看生成器改了哪些文件。`settings.json` 中的 `LDSize=4092` 是界面上 Application Region Size 的「4MB」选项，`HexFile_Path` 写绝对路径也可以，生成器会自动换成相对工程目录的路径（`generator/soc_gen.py` 第 746~749 行）。
- 软件编译：`sdk/` 里的 BSP 是 2025.2 生成的，makefile 的默认工具链前缀是 `riscv-none-embed-`，`-march` 只有 `rv32im`。实测用 IDE 2026.1 的 gcc 13.4 编译 bootloader 与 gpioDemo，都在 CSR 指令处报 `extension 'zicsr' required`。Efinity 2026.1 的软件包模板（`ipm/ip/efx_hard_soc/embedded_sw/software/standalone/common/riscv64-unknown-elf.mk`）已经加上 `_zicsr_zifencei`。所以升级 SoC IP 以后要把新生成的软件包同步到 `sdk/`，同步时保留「Bootloader 与程序区」一节列出的修改。在那之前，可以用 WSL 中的 gcc 8.3.0 编译（见 `doc/板卡实测记录.md` 第 5 节）。

1. 硬件：用 Efinity 打开 `fpga/ddr_demo_ti60/ddr_demo_ti60.xml`。先在 Interface Designer 中打开接口设计，确认 `HDMI_PLL` 算出的频率与相位同上表一致，并运行 Check Design，然后再编译。
2. 软件：用 RISC-V IDE 导入工程，BSP 位置选 `Main/sdk`。步骤见资料目录中的《TI60F225I3-V4 DEMO板软硬件设计说明》第 77~87 页。
3. 上板：
   - 先烧写 `bit/official/` 下的官方预编译比特流，验证下载链路和 DDR3。这份比特流是官方工程目录中的产物，不含 HDMI。
   - 再烧写本目录编译出的比特流。HDMI 显示器上应出现八色竖条。

## 相对官方工程的改动

未搬入的内容。它们会在编译或调试时重新生成，并且里面写着官方机器上的绝对路径：

- `outflow/`，其中只保留了 `.bit` 与 `.hex`，放在 `bit/official/`；
- `work_syn/`、`work_pnr/`、`work_pt/`、`work_dbg/`；
- `embedded_sw/soc/.metadata/` 与 `embedded_sw/soc/bsp/.metadata/`，这两个是 Eclipse 工作区缓存；
- `la0_waveform.*`、`phy_debug_waveform.*`，这些是 Debugger 抓到的波形。

改正绝对路径的 5 个文件：

- `fpga/ddr_demo_ti60/ip/soc/settings.json`：`--base_path` 改为 `E:\Workspace\Efinix\Main\fpga\ddr_demo_ti60\ip`；
- `sdk/software/standalone/gpio/gpioDemo/.cproject`，以及 `gpioDemo_ti.launch`、`gpioDemo_trion.launch`、`gpioDemo_tz.launch`：BSP 路径改为 `E:\Workspace\Efinix\Main\sdk`。

为接入 HDMI 修改的 4 个文件：

- `rtl/ddr3_example_top.v`：新增 14 个 HDMI 端口和 `hdmi_tx` 实例，原有端口与逻辑不变；
- `fpga/ddr_demo_ti60/ddr_demo_ti60.peri.xml`：
  - 3A bank 电压由 1.5V 改为 1.8V，与板上硬件一致，官方 v6 与 02 号 HDMI 工程也是 1.8V；
  - 新增 `clk_27m` 输入、`HDMI_PLL` 和 4 路 LVDS TX；
- `fpga/ddr_demo_ti60/sdc/ddr3.sdc`：新增 HDMI 时钟、时钟组，以及 LVDS TX 输出延迟（数值取自同为 2025.2 版本的官方 02 号 HDMI 工程）；
- `fpga/ddr_demo_ti60/ddr_demo_ti60.xml`：源文件列表加入 `rtl/hdmi/` 下的 4 个文件。

为扩大程序区修改的文件：

- `sdk/software/standalone/bootloader/src/bootloaderConfig.h`：`USER_SOFTWARE_SIZE`；
- `sdk/bsp/efinix/EfxSapphireSoc/linker/default.ld`、`freertos.ld`：程序区长度与断言；
- `fpga/ddr_demo_ti60/ip/soc/settings.json`：`LDSize`、`HexFile_PathEnable`、`HexFile_Path`；
- `fpga/ddr_demo_ti60/ip/soc/` 与 `ip/soc/source/hardware/netlist/` 下的 8 个片上 RAM 初始文件。

为命令行编译修改的文件（2026-09-16）：

- `fpga/ddr_demo_ti60/ddr_demo_ti60.xml`：`<efx:debugger>` 下的 `auto_instantiation` 由 `on` 改为 `off`。原因：自动插入调试核这一步要求 `work_dbg/debug_top.v` 事先存在，这个文件在 GUI 里由 Debug Wizard 生成，命令行的 `efx_run` 不生成，而 `work_dbg/` 没有搬进本目录。关掉后少的只是自动插入的逻辑分析仪 `la0`（探针 `cal_done`、`ddr_rstn`），顶层用 `` `define Efinity_Debug `` 手工例化的 `edb_top` 仍在。
- 同一次编译中，Efinity 2026.1 把 `ddr_demo_ti60.xml` 与 `ddr_demo_ti60.peri.xml` 按新版格式重写了一遍（版本号、标签写法、子元素顺序）。用 `git diff -w` 核对过，三个 PLL 的分频、相位与反馈设置没有变化。

新增的文件在 `rtl/hdmi/`、`sim/hdmi/`、`sdk/software/standalone/application/bootSizeTest/` 三个目录下，另有 `sdk/tool/bootloader/bootloader_512K.bin` 与 `.hex`。其中 `color_bar_rgb.v`、`dvi_encoder.v`、`encode.v` 与官方 `03_hdmi_tx_demo` 中的文件 MD5 一致。除上面列出的文件外，其余文件在搬入时与官方工程逐个比对过 MD5，全部一致。

## 注意事项

- 接口设计：`peri.xml` 中 HDMI 相关的部分是按官方工程的格式手工写入的，2026-09-16 编译时已通过 Interface Designer 的检查，PLL 参数没有被改动。以后如果在 Interface Designer 里调整了 PLL 参数，要同步检查 `sdc/ddr3.sdc` 中的时钟定义。
- 路径依赖：
  - 上面 5 个改过绝对路径的文件，以及 `settings.json` 中的 `HexFile_Path`，`Main/` 换了位置就要一起改；
  - `ip/soc/source/soc_config`、`sdk/bsp/efinix/EfxSapphireSoc/openocd/debug_profile_soft.cfg` 与 gpioDemo 的 `.cproject` 仍引用 `C:\Efinity\2025.2` 和 `C:\Efinity\efinity-riscv-ide-2025.2`。本机装的是 2026.1，这些文件会在升级 SoC IP、重新导入软件工程时重新生成，不必手改。
- 软件包的位置：官方工程里，软件包放在 Efinity 工程目录下的 `embedded_sw/soc/`。如果在 Efinity 里重新生成 SoC IP，要检查新生成的软件包输出到了哪里，再把 `bsp/` 同步到 `sdk/bsp/`。
- 验证状态（2026-09-16）：Efinity 2026.1 编译通过，时序没有负 slack；JTAG 加载后，片上 RAM 读回的内容与新 bootloader 一致，memTest 通过，HDMI 显示器上八色竖条正确（`doc/板卡实测记录.md` 第 6、7 节）。512KB bootloader 从 Flash 引导还没有上板确认。
