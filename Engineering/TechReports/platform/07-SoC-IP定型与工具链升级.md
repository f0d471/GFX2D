# 07 SoC IP 定型与工具链升级

## 一、动机

第 04 章做完构型对比，结论是保留 Standard。原来的 Todo 条目「soc资源裁剪」里唯一能省片上 RAM 的一项
（DDR 跨时钟 FIFO 的 17 块）只有 Lite 加统一内存时钟才能省，这一项随之关闭。
剩下的裁剪项（I2C0 约 370 XLR、`axiA` 从口约 22 XLR、已被优化掉的 `io_ddrMasters_0`）都不省 RAM，
逻辑单元只用了约 36%，单为它们重新生成一次 IP 不划算。

但 SoC 的配置不是只有 Lite 与 Standard 两档可选。`ip/soc/settings.json` 里每个外设都有自己的开关
（`I2C0`~`I2C2`、`GPIO0`/`GPIO1` 与宽度、`USER_0_INTR`~`USER_7_INTR`、缓存大小与路数、`FPU` 等），
被构型绑住的只有 `DDRCLK_DOMAIN` 一项。后面的工作点对 SoC 还有几样需求：

| 需求 | 出处 |
| --- | --- |
| 按键 KEY0~3 接进 SoC | 设计文档 1.3 节，游戏输入 |
| 扫描输出的场同步中断、引擎的中断、摄像头的帧完成中断 | 设计文档 1.3 节、扫描输出条目 D5、摄像头条目 |
| 摄像头的 I2C 与模组复位线 | 摄像头条目 1.3 节 |
| efx_soc 3.3.0 升到 3.4.0，改用 gcc 13.4 | Efinity 2026.1 打开工程就提示升级；旧软件包只能用 gcc 8.3 编 |

改 SoC 就要重新生成 IP 与软件包、恢复 `sdk/` 的登记项、再跑一遍全套回归。
这件事越晚做越危险：扫描输出、引擎、摄像头接进顶层之后，一次重新生成扰动的布局更多
（`core_clk` 的 hold 余量只有 0.026 ns），回归要覆盖的东西也更多。
所以本章把这些需求一次收齐，生成一次，之后 SoC 不再动。唯一留下的口子是 E8 的缓存大小：
它要先有测每弹幕开销的程序才有数据，出结论后若要改，是只改一个参数的一次生成。

## 二、为什么是这几步

1. 配置定稿：把上表的需求落成 `settings.json` 的开关。
2. 找一条能改端口的生成路径。
3. 在仓库外的复制工程里试生成，对照端口与初始文件。
4. 在主线工程里生成，同步软件包，恢复登记项。
5. 顶层与管脚约束跟进。
6. 换工具链，重编全部程序。
7. 编译比特流，按 0.4 节检查时序、资源、告警、接口。
8. 上板：调试通路、回归表全部、`bootSizeTest` 与对照组、gfx2dGame 的逐帧 CRC、按键。

第 2 步是这一章新增的。E1（第 04 章）用 `tools/gen_soc.py` 直接调生成器 `soc_gen.py`，
只换了网表，没有换工程引用的包装文件 `soc.v`：那一次只动构型，端口集合几乎不变
（Lite 统一时钟少一个 `io_memoryClk`，手工删掉即可）。这一次要开 GPIO1、开两条中断、关 `axiA`，
端口集合变了几十项，只换网表不够。第 3 步是因为生成器会整体覆盖 `ip/soc/`，
先在复制工程里看清它会改什么，主线上才知道哪些差异是预期的。
第 8 步里 `bootSizeTest` 必须跑，因为工具链变了；gfx2dGame 的逐帧 CRC 必须跑，
因为 SW1 的判据「PC 与板上逐帧相同」是在旧工具链下成立的。

## 三、每一步做了什么

### 3.1 配置定稿

