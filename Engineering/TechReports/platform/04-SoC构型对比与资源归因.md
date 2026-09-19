# 04 SoC 构型对比与资源归因：决定保留 Standard

## 一、动机

第 01 章搭出的基线把 SoC、DDR3 控制器、HDMI 与 APB 控制通路接到了同一块板上，
第 02、03 章查清了 DDR 的地址规则并接上了第二个 AXI 主机。到这一章为止，
有一个数字一直在挡路：**片上 RAM 用掉了 65 块，全片只有 256 块。**

Ti60F225 的资源分布是偏的。逻辑单元宽裕，XLR 用 16151 / 60800，占 26.6%；
DSP 只用 4 个，占 2.5%；而片上 RAM 是 65 / 256，占 25.4%。
设计文档 2.3 节给渲染引擎留了 108 块，两者相加是 173 块。
数字上过得去，但没有富余到可以白扔——引擎的条带缓冲、纹理缓存、扫描输出的行 FIFO
都要吃这个资源。

这 65 块里有一块明显的浪费。SoC 内部的 `system_ddr_ddrLogic_cc_fifo` 占 **17 块**
（另有 2 块在同一个桥的读写命令 FIFO 上），它的用途是跨时钟域搬运 DDR 的读写请求与响应。
而顶层 `rtl/top/gfx2d_top.v` 把 `io_systemClk` 与 `io_memoryClk` 都接在 `core_clk` 上，
这个 FIFO 的两端是同一个时钟，它在这里不搬运任何东西。

它之所以存在，是因为 SoC 的构型选的是 **Standard**。Sapphire SoC 有两个构型，
手册（UG-RISCV-SAPPHIRE v6.0，Cache/Memory 页）写明「External Memory Clock Domain：
Unified System Clock」只在 **Lite** 构型可选。Lite 用统一系统时钟，因此不需要跨时钟 FIFO，
17 块 RAM 随之消失。代价写在手册的同一页：Lite 没有 FPU、自定义指令与压缩指令，
整数乘除与桶形移位器要手动打开。

于是有一个明确的取舍：**用 Lite 换掉 17 块片上 RAM，代价是多少 CPU 性能，
以及对后续工作有什么别的副作用。** 设计文档把它列成实验 E1。

## 二、为什么是这几步

要回答这个取舍，需要三类信息，缺一类就得不出结论。

| 步 | 内容 | 少了会怎样 |
| --- | --- | --- |
| 一 | 生成 Lite 构型的 SoC，与 Standard 逐项对比资源 | 只知道「能省」，不知道省多少、换来什么 |
| 二 | 编出两个比特流，取资源与时序的实测值 | 设计文档里的资源比对表是官方工程的 2025.2 结果，不是本工程在本版工具下的值 |
| 三 | 用同一份程序在两个构型上跑 CoreMark | 不知道性能代价，取舍无从判断 |
| 四 | 上板跑回归表 | 资源与分数都对，但板子上跑不起来，等于没有 |
| 五 | 把 65 块 RAM 与 XLR 的来源逐项归因 | 第 01 章 5.1 节里有一行「其余约 −506 XLR，差额反推，没有逐项归因」，重生成 IP 与重跑布局是最好的归因时机 |

前三步与第五步不需要板子，第四步必须上板。第四步也是这一章真正花时间的地方，
因为它暴露了一个没人预料到的问题：**Lite 构型的调试通路在这块板上不可用。**

这个实验的边界要写清：它只回答「换不换 SoC 构型」。
关 I2C0 之类的裁剪动作、升级 IP 版本、以及缓存加大，都不在本章范围内，
它们各自的代价与判据写在 [第 07 章](07-SoC-IP定型与工具链升级.md)。

## 三、每一步做了什么

### 3.1 拿到 IP 生成的非图形入口

Efinity 的 IP 生成在界面上叫 IP Manager，命令行没有入口：`efx_run.bat` 只负责编译，
`bin\efx_py.exe` 缺 DLL 起不来。要批量生成几套构型做对比，必须复现 IP Manager
对生成器的调用。

