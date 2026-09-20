# sw：本项目编写的 RISC-V 程序

与 `sdk/` 的分工：`sdk/` 是 IP 生成的 BSP 和官方示例，重新生成 SoC IP 时会被覆盖，**只读**；本目录放本项目编写的程序，不会被生成器碰到。
项目要往 sdk 加的东西一律放在 `bsp/` 这一层，由它转出去，不改 sdk 本身。

## 分层

`sdk/` 只读，项目自己的一侧分三层，**分层由编译结构保证**（各层的头文件路径），不靠约定：

```
sw/
  mk/target.mk     板上构建（xPack riscv-none-elf-gcc 13.4.0 + sdk 的 BSP）
  mk/host.mk       PC 构建（WSL 的 gcc + SDL2）
  bsp/             项目板级层：叠在只读的 sdk 之上
    include/       board.h（板级接口）、memmap.h（DDR 分区偏移与大小）、dcache_line.h（缓存行折算）
    memmap.c       分区表本体，两个平台共用
    ti60/          板上实现 + linker/gfx2d.ld
    host/          PC 实现：SDL2 显示与键盘，DDR 分区用一块内存模拟
  lib/             纯 C，两边都编，不依赖任何平台
    core/          CRC32、xorshift32、轻量格式化输出
    test/          主机侧单元测试（另含 CRC32 的独立参考实现）
    s3l/           第三方：small3dlib（CC0，header-only 的整数 3D 光栅器），见下
  apps/            应用，只写应用，按用途分三类（见下）
    game/          游戏与演示
    test/          回归判据
    probe/         探针与 benchmark
```

依赖方向只有一个：应用 → `lib/` 与 `bsp/include/board.h`，`board.h` 由 `bsp/ti60` 或 `bsp/host` 实现。
编 `lib/` 时只有 `-Isw/lib`，看不到 `board.h`；编应用时看不到 sdk——
应用里写 `#include <soc.h>` 会直接编不过。PC 在这套结构里就是「另一块板子」，
应用代码里不出现平台判断。判据 `Main/tools/check/check_sw_layers.sh` 会逐条验证这些。

## 应用的三类

按「跑完之后拿到的是什么」分：

| 目录 | 放什么 | 判断标准 |
| --- | --- | --- |
| `apps/game/` | 游戏与演示 | 给人看的，赛题要演示的那一类 |
| `apps/test/` | 回归判据 | 只给通过或失败（末尾 `fail 0`），平台或构建一改就要重跑 |
| `apps/probe/` | 探针与 benchmark | 给出数字或现象，服务于某个实验的结论；E7 的 `execSwTest`、E8 的 `entityBench` 建起来也放这里 |

| 程序 | 说明 |
| --- | --- |
| `apps/game/gfx2dGame/` | 游戏应用（SW1 起）。当前是最小骨架，判据见下 |
| `apps/game/cup3d/` | 纯软件 3D 光栅器在扫描输出的帧缓冲上转一只茶杯。三个用途：用真实负载量软件渲染的像素吞吐（E7 要的数）、让扫描输出跑在会动会遮挡的负载下、对外演示。320×240 光栅，1:1 贴在 640×480 的正中间。期望输出见下 |
| `apps/test/bootSizeTest/` | 检查 bootloader 从 Flash 拷贝的长度。带一个 300,000 字节、内容随位置变化的数据块，启动后打印整体与最后 4KB 的 CRC32 |
| `apps/test/cacheCoherencyTest/` | 实验 E3 的判据程序：用探针主机当外部写入者，一致性判据、注错与注错自检，并量失效指令的代价 |
| `apps/test/keyTest/` | SoC IP 定型的按键判据：空闲时 GPIO1 读口应为 `0x0f`，30 秒窗口内 KEY0~3 各要出现一次按下与松开（要人按） |
| `apps/test/displayTest/` | 扫描输出的板上判据，也是 `board_present`/`board_wait_frame` 的验证程序：IDENT、PATTERN（画定位图案上屏，这一段看屏幕）、FILL（整屏填充计时）、ANIM（120 帧移动方块，打帧率）、STATS（120 场快照）。走 SW1 的分层构建，产物 `build/displayTest-O2.bin`。期望输出见下 |
| `apps/probe/ddrAliasTest/` | 实验 E2：逐位翻转地址检查 DDR 地址译码与回绕，并演示数据缓存遮蔽别名的现象 |
| `apps/probe/ddrBurstProbe/` | 实验 E4、E5：经 APB 驱动 `axi_probe_master`，做 CPU 与第二个主机的交叉读、突发长度扫描、并发实验 |
| `apps/probe/cacheMechTest/` | 实验 E3 的机理实验 T1~T11：装行规则、写缓冲、缓存行几何、失效时机、被跳过的失效指令、失效与装行的代价 |
| `apps/probe/cacheMinTest/` | 实验 E3 早期的最小实验，只作现象可复现保留，不作为结论证据 |