| 项 | 原值 | 定为 | 理由 |
| --- | --- | --- | --- |
| 构型 | Standard，`DDRCLK_DOMAIN=1` | 不变 | 第 04 章 |
| IP 版本 | 3.3.0 | 3.4.0 | 见一 |
| `GPIO1` | 关（`GPIO1Width` 已是 8） | 开 | [3:0] 接 KEY0~3，[4] 预留摄像头复位，[7:5] 备用 |
| `USER_1_INTR`、`USER_2_INTR` | 关 | 开 | A 给扫描输出、B 给引擎、C 给摄像头，一个来源一条线 |
| `I2C0` | 开 | 保留 | 原计划关掉，改为留给摄像头配置传感器 |
| `AXISlave`（`axiA` 从口） | 开 | 关 | 控制寄存器全走 APB Slave 0 |
| `AXIMaster`（`io_ddrMasters_0`） | 开 | 关 | 第二个主机都走 `Axi_Mux`；它的 ID 最高位是 1，接上会让响应回错主机（第 02 章） |
| 缓存、调试模块、其余外设 | — | 不变 | 缓存留给 E8；调试模块是 OpenOCD 的依靠 |

对应的参数名是查 `<EFINITY>/ipm/ip/efx_soc/efx_soc/ipm/ip_component.xml` 里各端口的 `isPresent` 条件得到的：
`axiA_*` 与 `axiAInterrupt` 是 `${AXISlave} == 1`，`io_ddrMasters_0_*` 是 `(${AXIMaster} == 1)&&(${DDR} == 1)`，
`userInterruptB`、`userInterruptC` 分别是 `${USER_1_INTR} == 1`、`${USER_2_INTR} == 1`，`system_gpio_1_io_*` 是 `${GPIO1} == 1`。

生成后 `soc.h` 里的中断号：A 是 16，B 是 17，C 是 22（`SYSTEM_PLIC_USER_INTERRUPT_[ABC]_INTERRUPT`）。
摄像头只在 SoC 里预留，不接管脚、不改 `peri.xml`：管脚、I2C 的顶层连线与复位线都等摄像头工作点自己接，那时不需要重新生成 IP。

### 3.2 找一条能改端口的生成路径

`ip/soc/soc.v`（27250 行）的第 49 行是包装模块 `soc`，第 392 行起是网表，
所有模块名都带了唯一后缀（如 `EfxSapphireSoc_469dfaa85da54d55815c0d2324d480a8`）；
而 `soc_gen.py` 的产物 `source/hardware/netlist/EfxSapphireSoc.v` 里模块名不带后缀。
也就是说，`soc.v` 不是生成器出的，是 IP Manager 在生成器之后做的：按 `isPresent` 裁出包装模块的端口，
再把网表改名拼在后面。软件包同理，由 `ip_component.xml` 里另一个文件集 `external_script_embedded_sw`
调用 `embedded_sw/sw_script.py` 生成，输出到 `<工程>/embedded_sw/soc/`，目录结构与本仓的 `Main/sdk/` 一一对应。

IP Manager 有官方的 Python 入口 `ipm_api_service.design.IPMDesignAPI`，
Efinity 安装目录下附了示例 `project/example_scripts/ipm/example_fifo.py`：
`create_ip` 取当前安装里最新版本的 IP，`config_ip` 设参数，`validate_ip` 校验，`generate_ip` 生成。
这条路走的是图形界面背后的同一套代码，端口裁剪、改名、软件包都在里面。
本章把它写成 `Main/tools/ipm_gen_soc.py`：读 `settings.json` 的用户参数逐项设置，
`--override` 给改动项，`--outputs` 只选 `generator`、`source`、`script`、`embedded_sw` 四个文件集
（示例工程与四种仿真器的文件集会往工程里写一堆用不到的东西），并在生成前列出三份清单：
`settings.json` 里有而新版本没有的参数、`settings.json` 里的自动参数、新版本里取默认值的用户参数。
这三份清单是「升级之后参数有没有悄悄变」的判据。

`--dry-run` 的输出（主线工程，改动项五个）：

```
IP 版本    : 3.3.0 -> 3.4.0
覆盖       : GPIO1=1'b1 USER_1_INTR=1'b1 USER_2_INTR=1'b1 AXISlave=1'b0 AXIMaster=1'b0
设置的用户参数 152 项
settings 里有、新版本里没有（忽略）：无
settings 里是自动参数（由 IP 重新推导）：无
新版本的用户参数取默认值：
    无
validate_ip 通过
```

三份清单都是空的：3.3.0 的 152 项用户参数在 3.4.0 里一一对应，没有新参数取默认值。
所以升级本身不带来配置上的变化，差异只来自五个改动项与 3.4.0 的模板。

`gen_soc.py` 保留，用途写明为「只换网表的构型对比」。

### 3.3 在复制工程里试生成

复制工程放在仓库上一级的 `.socgen/`（与 E1 的做法相同），只带 `gfx2d_ti60.xml`、`peri.xml` 与 `settings.json`。
生成成功后对比主线的 `soc.v`，只看包装模块的端口表：