调用关系写在 IP 定义文件
`<EFINITY>/ipm/ip/efx_soc/efx_soc/ipm/ip_component.xml` 里，是明文的：

```
${EFINITY_PYTHON} ${IP_LIBRARY_ROOT}/efx_soc/efx_soc/generator/soc_gen.py
  --out_path <工程的 ip 目录> --user_project_path <工程目录>
  --gen_name soc --parameters ${IP_USER_PARAMETERS}
```

`--parameters` 是 `nargs="+"`，每一项形如 `KEY=VALUE`。真正跑 Python 的解释器是
`<EFINITY>/bin/python3.bat` 指向的 `python311\bin\python.exe`；注意它需要
`PYTHONHOME` 才能找到标准库（`python311\bin\pybuilddir.txt` 是空的、没有 `pyvenv.cfg`、
`Lib` 在上一层），这是 `ftdi_pgm.bat` 与 `efx_run.bat` 能跑而我们直接调它跑不起来的原因。

照着这个关系生成，先后撞上四个生成器自身的健壮性问题，见第 4.1 节。
解决之后生成本身是稳的，工具固化成 `tools/gen_soc.py`。

### 3.2 四套构型的参数

对比要干净，一次只动一个变量，所以一共做了四套：

| 变体 | `SOC_MODE` | 内存时钟 | 其它 |
| --- | --- | --- | --- |
| Standard @3.3.0 | 0 | 独立 | 仓库现状，efx_soc 3.3.0 生成物 |
| Standard @3.4.0 | 0 | 独立 | 用 Efinity 2026.1 自带的 efx_soc 3.4.0 重新生成 |
| Lite + 独立内存时钟 | 1 | 独立 | `DDRCLK_DOMAIN=0` |
| Lite + 统一时钟 | 1 | 统一 | `DDRCLK_DOMAIN=1`，即生成器命令行里的 `--noDdrAClock` |

四套都另开 `MULDIV_EXT=1` 与 `BARREL_SHIFTER=1`（Lite 下必须显式打开，
Standard 下这两个参数不生效，内置就有）。其余参数逐字相同。

四套都在仓库之外各复制一份工程来做，避免生成器覆盖 `ip/soc/` 与软件包时
需要恢复 `sdk/README.md` 里登记的那几处修改。复制件放在本仓上一级目录下的
`.e1\`、`.e1v34\`、`.e1liteclk\`，用完删除。

### 3.3 顶层要跟着改一处

Lite + 统一时钟那一套的 SoC 网表里没有 `io_memoryClk` 与 `io_memoryReset` 端口
（IP 定义里 `io_memoryClk` 的 `isPresent` 条件是
`((${SOC_MODE} == 0)||(${DDRCLK_DOMAIN} == 0)) && (${DDR} == 1)`，
Standard 下必然存在、Lite 统一时钟下不存在）。端口集合逐项对比过：
Lite 恰好少 `io_memoryClk` 一个，没有新增。

所以复制件的 `gfx2d_top.v` 里删掉了 `.io_memoryClk (core_clk)` 一行。
`io_memoryReset` 本来就接空，不用动；`io_ddrMasters_0` 那一整段保持不接时钟。
不改这一行，综合会报端口不存在。

### 3.4 编译与判据

四套各编一次比特流，四步全 PASS。判据按 `doc/开发环境.md` 0.4 节的顺序检查：
四步 PASS、时序每个时钟的 setup 与 hold 都为正、资源与上一版对比、
约束告警 0、`Missing Interface Pins` 仍是已知的 6 项、本项目的位宽截断告警为 0。

### 3.5 CoreMark

性能对比用 SDK 自带的官方 CoreMark，源码不改，只在 `Main/sw/` 下加一个 makefile
让它按本项目的方式编译，产物落在仓库外。两种构型跑**同一份 ELF**
（`-march=rv32im`、`-mabi=ilp32`、`ITERATIONS=2000`、`-O3`、GCC 8.3.0、
bin 33,970 字节），这样两边只有硬件的差别。

上板顺序按 `doc/开发环境.md` 0.6 节：**先开串口再加载程序**，
否则串口没打开期间板子发出的数据会被直接丢弃。加载用 OpenOCD 的
`load_image <bin> 0x1000 bin` 加 `resume 0x1000`。

### 3.6 506 XLR 的归因

第 01 章 5.1 节表里有一行「其余（死文件清理、外部主机口不接时钟等）约 −506 XLR，
差额反推，没有逐项归因」。这一段用两份已存在的报告把它重建：官方工程的
`ddr_demo_ti60-hierarchical_stats.rpt` 与本工程的 `gfx2d_ti60.hier_util.rpt`，
按模块名逐项对照。

### 3.7 工具与文档

上板过程中固化了两个工具，放在 `Main/tools/`：
`gen_soc.py`（3.1 节的生成调用）与 `board_run.py` 加
`openocd/gfx2d_debug.cfg`（3.5 节的上板流程，含官方 cfg 在本板上的两处修正）。
两者都写进了 `tools/README.md`。

## 四、遇到的问题

### 4.1 生成器有四处在手工调用时必踩的坑

**第一现场**（四个依次出现，都是 Python 异常）：

```
KeyError: 'SOC_MODE'
SyntaxError: invalid syntax   （eval 到 1'b0==1）
SyntaxError: unterminated string literal   （eval 到 32'd53248）
ValueError: Unknown format code 'x' for object of type 'str'
```

**解读与定位**：第一条是我把 `--parameters` 传成了一个字符串，而它是 `nargs="+"`，
每项要单独传。第二条是给值加了双引号，`1'b0` 被当成字符串比较。第三条更隐蔽：
`_format_token` 会对值做 `eval()`，而它的 `try` 只捕 `ValueError`，
Verilog 字面量抛的是 `SyntaxError`，会直接冒出来
（`ip_component.xml` 的 `settings.json` 里这种值到处都是）。