除 `gfx2dGame` 以外的七个诊断程序仍沿用官方的 makefile 体系
（每个的 makefile 里用 `STANDALONE` 指到 `sdk/software/standalone`，现在是往上四级的 `../../../../sdk/...`），
**没有迁到新构建**，要改它们时顺手迁移。`gfx2dGame` 与以后的应用用新构建，
makefile 里 `include ../../../mk/$(PLATFORM).mk`。

2026-09-19 从 `sw/<程序>/` 搬到 `sw/apps/<类>/<程序>/`，七个程序搬迁前后的 bin 逐位相同。
TechReports 旧章节的配图里仍是搬迁前的路径（配图画的是该章结束时的状态，按规范不改）。

带 `logs/` 目录的程序，里面是与当前产物对应的上板原始日志（入库），文件名与 TechReport 里引用的一致。

## 编译两套

应用的 makefile 只写应用本身，平台由 `PLATFORM` 选（默认 `target`）：

```
# 板上：产物在 apps/game/gfx2dGame/build/，名字带优化级别，如 gfx2dGame-O2.bin
wsl -e bash -c 'cd sw/apps/game/gfx2dGame && make RISCV_BIN=<RISCV_BIN>'

# PC：产物 apps/game/gfx2dGame/build_host/gfx2dGame，跑起来开 640x480 窗口
wsl -e bash -c 'cd sw/apps/game/gfx2dGame && make PLATFORM=host && ./build_host/gfx2dGame'

# 也可以从 sw/ 目录直接编（产物落在 sw/build/）
wsl -e bash -c 'cd sw && make -f mk/host.mk'
wsl -e bash -c 'cd sw && make -f mk/target.mk RISCV_BIN=<RISCV_BIN>'
```

`<RISCV_BIN>` 是工具链前缀，取值见 `../doc/开发环境.md` 第 1、11 节。两套构建都用
`-std=gnu11 -Wall -Wextra -Werror`，PC 另加 `-Werror=double-promotion -Werror=float-conversion`，
所以「零告警」是编得过就成立，不需要人工看日志。板上构建的优化级别由 `OPT` 定（默认 `2`），
写进产物名；`make INJECT=XS32_SHIFT` 打开注错开关，产物名再加 `-inject`；
`EXTRA_CFLAGS=-DFRAMES=12000` 是留给一次性实验的口子。

改编译选项或改任何一个头文件都会触发重编：前者由目标目录里的编译选项标记文件保证，
后者由 gcc 的 `-MMD` 依赖文件（`.d`，makefile 末尾 `-include`）保证。
两条都进提交前检查（下面的第 6 项），因为它们坏掉时构建照样报成功、产物却是旧的。

## 主机侧单元测试与结构判据

```
wsl -e bash -c 'cd sw/apps/game/gfx2dGame && make PLATFORM=host test'    # 或 cd sw && make -f mk/host.mk test
wsl -e bash -c 'RISCV_BIN=<RISCV_BIN> bash Main/tools/check/check_sw_layers.sh'
```