```
  删掉的                                  新增的
  axiA_*（40 个端口，含 axiAInterrupt）   system_gpio_1_io_read[7:0]
  io_ddrMasters_0_*（41 个端口，含 clk）  system_gpio_1_io_write[7:0]
                                          system_gpio_1_io_writeEnable[7:0]
                                          userInterruptB
                                          userInterruptC
```

与 3.1 节的表逐项对上，没有多出或少掉的端口。
四个片上 RAM 初始文件（`ram_symbol0~3.bin`）与主线逐字节相同：
这一次 `HexFile_Path` 直接指向 `sdk/tool/bootloader/bootloader_512K.hex`，生成器用它生成初始文件，
结果与 2026-09-16 手工替换进去的那四个文件一致，说明 bootloader 没有被这次生成动到。

生成器记录的命令行 `source/soc_config` 暴露了一处计划外的变化：

```
旧  --i2c name=system_i2c_0_io,address=0x016000,interruptId=8
新  --gpio name=system_gpio_1_io,address=0x016000,width=8,interrupts="0->14;1->15"
    --i2c name=system_i2c_0_io,address=0x017000,interruptId=8
```

本工程的 `ADDR_Scheme` 是 0，外设地址由生成器按顺序自动排，`settings.json` 里的 `*_M_Addr` 不起作用。
GPIO1 一开，就占了 I2C0 原来的 `0xF801_6000`，I2C0 挪到 `0xF801_7000`。
现在没有程序用 I2C0，UART0（`0x0000`）、SPI0（`0x4000`）、GPIO0（`0x5000`）、APB Slave 0（`0xF810_0000`）都没动，
所以没有影响；新地址登记在 `fpga/README.md` 的外设表里，摄像头工作点按它写。

### 3.4 主线生成、同步软件包、恢复登记项

`localize_paths.py --apply` 展开路径后在主线工程生成。之后三件事：

1. `ip/soc/` 下多出的 `T120F324_devkit/`、`Ti60F225_devkit/`、`Testbench/` 是示例文件，以前也不入库，删掉。
2. `embedded_sw/soc/` 整体复制到 `Main/sdk/`，删掉 `embedded_sw/`。
3. 看 `git diff Main/sdk`，逐个文件说出来源。

同步后 `Main/sdk/` 有 17 个文件改动、2 个新文件。逐个归类：

| 文件 | 来源 | 处理 |
| --- | --- | --- |
| `bootloaderConfig.h` | 登记项 `USER_SOFTWARE_SIZE` 被改回官方的 `0x01F000` | 恢复 |
| `linker/default.ld`、`freertos.ld` | 登记项「镜像超限断言」被删；程序区 4092K 由 `LDSize` 生成，本来就对 | 恢复 |
| gpioDemo 的 `.cproject` 与三个 `.launch` | 占位符被展开成本机路径 | `--restore` 之后与已提交的相同 |
| `openocd/ftdi_ti.cfg` | 3.4.0 模板把 `ftdi device_desc "Titanium Ti60F225 Development Kit"` 这一行打开了，旧版是注释掉的 | 恢复成注释掉的版本，**补登记**（见下） |
| `include/soc.h` | 开 GPIO1、开中断 B/C、关 `axiA`、I2C0 挪地址 | 预期 |
| `openocd/debug_profile_soft.cfg` | 工具路径 2025.2 换成 2026.1，多了 `FAMILY`、`DEVICE` 两行 | 预期 |
| `openocd/sapphire_soc_xsvd.json` | 调试器的寄存器描述，随外设增减 | 预期 |
| 两份 `riscv64-unknown-elf.mk` | 前缀 `riscv-none-embed-` 换成 `riscv-none-elf-`，`-march` 加 `_zicsr_zifencei`，加 `-fcommon` | 3.4.0 模板，本章要的就是它 |
| 两份 `standalone.mk` | 加 `-ffunction-sections -fdata-sections`、`--gc-sections`、`--no-warn-rwx-segment`，链接 `syscalls.o` | 3.4.0 模板 |
| 两份 `trap.S` | 新增浮点寄存器的保存，只在定义了 `__riscv_flen` 时生效 | 3.4.0 模板；本构型没开 FPU，行为不变 |
| 两份 `syscalls.c`（新） | newlib 的桩函数，消掉链接告警 | 3.4.0 模板 |