第四条花了最久。模板里有 27 个形如 `{APBSlave0_Addr}`、`{UART0_Addr}`、`{Base_AXIS}`、
`{Base_IM}` 的占位符，生成器用 `0x{:06x}` 去格式化它们，而值是字符串。
这些键在 `ip_component.xml` 里都是 `resolve="immediate"` 的**自动参数**——
在界面上不可编辑、保存配置时才有值。本仓库的 `settings.json` 被手工编辑过
（为把程序区改成 4092K、把 bootloader 指到 512KB 那几处），这类自动项全丢了。

定位办法是把 27 个占位符一次列全：导入生成器模块，把它 `config_dict` 里用到的
167 个占位符与配置里的 152 个键做差集（`tools/gen_soc.py` 现在做同样的事）。
补齐的规则来自生成器自己的 `alignAddress()`：外设
`X_Addr = X_M_Addr & 0xffffff000`，APB 从机掩码由 `APBSlave0_Size` 决定，
`Base_AXIS/Base_IO = Base_M_* & 0xff000000`，`Base_IM` 取 IP 定义里的默认片上 RAM 地址。

**根因与修法**：四条都是厂商生成器的健壮性问题，不是本工程配置错。
修法是在调用侧补：逐项传参、解析 Verilog 字面量、按 `alignAddress()` 的规则补齐自动项。
固化在 `tools/gen_soc.py` 里，并在脚本头注明依据与出处，便于升级 Efinity 之后重新核对。

**原判据为什么没有覆盖到它**：这一层此前一直是人在图形界面上点的，
界面负责解析参数与保存完整的配置，所以这四个问题在命令行路径上才第一次出现。

### 4.2 Smart App Control 挡住整条工具链

**第一现场**：

```
ImportError: DLL load failed while importing _ctypes: 应用程序无法正常启动
ImportError: DLL load failed while importing QtCore: 应用程序无法正常启动
（OpenOCD）Error: Can't communicate with the CPU    —— 实际是 libftdi1.dll 没加载起来
```

**解读与定位**：先怀疑文件沙箱，放开到完全访问后同样失败，排除。
再怀疑嵌入式 Python 的库路径（`pybuilddir.txt` 为空、无 `pyvenv.cfg`、`Lib` 在上一层），
设 `PYTHONHOME` 后 Python 本体能起，但 `import _ctypes` 仍失败。
用 `LoadLibraryW` 逐个试二进制取错误码，得到区别：
`python311.dll` 与 `libffi-8.dll` 能加载，`_ctypes.pyd` 报
**「应用程序控制策略已阻止此文件」**。