前者把 `lib/test/` 在 64 位（挂 ASan+UBSan）与 `-m32` 下各编一份并比对输出，必须逐字节相同；
后者查七项：分层可见范围、同名目标文件、软浮点符号、`sdk/` 无改动、
**改了头文件必须重编并重链**、**缓存行区间折算**，其中四项带注错、两项带对照。
第 2、3、4、6 项跑的是**真实的板上构建**（临时 `OBJDIR`），不用主机 gcc 加手写的 `-I` 去模拟——
判据必须与被测的真实构建用同一组参数，理由见
[`../../Engineering/TechReports/sw/01-软件平台分层与双目标构建.md`](../../Engineering/TechReports/sw/01-软件平台分层与双目标构建.md) 4.6 节；
第 7 项跑主机探针，测的是 `bsp/include/dcache_line.h` 里那段真实的纯算术。
脚本需要交叉工具链，`RISCV_BIN=<RISCV_BIN>` 传进去。

## gfx2dGame 的期望值

**PC 版**：开一个 640×480 窗口，stdout 63 行，退出码 0：

```
gfx2dGame SW1
frame 0 crc cf8e2861 cycles 500652
...
frame 59 crc edf398b3 cycles 272305
frames 60
done
```

（`cycles` 每台机器、每次运行都不同，这里照实抄的是入库日志 `logs/pc_normal.log` 的首尾两行；
CRC 那一列才是判据。）

**板上**：gcc 13.4.0（2026-09-19 起）bin 2578 字节（`sha256:27a4157520558dbe…`，`-O2`），60 帧 CRC 与 PC 日志逐帧相同。此前 gcc 8.3.0 时 bin 2442 字节（`sha256:98fb37c9dc9bc060…`，xPack gcc 8.3.0，默认 `-O2`；
`-Os` 是 2210 字节 `8699eb87f4130ce5…`，注错版 2442 字节 `577cacb50a0b29bd…`），
串口 62 行，末尾 `frames 60` 与 `done`。板上不上屏（扫描输出属于 display 系列），
画面以整帧 CRC 的形式输出，**60 个 CRC 必须与 PC 逐帧相同**。
板上用的是 `Main/fpga/gfx2d_ti60/outflow/gfx2d_ti60.bit`
（2026-09-19 SoC IP 定型之后这一份 `sha256:4f1c7ec23d99f9ba…`，2,070,156 字节；此前 `d8af4ce13c416834…`）。

日志四份在 `apps/game/gfx2dGame/logs/`：`pc_normal.log`、`pc_inject.log` 是 PC 的 stdout（期望值），
`board_normal.log`、`board_inject.log` 是板上串口原始输出。比对只取 `frame N crc X` 这一列：

```
diff <(grep -o 'frame [0-9]* crc [0-9a-f]*' logs/pc_normal.log) \
     <(grep -o 'frame [0-9]* crc [0-9a-f]*' logs/board_normal.log)
```

注错版（`INJECT=XS32_SHIFT` 把 xorshift32 的第一个移位常数从 13 改成 12）从第 0 帧起就不同：
正常 `cf8e2861`，注错 `fe77d971`。板上日志开头几行是 Flash 里旧程序在 `reset halt` 之前的输出，
不是本程序的，比对时按上面的方式过滤。

`cycles` 那一列只有参考意义：PC 上是把性能计数器折算成 100MHz 的周期数，
不作性能结论（总览约束 5）；板上是 CLINT 的真实周期数，约 3270 万/帧。

## 编译（七个诊断程序）

2026-09-19 起 `sdk/` 是 efx_soc 3.4.0 生成的，工具链换成 xPack 的 gcc 13.4.0（与 IDE 2026.1 自带的同版本，Linux 版装在 WSL 里），安装见 `../doc/开发环境.md` 第 2 节：

```
wsl -e bash -c 'cd sw/apps/test/bootSizeTest && make BSP=efinix/EfxSapphireSoc RISCV_BIN=<RISCV_BIN> OBJDIR=build'
```

`<RISCV_BIN>` 是工具链前缀，取值见 `../doc/开发环境.md` 第 1、11 节。