`ftdi_ti.cfg` 那一行的判断：旧版里它是注释掉的，这份文件 2026-09-16 入库时就是这样，
`sdk/README.md` 的登记表里没有它，也查不到是谁改的。板上的 OpenOCD 一直按注释掉的版本工作，
而这一行打开之后，OpenOCD 会要求下载器的 USB 描述字符串与之完全一致，本板 FT4232 的描述对不对得上没有核实过。
所以保持注释掉的版本，登记进 `sdk/README.md`，理由写明「未核实」而不是「官方的错了」。
5.2 节上板时 APB 探针那一项就是用这份 cfg 跑的，能用。

`ip/soc/` 这边的三处处理：

- `settings.json`：3.4.0 把 `--base_path` 写成相对路径 `ip`，不再需要占位符；`HexFile_Path` 仍是绝对路径。
  它的 `output.external_source_source` 被 API 写成了空列表（图形界面生成时这里记着 `soc.v` 等四个源文件），
  按实际生成的四个文件补回。生成器还把行尾写成了 CRLF，与原文件一致，保持。
- `source/soc_config`：3.4.0 把 bootloader hex 的完整路径写进了这个文件（旧版这里是 Efinity 自带的 124K hex 的安装路径）。
  它含仓库的本机路径，加进 `tools/localize_paths.py` 的处理清单，风格 `bs`。
- 其余是生成物，原样入库。

### 3.5 顶层与约束

`rtl/top/gfx2d_top.v`：

```
u_soc 的连接                       改动前                     改动后
  axiA_*                           输入接 0、输出悬空         删除（端口已不存在）
  io_ddrMasters_0_*                输入接 0、时钟不接         删除（端口已不存在）
  system_gpio_1_io_read[7:0]       （无）                     {4'b0000, key_n}
  system_gpio_1_io_write/OE        （无）                     悬空，[4] 留给摄像头复位
  userInterruptA / B / C           A 接 0                     三条都接 0，源模块接入时各自接上
  system_i2c_0_io_*                读口接 1、写口悬空         不变，注释写明留给摄像头
顶层新增端口  key_n[3:0]  input
```

按键不做同步与消抖：SoC 的 GPIO 在自己的时钟域采样，软件按帧读，抖动由软件处理。

`peri.xml` 加四个输入：`key_n[0..3]` 分别在 `GPIOL_07`、`GPIOR_22`、`GPIOR_21`、`GPIOL_03`（球号 C4、P14、N14、A3），
3.3V LVCMOS，不开内部上拉。这组取值来自同一块板的官方按键例程
（资料包 `02_.../01_Ti60F225_Key_led_osc_demo/01_Ti60F225_key_demo/`，`LED_8bit_Test.peri.xml` 与 `pinout.csv`），
例程的 `key_detect.v` 在 `~i_key` 时计数，即按下为 0。四个管脚在本工程里都没被占用，
所在的 TL、BR 两个 bank 已经是 3.3V（UART 与 LED 就在上面）。

### 3.6 工具链

3.4.0 的软件包要 `riscv-none-elf-` 前缀与认识 `_zicsr_zifencei` 的 gcc。
IDE 2026.1 自带的是 xPack 13.4.0，但那是 Windows 版，本工程的编译在 WSL 里跑。
所以从 xPack 的发布页取同一版本的 Linux 版 `xpack-riscv-none-elf-gcc-13.4.0-1-linux-x64.tar.gz`，
SHA256 `2f669492e131906f21641de3ed5cb808d17eff2716b551c73651fe6650565a5f` 与发布页的 `.sha` 文件一致，
解压到 WSL 的 `~/opt/`，旧的 8.3.0 留着但不再使用。

七个诊断程序（新增 `keyTest`）与 memTest 用官方 makefile 编，全部无错误无告警。
gfx2dGame 走项目自己的 `sw/mk/target.mk`，它不经过 sdk 的 `standalone.mk`，
链接时多出一条新版 binutils 才有的 `LOAD segment with RWX permissions` 告警；
sdk 的新模板用 `--no-warn-rwx-segment` 压掉了同一条，`target.mk` 照样加上。

`sw/bsp/ti60/board.c` 的 `board_keys()` 仍返回 0，只在注释里登记 GPIO1 的基址与按键位：
按键映射到哪几个 `BOARD_KEY_*` 位是 SW9 的事。