**根因**：系统开着 Smart App Control 且处于强制模式
（`HKLM\SYSTEM\CurrentControlSet\Control\CI\Policy` 的
`VerifiedAndReputablePolicyState = 1`）。系统日志
`Microsoft-Windows-CodeIntegrity/Operational`（事件 3077/3033，策略 ID
`{0283ac0f-fff1-49ae-ada1-8a933130cad6}`）给出确切对象：
Efinity 的 `_ctypes.pyd`、`PyQt6\QtCore.pyd`（编译要它）、
RISC-V IDE 的 `libftdi1.dll` 与 `libyaml-0-2.dll`（OpenOCD 要它们）。
生成 IP、编译、JTAG 加载、OpenOCD、读写 Flash 五件事一起不可用。

**修法**：关掉 Smart App Control，**不需要重启**，同一会话里即刻生效。
关掉之后逐项复测：`ftdi_pgm.bat --scan_usb` 读到 `idcode 0x10660a79`、
`efx_run.bat` 能起、OpenOCD 的 TAP 认到同一 ID。

一条值得记下的旁证：同一份日志里，2026-09-16（基线上板成功那天）全天只有 2 条拦截事件，
2026-09-17 一天 22 条，被拦对象在两天之间变了。这符合它依赖云信誉判断的行为，
也就是说**同一个文件今天被拦、明天放行都可能发生**，这类故障不能用「昨天还能跑」来排除。

### 4.3 官方 OpenOCD 配置在本板上两处对不上

**第一现场**：

```
Error: IR capture error at bit 4, saw 0x25 not 0x..3
（换成 irlen 5 之后）
cpu.yaml not found within 3 levels
Error: cpuConfigFile /cpu0.yaml not found
Error: target 'fpga_spinal.cpu0' init failed
```

**解读与定位**：第一条是 IR 宽度。`sdk/.../openocd/debug.cfg` 写的是
`-irlen 4`，而 `debug_ti.cfg` 写的是 5，`ftdi_pgm.bat --scan_usb` 报的 `ir_width` 也是 5。
改成 5 之后 TAP 正常识别。

第二条是 `cpu0.yaml` 的查找。两份官方 cfg 都写死「从当前目录往上最多 3 级」，
而实际运行目录离 `Main/sdk/cpu0.yaml` 有 5 级（在仓库外的工程副本里更远）。
我一开始按字面量把工作目录摆到「刚好 3 级」的位置，仍然找不到，
因为数是错的。两份 cfg 里都有一个 `LEGACY_IDE` 分支改用
`$::env(workspace_loc)/cpu0.yaml`，那条路才是可用的；但它被
`ftdi_ti.cfg` 第 1 行的 `set LEGACY_IDE 0` 覆盖，所以要在加载 cfg 之前设回 1。

**根因与修法**：两条都是官方 cfg 与这块板的实际不符。
修法是不改 `sdk/` 下的生成物（重新生成 IP 会覆盖），另写一份
`Main/tools/openocd/gfx2d_debug.cfg`，复刻 `debug_ti.cfg` 的目标定义、
`irlen` 取 5、`cpu0.yaml` 走 `workspace_loc`。

**原判据为什么没有覆盖到它**：`doc/开发环境.md` 第 6 节给的命令是
「在 `Main/sdk/bsp` 下启动 OpenOCD」，那个目录距 `Main/sdk` 是 2 级，正好落在 3 级以内，
所以按文档的命令能跑通；一旦换到别处（或在仓库外的副本里做实验）就暴露了。
文档里还写着「从当前目录往上最多找 4 级」，与官方 cfg 里的 3 级不符，一并订正。

### 4.4 Lite + 统一时钟构型的调试通路不可用

这是本章花时间最多的一个，也是把结论从「换 Lite」翻成「保留 Standard」的那一个。