- 必须写 `wsl -e`，否则命令会先经过一层 shell，`$` 开头的变量被提前展开成空，make 会退回用主机的 gcc。
- 在仓库里任意位置调用即可，`wsl.exe` 会把当前的 Windows 目录映射进去。
- `OBJDIR` 指到仓库外面，产物不进版本库。默认值是程序目录下的 `build/`，已在 `.gitignore` 中。
- 工具链前缀跟着 `sdk/` 的版本走：3.4.0 的软件包要 `riscv-none-elf-`（gcc 13.4），旧的 8.3.0 编不了。

## 上板运行

两种方式：

- 调试器加载：OpenOCD 连上后 `reset halt`，`load_image <bin> 0x1000 bin`，`resume 0x1000`。**加载前必须复位**，否则 CPU 取到的是指令缓存里的旧内容。**OpenOCD 要在 `sdk/bsp` 下启动**，在本目录的程序目录里启动会找不到 `cpu0.yaml`。
- 从 Flash 启动：把 bin 转成 hex（`tools/board/bin2hex.py`）写入 Flash `0x380000`，复位后由 bootloader 拷进 DDR。

两种方式的命令见 `../doc/开发环境.md` 第 6、8 节。

## bootSizeTest 的期望值

gcc 13.4.0 编译结果（2026-09-19）：bin 302,026 字节，数据块起于 `0x000017dc`、长 300,000 字节，`crc_all = 0x47df83a8`，`crc_tail = 0x78c8c48f`。
两个 CRC 只取决于数据块的内容，与 gcc 8.3.0 时相同（那时 bin 301,882 字节、数据块起于 `0x00001750`）。
对照组必须先跑：先跑 512KB 版会把完整镜像留在 DDR 里，重载比特流不给 DDR 断电，对照组会读到这份残留而假通过（2026-09-19 实际踩到，见 platform 第 07 章）。程序或工具链变化后要重新计算，构造与对照组设计见 `../../Engineering/TechReports/platform/01-从零搭起可上板的硬件基线.md` 3.5 节。

## ddrAliasTest 的期望值

gcc 13.4.0 编译，bin 2562 字节（`sha256:9d179bece4f1ff31…`，gcc 8.3.0 时 2418 字节），2026-09-19 上板输出与下面一致。串口逐行打印 `LOW k=0..27`、`LOW k=-1`、`HIGH k=28..31`、`CACHE_NOEVICT`、`CACHE_EVICT`，全部为 `ok`，最后一行之前是 `base 0x0c800000 fail 0`。其中 `CACHE_NOEVICT` 读到 `11`（缓存里的旧值）是预期现象，不计入 `fail`。实验设计与原始输出见 `../../Engineering/TechReports/platform/02-DDR地址译码与AXI-ID实测.md`。

## ddrBurstProbe 的期望值

需要带 `axi_probe_master`、`axi_mux_guard`（VERSION 2）与 `scanout_stub` 的比特流（2026-09-19 起）。
gcc 13.4.0 编译，bin 6602 字节（sha256 前 16 位 `b31871c65b8e3fea`；gcc 8.3.0 时 6322 字节 `ac1bcc7b4c04a26d`），`resume` 后约 3 秒输出完毕。
串口末尾为 `fail 0` 与 `done`，除下列各处外每行为 `ok`。2026-09-19 三轮的原始输出在 `apps/probe/ddrBurstProbe/logs/e5_v2_run_*.log`，
判据改前那次假红在 `e5_v2_mask_race.log`。

- `IDENT guard ctrl=3 mask=7 age=64 ok`：guard 的 ID、`VERSION == 2` 与上电默认值，加载旧比特流（VERSION 1）时这一行 FAIL；
  `IDENT scan id=0x5343414e base=0x02000000 ok`：扫描替身窗口。**配置归属这条判据就在这里。**