新程序 `sw/apps/test/keyTest`：启动时要求空闲读数为 `0x0f`，然后 30 秒内每毫秒采样一次，
读口变化就打一行，结束时要求四个键各出现过一次按下与松开。
GPIO1 的基址取 sdk 的 `SYSTEM_GPIO_1_IO_CTRL`，SoC 没开 GPIO1 时这个宏不存在，程序直接编不过。

### 3.7 编译与上板

编译按 `doc/开发环境.md` 0.4 节，上板按 0.6 节，数字在第五节。

## 四、遇到的问题

### 4.1 IP Manager 的 API 在普通 Python 环境里加载不起来

**第一现场**

```
  File "C:\Efinity\2026.1\python311\Lib\site-packages\devicedb\pydevicedb.py", line 62, in <module>
    lib = ffi.dlopen(wrapper_lib)
OSError: cannot load library 'C:\Efinity\2026.1\python311\Lib\site-packages\devicedb\libdevicedb_wrapper.dll': error 0x7e
```

**解读**：`0x7e` 是 Windows 的 `ERROR_MOD_NOT_FOUND`，DLL 本身在，它依赖的其它 DLL 找不到。
当时只设了 `PYTHONHOME` 与 `EFINITY_HOME`，没有把 Efinity 的 `bin` 目录放进 `PATH`。

**定位**：Efinity 的命令行入口（`efx_run.bat` 等）都先 `call bin\setup.bat`，它把
`python311\bin`、`bin`、`pgm\bin` 等七个目录加到 `PATH` 前面，并设 `EFXIPM_HOME` 等变量。

**根因与修法**：在 `setup.bat` 设好的环境里用它自带的 python 运行。
`ipm_gen_soc.py` 开头检查 `EFINITY_HOME` 与 `EFXIPM_HOME`，缺了就报错并提示先 `call setup.bat`。
原判据没有覆盖它，因为这是第一次在图形界面之外调 IP Manager 本身。

### 4.2 复制工程放在 C 盘时生成失败

**第一现场**（API 只返回 `ResultCode.GENERIC_ERROR`，错误在 `<EFINITY>/ipm/log/efxIP.log` 里）：

```
[ERROR] [efx_ipmgr.builder] [BUILDER_INVOKE_BUILD_COMMAND] stderr: Traceback (most recent call last):
ValueError: path is on mount 'E:', start on mount 'C:'
```

**解读**：Python 的 `os.path.relpath` 不能跨盘算相对路径。

**定位**：`soc_gen.py` 第 740 行起，`HexFile_Path` 是绝对路径时会改写成相对工程目录的路径。
复制工程在会话的临时目录（C 盘），bootloader hex 在仓库里（E 盘）。

**根因与修法**：复制工程改放 E 盘仓库外的 `.socgen/`，与 E1 的位置一致。
主线工程与 hex 在同一个盘，不受影响。原判据没有覆盖它，因为 E1 的复制工程恰好也在 E 盘。

### 4.3 生成器改写的 settings.json 与原文件有三处格式差异

**第一现场**：`git diff` 显示 `settings.json` 整个文件都变了（189 行增、182 行删），忽略空白后只有 29 行。

**解读与定位**：三处差异。行尾：生成器写 CRLF，原文件也是 CRLF，我在核对时误判成 LF，一度改成 LF，
又用「差异行数与忽略空白后的差异行数不等」这个办法逐个文件查出来，改回 CRLF；
这次误判的原因是 Git Bash 下 `grep -c $'\r'` 对管道输入的计数不可靠。
`--base_path`：由占位符的绝对路径变成相对路径 `ip`，属于 3.4.0 的行为，更好，保留。
`output.external_source_source`：API 写成空列表，按实际补回四个源文件。

**根因与修法**：前两处是 3.4.0 与 API 的正常行为；第三处可能是 API 与图形界面记录输出的方式不同，
工程编译按 `gfx2d_ti60.xml` 里的 `ip_src_file` 取源文件，不读这一段，补回是为了让图形界面看到的状态与实际一致。
原判据没有覆盖它，因为此前没有用 API 生成过。

### 4.4 bootSizeTest 的对照组第一次假通过

**第一现场**

```
（官方 124KB bootloader 的比特流，对照组，期望两个 CRC 都对不上）
image 302028 bytes, old copy size 126976 bytes
blob 0x000017dc size 300000, tail 0x00049bbc
crc_all 0x47df83a8
crc_tail 0x78c8c48f
done
```