**第一现场**：

```
（烧写后第一次会话：正常）
[fpga_spinal.cpu0] Target successfully examined.
（第二次会话）
Error: !!!
Error: Can't communicate with the CPU
Error: !!!
Warn : target fpga_spinal.cpu0 examination failed
```

**解读**：TAP 认得到（`tap/device found: 0x10660a79`），但 CPU 的调试模块不应答。
再细测发现失败点在**读写 CPU 核心寄存器与启动 CPU**：
只做 `load_image` 是成功的，加上 `reg pc` 或 `resume` 就挂住不返回，
而且一旦挂过，后续会话连考察都过不去，只能重新烧比特流恢复。

**定位**：按「先排除自己的配置、再排除工具、最后用对照实验分离变量」走。
`irlen`（官方 cfg 写 4，本板是 5）与目标定义（两份 cfg 的 `dbgbase`、`coreid`、
`jtagMapping`、`readWaitCycles` 逐项相同）都排除；
用仓库现行的 Standard 比特流做对照，同样的 cfg 与命令序列连续三次会话全部成功，
排除板子、下载器、OpenOCD 版本与 cfg 本身；
网表里 `system_hardJtag` 111 处、`jtagBridge` 17 处、`debugger` 30 处两版逐项相同，
排除调试模块被裁。

真正定根因的是两组对照：**用同一份 efx_soc 3.4.0 生成、但保持 Standard 构型**，
连续三次会话全成功；**Lite 构型但内存时钟保持独立**，也连续三次会话全成功。
两个对照都正常，唯一同时带「Lite」与「统一时钟」的那一份出问题，
变量就落在**统一内存时钟**上。

**根因**：触发条件是统一内存时钟，与 `SOC_MODE` 是不是 Lite 无关。
机理层没有查到——「统一时钟为什么会让调试模块卡住」需要 Efinix 的支持或源码级 JTAG 波形，
本地手段到此为止。

**修法与验证判据**：本节的实测已按 `Bugs/README.md` 的口径记录在这里
（触发条件、四个变体的对照、排除过的假设）。最值得先试的修法是软 JTAG TAP
（生成器的 `SOFT_TAP` 选项，成本一次生成加一次编译）；
按本章的结论，这条路径不再往下走，理由是它换来的收益不够。
将来若又要启用统一时钟，先做软 JTAG TAP 这一步，判据用
「同一比特流连续开三次 OpenOCD 会话、每次都能读到 `reg pc`」。

**原判据为什么没有覆盖到它**：E1 的判据里写了「上板跑回归表」，
但没有任何一条覆盖「调试通路本身还能不能用」。既有判据都默认
「端口对上、四步 PASS、能比特流烧进去」就等于这条通路可用，
而故障恰好发生在这一层之下：比特流能烧、TAP 能认、`load_image` 能下，
只有读寄存器与启动 CPU 会失败。回归表是按**功能**列的（内存、显示、启动、控制），
而「调试器能不能二次连接」是**开发流程本身**的可用性，在 Standard 下从未失败过，
所以没有人把它写成判据。

## 五、结果

### 5.1 四套构型的资源与时序

Efinity 2026.1，四套各编一次，四步全 PASS。

| 项 | Standard @3.3.0 | Standard @3.4.0 | Lite + 独立时钟 | **Lite + 统一时钟** |
| --- | --: | --: | --: | --: |
| XLR（含 E4 探针） | 16151 | 16151 | 16151 | **13215** |
| **片上 RAM 块** | 65 | 65 | 65 | **46** |
| DSP | 4 | 4 | 4 | 4 |
| 全局时钟 | 10 | 10 | 10 | 10 |
| `u_soc` XLR / RAM | 8805.5 / 43 | 8805.5 / 43 | 8805.5 / 43 | **6088.5 / 24** |
| `cc_fifo` RAM | 17 | 17 | 17 | **0** |
| `core_clk` setup / hold | 3.516 / 0.026 ns | — | — | **3.958 / 0.027 ns** |

**19 块 RAM 的构成对上了账**，这是本章最干净的一个结果：