- `CROSS`、`SWEEP` 与 E4 相同，每行 `ok`；`SHARE errs=0x00000000 ok`。
- `SHARE cpu ticks alone=... shared=...`：CPU 拷贝在探针满负载读时约 **1.60 倍**（实测 53.9 ms 到 86.4 ms；E4 轮询基线约 1.94 倍）。
- `TRI rd` / `TRI wr`：三方并发（扫描替身 + 探针读或写 + CPU 拷贝）。CPU 拷贝 91.5 ms（1.70 倍）/ 103.2 ms（1.92 倍）；
  guard `max_wait_ar` 303 / 约 491、`max_wait_aw` 477 / 约 505、`hits` 约 17460 / 约 17540、`status=0`；
  扫描 `beat = txn × 160`、`skip=0`、`resperr=0`，最坏 `max_first` 78 / 约 362、`max_full` 244 / 约 528、`max_gap` 42 / 44。
  写方向时扫描首拍等待变长，来自控制器内部读写共用 DDR 端口（platform 第 06 章 5.3 节）。
- `INJECT age1_rd` / `age1_wr`：`AGE_LIMIT` 设 1，两个方向命中数都必须显著增加（实测 278537 / 278536）且 `probe_done`；
  `age64_*` 只记录（约 5600）。这一条只证明 guard 接到了真多路器，写方向 guard 不计为等待保证。
- `INJECT mask_s0 txn a b c d e ok`：清掉 guard 掩码的 S0 位。清位时在途的那一笔会照常完成，所以 `c - b` 最多为 1；
  之后必须冻住（`d == c`），恢复后继续增长（`e > d`）。实测 `14277 14906 14907 14907 15537` 与 `14278 14908 14908 14908 15539` 都判 ok。

周期数随板子会有约 2% 的波动；E4 各阶段的参考值与解释见
`../../Engineering/TechReports/platform/03-DDR3上的第二个主机与突发预算.md` 5.4 节，
E5 两个阶段的判据、实测值与解释见 `../../Engineering/TechReports/platform/06-Axi_Mux三端口仲裁与SoC防饿死保护.md` 5.3 节。
探针主机超时或挂死时 CPU 也访问不了 DDR，串口会停在最后一行，重新加载比特流恢复。

## cacheCoherencyTest 的期望值

需要带 `axi_probe_master` 的比特流。gcc 13.4.0（bin 5050 字节 `sha256:98b875e0d3baa9f5…`，2026-09-19 起；此前 gcc 8.3.0），默认编译档 `-Og`（`DEBUG=yes` 加 BSP 的 `DEBUG_OG?=yes`）。**加载前必须 `reset halt`**（`tools/board/board_run.py` 已包含）；不复位时探针挂住的坏状态会让程序一行都不跑，现象与程序有 bug 相同。

| 产物 | 编译 | bin 字节 | SHA256 前 16 位 | 末尾 | 日志 |
| --- | --- | ---: | --- | --- | --- |
| 正常版 | `OBJDIR=build` | 4850 | `a4c88d7b4f3fd078` | `fail 0` | `logs/normal_1.log` ~ `normal_3.log` |
| 注错版 | `OBJDIR=build_inject INJECT=1` | 4986 | `f3170f2da4a4c4a9` | `fail 0`，四行 `FAIL (inject)` | `logs/inject.log` |
| 注错自检版 | `OBJDIR=build_inject_selftest INJECT=2` | 4994 | `98287c7a6126d411` | `fail 4`，四行 `NOT_RED (inject)` | `logs/inject_selftest.log` |
| 正常版 `-O0` | `OBJDIR=build_o0 DEBUG_OG=no` | 9098 | `dfb5b2d3f767e29d` | `fail 0` | `logs/o0_normal.log` |
| 注错版 `-O0` | `OBJDIR=build_o0_inject DEBUG_OG=no INJECT=1` | 9146 | `974d605510654c6f` | `fail 0`，四行 `FAIL (inject)` | `logs/o0_inject.log` |

正常版每行 `ok`，A 组跑两遍所以 A 的行各出现两次：

- `P cpu_to_prb`：CPU 经别名放进去的值被探针读回；
- `A/B/C ... armed=1`：装行读到的确实是旧值，这一条为 0 时同组的判据作废；
- `A immediate`、`A after_loads`：读到旧值 `c0de00a1`，即缓存遮蔽了外部写入；
- `A invalidated_after`、`B invalidate_all`、`C invalidate_addr`：失效之后读到探针写入的新值；
- `D evicted`、`D word1`：驱逐之后读到新值，这是判据自检。