**解读**：两个 CRC 都等于期望值，对照组本应对不上。124KB bootloader 只拷前 126976 字节，
数据块在 `0x17dc` 起 300000 字节，后面约 17 万字节它根本没拷。

**定位**：我先跑了实验组（512KB bootloader）。它把完整镜像拷进了 DDR；重新载入比特流不会给 DDR 断电，
对照组的 bootloader 只覆盖前 124KB，数据块尾部读到的正是上一次留下的同一份数据。
第 01 章 3.5 节早就写了「上板时先跑对照组，再跑实验组；先跑 512KB 版会在数据块尾部留下正确数据，对照组可能假通过」，
我没有照做。

**根因与修法**：用 OpenOCD 把 `0x20000` 起 `0x30000` 字节清零（`mww 0x00020000 0 0xC000`，读回首尾都是 0），
再按正确顺序重跑：对照组 `crc_all 0xd98716f9`、`crc_tail 0xc71c0011`，两个都对不上；
实验组 `0x47df83a8`、`0x78c8c48f`，都对上。清零这一步写进了 `doc/开发环境.md` 回归表的 `bootSizeTest` 一行，
作为顺序做反之后的补救办法。原判据写了「先跑对照组」，但写在第 01 章里，回归表那一行只写了「对照组必须不一致」，
执行时照着回归表做就会漏掉顺序；现在把顺序写进了回归表本身。

### 4.5 用内联脚本改文档时反斜杠被吃掉

**第一现场**：`sdk/README.md` 第 58 行 `call <EFINITY>\bin\setup.bat` 写成了 `<EFINITY>` 加退格符加 `in\setup.bat`。

**解读与定位**：用 bash heredoc 里的 Python 字符串写这一段，`\b` 被 Python 解释成退格符（`0x08`）。

**根因与修法**：按字节替换改回，检查全文件没有其它控制字符；之后改文档一律用编辑工具，不用内联脚本拼含反斜杠的文本。
原判据没有覆盖它：文档没有「不含控制字符」的检查，这一次是回读时发现的。

## 五、结果

### 5.1 编译

Efinity 2026.1，四步 PASS，与 SoC 定型前（第 06 章结束时）逐项对比：

| 项 | 定型前 | 定型后 |
| --- | --: | --: |
| XLR | 21753（35.78%） | 21653（35.61%） |
| 片上 RAM 块 | 65 | 65 |
| DSP | 4 | 4 |
| 输入管脚 | 61 | 65 |
| `core_clk` setup / hold（ns） | 3.780 / 0.026 | 3.405 / 0.026 |
| `sdram_clk` setup / hold | 0.398 / 0.095 | 0.486 / 0.084 |
| `rx_cal_clk` setup / hold | 0.369 / 0.028 | 0.372 / 0.027 |
| `tx_cal_clk` setup / hold | 0.372 / 0.100 | 0.505 / 0.095 |
| `tx_cal_clk_90edge` setup / hold | 0.447 / 0.035 | 0.408 / 0.072 |
| `hdmi_tx_slow_clk` setup / hold | 36.365 / 0.095 | 36.297 / 0.097 |

所有 slack 为正，`core_clk` 的 hold 没有变薄。`Missing Interface Pins` 仍是已知的 6 项，新加的 `key_n` 不在其中；
SDC 解析成功。`warn.log` 与上一次编译逐条对比：没有新增告警，消失的是 `io_ddrMasters_0_clk` 未连接一条
与它那条通路上五个队列被优化成常数的五条；自写代码的截断告警为 0。

按模块的 XLR 变化（`hier_util.rpt`，只列变化超过 10 的）：

| 模块 | 定型前 | 定型后 | 差 | 解释 |
| --- | --: | --: | --: | --- |
| `u_soc` | 8907.5 | 9132.5 | +225 | 见下行 |
| 　`system_gpio_1_io_logic` | — | 75.5 | +75.5 | 新开的 GPIO1 |
| 　`system_i2c_0_io_logic` | 347.5 | 442.5 | +95 | I2C0 配置没变，增量来源未查清（见六） |
| 　`system_cores_0_logic_cpu` | 3400.5 | 3424.5 | +24 | 3.4.0 网表差异，未拆 |
| 　`system_bmbPeripheral_bmb_decoder` | 230.5 | 244 | +13.5 | 多一个外设的地址译码 |
| 　`system_ddr_ddrLogic_arbiterAxi4Read/Write` | 55.5 | 27.5 | −28 | `io_ddrMasters_0` 关掉后 DDR 口的仲裁只剩一路 |
| 　`system_axiA_logic_bmbToAxiBridge` | 25 | — | −25 | `axiA` 关掉 |
| `u_scanout` | 3637 | 3298 | −339 | 本模块没有改动，是布局打包的波动 |
| `u_mux_guard` | 510 | 541 | +31 | 本模块没有改动，同上 |