| 项 | Standard | Lite + 统一时钟 | 差 |
| --- | --: | --: | --: |
| `system_ddr_ddrLogic_cc_fifo` | 17 | 0 | −17 |
| `system_ddr_ddrLogic_bmbToAxiBridge` 的两个命令 FIFO | 2 | 0 | −2 |
| `dataCache_1` / `IBusCachedPlugin_cache` | 6 / 6 | 6 / 6 | 0 |
| `system_ramA_logic` / SPI0 / UART0 | 4 / 2 / 2 | 4 / 2 / 2 | 0 |
| **`u_soc` 合计** | **43** | **24** | **−19** |

一处细节值得记下：Lite 里 `cc_fifo` 这个**实例名还在**（层次报告里 21 XLR），
只是 `mem` 列变成 0——统一时钟之后它被综合成纯逻辑，不再占片上 RAM。
凭「实例名消失」判断会得到错误结论。

**两条结论都比设计文档原先的预期更强：**

1. **省资源是真的**：−19 块片上 RAM（−7.4% 全片）、−2936 XLR（−4.8% 全片），
   时序还变好了（`core_clk` setup 从 3.516 到 3.958 ns，最快频率从 142.7 到 165.5 MHz）。
2. **但省下的每一块都来自统一时钟**：拿去统一时钟，
   Lite 构型与 Standard 的资源**逐项完全相同**（16151 / 65 / `cc_fifo` 17 块）。
   Lite 关掉 FPU、自定义指令、压缩指令这些改动，在这块设计上一项都没省。

### 5.2 CoreMark

同一份 ELF（`-march=rv32im`、`ITERATIONS=2000`、`-O3`、GCC 8.3.0，
bin 33,970 字节），Standard 构型实测：

```
Total ticks      : 1119631573
Total time (secs): 11.196316
Iterations/Sec   : 178.630189
Iterations       : 2000
Compiler version : GCC8.3.0
Compiler flags   : -o3
Correct operation validated.
CoreMark 1.0 : 178.630189 / GCC8.3.0 / -o3
```

`Correct operation validated` 与 11.196 秒的单次运行时长都满足 CoreMark 的有效性规则
（不小于 10 秒）。

**Lite 那一侧没有跑成**：统一时钟构型的调试通路不可用（4.4 节），
而板上 Flash 里没有装本工程的程序，所以加载不了程序。
「Lite + 独立内存时钟」那一版本可补测，但它与 Standard 资源相同、没有对比价值，
按本章的结论不再花时间。

因为没有 Lite 的分数，**「换 Lite 要付出多少性能」这个数没有量出来**。
这一点要如实写明，不能用别的数替代。

### 5.3 上板

四个构型都烧进板子做过 JTAG 会话测试，结果见 4.4 节。
Standard 与 Lite + 独立时钟的构型各自连续三次会话成功；
统一时钟的构型只成功一次，且 `resume` 挂住。

Standard 构型上的程序回归：`sw/apps/probe/ddrAliasTest` 用新工具跑了一遍，
输出与 `sw/README.md` 登记的一致（`LOW k=0..27`、`LOW k=-1`、`HIGH k=28..31`、
`CACHE_NOEVICT`、`CACHE_EVICT` 全部 `ok`，`base 0x0c800000 fail 0`，末尾 `done`）。
其余五项回归未在本章逐项复跑。

### 5.4 506 XLR 的归因

第 01 章 5.1 节那行「其余约 −506 XLR，差额反推」，用两份报告重建之后是 **+263.5**，
**符号相反**。逐项相减得到 +349.5，与实测总额 +613 差 263.5。