注错版变红的恰好是 A3 两行加 B、C 四行，A1、A2、D、P 与正常版相同。注错自检版保留失效调用、按注错模式判定，四行没变红就计入 `fail`，末尾必须是 `fail 4`；它证明注错宏没生效的产物不会得到 `fail 0`。

`TIME` 三行是失效代价，1 tick = 10 ns = 1 个 CPU 周期。三次正常版的 1000 次总量：空循环 13090~13104，全清 148044~148164（单次减空循环约 135.0 个周期），单地址 18094（约 5.0），全清加一次读 217707~217748（约 204.6）。这里的循环体写栈上的 `sink`，所以「全清」里含栈那一行重新装入的约 69 个周期，失效指令本身的代价看 `cacheMechTest` 的 T11。`probe_err` 一行都不应出现，出现即为探针异常并已计入 `fail`。

原始输出与结论见 `../../Engineering/TechReports/platform/05-CPU数据缓存与第二个主机写入的一致性.md` 第五节。

## cacheMechTest 的期望值

需要带 `axi_probe_master` 的比特流。gcc 13.4.0，`-Og`，bin 7018 字节（`sha256:4369e2ff4ae43938…`；gcc 8.3.0 时 6906 字节 `21dccb3fd0e8a20b…`）。每组一行数据加一行 `Tn ok`，末尾 `fail 0` 与 `done`（`logs/run_1.log`、`run_2.log`）。各组的期望：

- `T1 mismatch=0`；`T2 write_only=0 write_then_read=16 alias_then_read=16`；`T3 armed=16 stale_after_probe=16 probe_bad=0`；
- `T4 armed_mask=ffffffff evict_mask=fffff000 bad=0`；`T5 stale=8`；
- `T7`、`T8`、`T9` 每个数都是 8；
- `T10` 六个数依次是 `8 0 8 8 0 8`（跳过的失效指令紧跟分支时仍清缓存，隔 1 条或 2 条 `nop` 不清）；
- `T11` 只打印不判定，参考值：`empty=9016 all=76016 addr=13019 two_hits=10082`，`two_misses` 约 147900~147970；
- `T6 mcause=00000002` 两次（CSR `0x810` 不存在）。

启动行打印 `sp_row`（栈顶所在的缓存行号，当前是 43）。它若落进某组用的行号段，那一组可能被栈访问挤掉，要先换地址。

## cacheMinTest 的期望值

不需要探针主机。gcc 13.4.0，`-Og`，bin 2858 字节（`sha256:0182ca614d78cf86…`；gcc 8.3.0 时 2690 字节 `ea0556fddb1f214a…`）。串口逐行打印 `S1`、`S2`、`S3` 三组，末尾 `fail 0` 与 `done`（`logs/run_1.log`）：

- `S3 CACHE_NOEVICT` 按设计报 FAIL（读到 `22`），不计入 `fail`；`S3 CACHE_EVICT` 为 `ok`；
- `S1` 三项与 `S2` 三项都 `ok`。

**这个程序只作「现象可复现」保留，不作为判据。** `S3` 在别名写之前先读了 `Q ^ EVICT`，那一步本身就把 `Q` 的行换出去了，它的 FAIL 证明不了「写之后读一次不装行」。装行规则以 `cacheMechTest` 的 T2 为准：只写不装行，写之后读一次会装行。过程见 platform 第 05 章 4.3 节。

## displayTest 的期望值

需要带扫描输出（`u_display`）的比特流，2026-09-21 起的版本。gcc 13.4.0，`-O2`，
走 SW1 的分层构建（`make RISCV_BIN=<RISCV_BIN>`，产物 `build/displayTest-O2.bin`）。
末尾 `fail 0` 与 `done`：