片上 RAM 按模块分布与定型前完全相同（`u_soc` 43、`u_ddr3` 22）。

### 5.2 上板

同一份比特流（`sha256:4f1c7ec23d99f9ba…`，2,070,156 字节），gcc 13.4.0 编的程序：

| 项 | 结果 |
| --- | --- |
| 调试通路 | `board_run.py --check-only` 连跑三次，每次两次会话，6 次都读到 PC |
| memTest | `Data matched .. Test PASSED` |
| APB 探针 | `0xf8100000: 47465832 00000001`；SCRATCH 写 `a5a55a5a`、`5a5aa5a5` 都读回一致；`0xf8108008` 读回 0 |
| GPIO1 空闲读数 | `0xf8016000: 0000000f`，四个键都是高电平，低有效的极性与接线对 |
| `ddrAliasTest` | 35 行 `ok`，`base 0x0c800000 fail 0` |
| `ddrBurstProbe` | 37 行 `ok`，`IDENT guard ctrl=3 mask=7 age=64 ok`，`fail 0` |
| `cacheCoherencyTest` | 正常版 `fail 0`；`INJECT=1` 四行 `FAIL (inject)`、`fail 0`；`INJECT=2` 四行 `NOT_RED (inject)`、`fail 4` |
| `cacheMechTest` | `fail 0`，T1~T5、T7 的数值与 `sw/README.md` 登记的逐项相同 |
| `cacheMinTest` | `fail 0`，`S3 CACHE_NOEVICT` 按设计报 FAIL |
| `bootSizeTest` | 实验组两个 CRC 对上，对照组两个都对不上（过程见 4.4） |
| gfx2dGame | 60 帧 CRC 与入库的 PC 日志 `logs/pc_normal.log` 逐帧相同，`frames 60`、`done` |
| HDMI | 八色竖条，用户看屏确认 |
| 按键 | `keyTest`：空闲 `0x0000000f`；用户依次按板上丝印 SW0~SW3（即原理图的 KEY0~3），读口依次是 `0xe`、`0xd`、`0xb`、`0x7`，四个键各一次 down 与 up，`fail 0`、`done`。1 毫秒采样下没有看到抖动。日志 `Main/sw/apps/test/keyTest/logs/run_1.log` |

按键的丝印与网络名：板上印的是位号 SW0~SW3，说明书与原理图的网络名是 KEY0~3，
原理图里 KEY0 接 SW0、KEY1 接 SW1、KEY2 接 SW2、KEY3 接 SW3。装配图上另有一个 SW5，不是这四个按键之一，用途没有查。

`bootSizeTest` 的两个 CRC 只取决于数据块的内容，换工具链后期望值不变；
bin 从 301,882 字节变成 302,026 字节，数据块起点从 `0x1750` 变成 `0x17dc`，镜像仍然大于 124KB，对照组仍有区分力。

写 `bootSizeTest` 之前，Flash `0x380000` 起 512KB 的原内容已读出备份（仓库外）；
现在那一段里是 gcc 13.4 编的 `bootSizeTest`。

## 六、还欠什么

- **`ftdi_ti.cfg` 的 `device_desc`**：保持注释掉，本板下载器的描述字符串与它是否一致没有核实。
- **I2C0 的 +95 XLR 与 CPU 的 +24 XLR** 没有归因。I2C0 的配置没有变，增量可能来自 3.4.0 的网表或打包差异，
  要在「同一版本、只切 I2C0」的对照下才能拆开。
- **第 01 章那约 506 个 XLR 的逐项归因**原计划借这一次生成做，没有做；它与上一条是同一类工作，一并留着。
- **新开的三条中断线**只做到「接 0、程序照常跑」，使能后是否误触发没有单独测；源模块接入时各自的工作点会测。
- **缓存大小**留给 E8，出结论后若要改，按 `sdk/README.md`「重新生成 SoC IP 时」只改缓存参数再生成一次。