| 项 | XLR 差额 | 来源 |
| --- | --: | --- |
| 删掉官方工程的调试核与逻辑分析仪 `edb_top_autodbg_inst__` | −1420 | 官方 `hierarchical_stats.rpt`。第 01 章 4.2 节记的 −944 是其中 `la0` 独有的部分 |
| `u_soc` 对 `u_sapphire_soc` | +63.5 | 同构型同 RAM，方向与预期相反，八成是 Efinity 版本差异，待查 |
| `u_ddr3` 对 `u_ddr3_top` | −68.5 | 同一份加密控制器，2025.2 与 2026.1 编出的 XLR 不同 |
| 新增 `u_hdmi_tx` | +321 | 本工程新增 |
| 新增 `u_axi_mux` | +257.5 | 第 03 章新增 |
| 新增 `u_axi_probe` | +1162 | 第 03 章的临时测量主机，将来撤走 |
| 新增 `u_apb_probe` | +34 | 本工程新增 |
| **未归因** | **+263.5** | 两边顶层胶合逻辑与两个 Efinity 版本对同一份 RTL 的不同优化 |

两点要写清：**出处订正**——分模块数据不在 `ddr_demo_ti60.place.rpt` 里
（它只有一张 Resource Summary），在 `ddr_demo_ti60-hierarchical_stats.rpt`；
**未归因的 263.5 不能再平账**，它来自跨工具版本的比较，同一次生成里才能拆干净。

另外，把 `u_axi_probe` 那 1162 摘掉，本工程非测量占用是 14989，
比官方工程的 15538 少 549——**这一刀重构的净收益是 549，不是 506。**

### 5.5 定稿结论

**保留 Standard 构型，不换 Lite。**

理由是这次实验暴露的事实：省资源的那一版（Lite + 统一时钟）调试通路不可用，
它读不到 CPU 寄存器、`resume` 会挂住，因而无法用 OpenOCD 启动任何自写程序，
回归表六项里走调试通路的四项都做不了。
而**可用的三个构型资源完全相同**（16151 XLR、65 块片上 RAM、`cc_fifo` 17 块），
所以换 Lite 在当前不带来任何可用的资源收益，只带来一个未查清机理的调试故障。

代价是那 19 块片上 RAM 继续占着。设计文档 2.3 节的引擎 RAM 预算按 65 块的起点重算，
看是否还有别的可裁项——E1 顺手量出的两个裁剪项的占用是
I2C0 370.5 XLR、`axiA` 从口窗口 22 XLR，两者都**不省 RAM**，
所以「省 RAM」这条线在本章关闭，资源压力要靠引擎自身的设计去适配。

## 六、还欠什么

- **Lite 的性能代价没有量出来。** 统一时钟构型跑不了 CoreMark，
  所以「省 19 块 RAM 要降多少性能」这个数空缺。本设计不再走这条路，
  这个数也就不补了，但结论里要如实写明是「没量」而不是「量了没差别」。
- **统一时钟下调试模块卡住的机理没有查到。** 触发条件已锁定（统一内存时钟），
  机理需要 Efinix 支持或源码级 JTAG 波形。若将来因为别的原因又要用统一时钟
  （例如引擎的 RAM 预算真的撑不住），这条要重新捡起来，
  第一步是试软 JTAG TAP（生成器 `SOFT_TAP`）。
- **第二步「量裁剪项的净收益」没有做。** 原计划在同一份复制工程里再生成一次
  「Lite + 关 I2C0、`axiA`、`io_ddrMasters_0`」与 Lite 组对比。
  既然 Lite 这条线整体不走，这一步的产物也就没有用途，一并取消。
  裁剪本身仍可做，后来并进了 [第 07 章](07-SoC-IP定型与工具链升级.md) 的 SoC IP 定型。
- **`u_soc` 那 +63.5 XLR 与未归因的 +263.5 没有拆开。** 两者同源，
  都要在「同一版工具、同一份 IP」下重新生成才有意义。
  下一次因为别的理由重生成 SoC IP 时顺带做掉。
- **回归表只复跑了一项。** 本章只跑了 `sw/apps/probe/ddrAliasTest`（它直接验证 DDR 地址通路，
  与本章相关）。memTest、APB 探针、`ddrBurstProbe`、`bootSizeTest`、HDMI 未在本章复跑。
- **`doc/开发环境.md` 第 6 节要订正两处**：`cpu0.yaml` 的查找级数写的是 4，
  官方 cfg 里实际是 3；`debug.cfg` 与 `debug_ti.cfg` 的 `irlen` 不一致（4 对 5），
  本板要用 5。两处都已实测确认。