- `IDENT id=0x44495350`（`"DISP"`）、`version=0x00000001`、`front_base=0x02000000`，三行 `ok`；
- `PATTERN ctrl=0x00000003 front=0x02000000 status=0x00000004`、`resperr=0x00000000 ok`；
- `ANIM frames=0x78`、`fps_x100=0x170c`（**59.9 fps**）、`swap_count=0x79`（与请求次数相等）；
  每帧的三段：`fill_per_frame` 约 `0x14c7b0`（1,361,837 个周期）、`present_per_frame` 约 `0x4470b`、
  画方块约 25,200；
- `STATS` 五行全 `ok`：`frame_beats_bad=0`、`end_level_bad=0`、
  `min_level_floor=0x15b`（347 拍，判据是不低于 160）、`zero_hits=0`、`underflow_beats=0`；
  同一行还打 `max_full`（约 `0xfe`~`0xff`）与 `max_first`（约 `0x55`~`0x59`）。

**`PATTERN` 那一段的判据是看屏幕**，串口只给寄存器侧的数：四角依次是红、绿、蓝、白，
最左四列是红、绿、蓝、白（正好一个 128 位拍里的 4 个像素），
再加一条黄色横线与一条青色竖线。颜色顺序反了说明字节序或拍内像素顺序有问题，
线的位置差一说明行跨或边界算错。

`FILL` 一段的三个数（`back2back_cyc`、`after_vsync_cyc` 与 `ANIM` 的 `fill_per_frame`）
是同一个 `fb_fill` 在不同位置量出来的，相差 58%，归因未定，见
`../../Engineering/Todo/hw/扫描输出.md` 第 4 节 2026-09-21 条与 E7。
判据只卡一个很宽的上界（`per_px_x100` 不超过 10000），不拿它当性能结论。

## 第三方：lib/s3l/small3dlib.h

上游是 small3dlib（作者 drummyfish，**CC0**，公有领域），header-only、纯整数、不用堆，
正好能在这颗没有 FPU 的 rv32im 上跑。本仓这一份相对上游只有一处改动：
第 1353 行起 `S3L_sin` 里加了一段 `#if defined(S3L_USE_LACC) && S3L_USE_LACC` 的分支，
那是另一个平台的硬件超越函数接口；本仓不定义这个宏，走的是上游的整数查表路径。
除此之外一字未改，要升级就整份换，改动只有这一处要重新加回去（或者一起去掉）。

它是 header-only，所以 `lib/` 下没有对应的 `.c`，应用直接 `#include "s3l/small3dlib.h"`。
按分层规则应用能看到 `-Isw/lib`，所以这条包含路径成立。

## cup3d 的期望值

需要带扫描输出的比特流。gcc 13.4.0，`-O2`，bin 20,394 字节。
串口三行加 `done`，**画面要看屏幕**：屏幕正中间 320×240 的区域里是一只有明暗的茶杯，
四周是黑的（边框只在启动时清一次，之后每帧只清画面那一块）。

```
cup3d tris=396 res=320x240
frames=240 cycles=47893e79 fps_x100=1900
per_frame clear=420436 shade=25224 raster=4083968 present=470801
done
```

2026-09-21 板上实测，每帧约 5,000,000 个周期 = 50 毫秒，**19 fps**。四段的占比：

| 段 | 周期 | 毫秒 | 占比 | 说明 |
| --- | --- | --- | --- | --- |
| 光栅 | 4,083,968 | 40.8 | **82%** | 396 个三角面，关了背面剔除所以内外壁都画 |
| 等切换 | 470,801 | 4.7 | 9% | `board_present` 等场起始 |
| 清画面 | 420,436 | 4.2 | 8% | 320×240 的颜色缓冲加 z-buffer |
| 着色 | 25,224 | 0.25 | 0.5% | 每帧 396 次点积，按面不按像素，可以忽略 |

这组数是软件渲染的第一份真实负载数据，比 `displayTest` 的纯填充更有参考价值，
E7 的条目里引了它。**瓶颈在光栅不在访存**：着色只占 0.5%，清屏加上屏合起来也只有 17%。
要提速先动光栅（背面剔除、缩小覆盖面积、减少过绘），而不是先动分辨率。
