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
    core/          CRC32、xorshift32、轻量格式化输出、Q.8 定点与正弦表（SW7）、帧时间直方图与日志队列（SW8）
    gfx/           命令表构造层（含批量提交 gfx_blit_run，SW7）、软件执行器、十一个整帧用例（命令 ABI v1）、按方程的普查（SW8）
    test/          主机侧单元测试（另含 CRC32 与软件执行器的独立参考实现、两组一致性向量）
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
| `apps/probe/` | 探针与 benchmark | 给出数字或现象，服务于某个实验的结论；E7 的 `execSwTest`、E8 的 `entityBench` 已在这里 |

| 程序 | 说明 |
| --- | --- |
| `apps/game/gfx2dGame/` | 基础 Demo（SW6，赛题基础要求 3）：场景 A 大量方块随机移动、场景 B 三张整屏图快速切换，每帧一条命令表交给软件执行器，双缓冲呈现。默认编**演示模式**（实时输入），`EXTRA_CFLAGS=-DSW6_JUDGE=1` 编**判据模式**（回放两段入库录像、逐帧打日志）。串起来跑的是 `Main/tools/check/check_sw6_demo.sh`。期望输出见下。另有 SW7 游戏本体的代码逐步放进 `src/game/stg_*`（A 批是实体池 `stg_pool`；B 批加了弹种表 `stg_kind`、发射器 `stg_emit`、五种弹型 `stg_pattern` 与弹幕场 `stg_field`），它们的主机测试在 `test/`，由 `Main/tools/check/check_sw7_game.sh` 编译运行；弹型判据的期望文件 `test/stg_patterns.txt` 由 `Main/tools/check/gen_stg_patterns.py` 生成。`EXTRA_CFLAGS=-DSTG_FIELD=<n>` 换成弹幕场的入口（1 演示、2 判据、3 `N_max` 搜索、4 游戏的判据入口），见下「弹幕场入口」。 |
| `apps/game/cup3d/` | 纯软件 3D 光栅器在扫描输出的帧缓冲上转一只茶杯。三个用途：用真实负载量软件渲染的像素吞吐（E7 要的数）、让扫描输出跑在会动会遮挡的负载下、对外演示。320×240 光栅，1:1 贴在 640×480 的正中间。期望输出见下 |
| `apps/test/bootSizeTest/` | 检查 bootloader 从 Flash 拷贝的长度。带一个 300,000 字节、内容随位置变化的数据块，启动后打印整体与最后 4KB 的 CRC32 |
| `apps/test/cacheCoherencyTest/` | E3 回归（**B6 改写版**）：引擎当外部写入者（FILL COPY）与第二个读者（BLIT），P/A/B/C/D 五组判据、注错与注错自检。B6 前的探针版（外部写入者 = `axi_probe_master`）随 D22 撤探针停用，机理实验归档 platform 第 05 章，改写前的最后一份基线日志 `logs/b6_preintegration_20261002.log`。**target-only**，target.mk 构建，产物 `build/cacheCoherencyTest-O2.bin`（注错 `build_inject/`、自检 `build_inject_selftest/`）。期望输出见下 |
| `apps/test/engineTest/` | 引擎 P1 B6 的板上判据：同一张命令表软件执行器画一份、引擎画一份逐行 CRC 比对（SW3 十一个整帧金标准 + 几何定向与写掩码 + 单缓存行失效判据 INJ-COH），PERF 弹幕性能与 E5 复测（扫描 FIFO 水位/欠载/guard 等待）。**target-only**，target.mk 构建，产物 `build/engineTest-O2.bin`（注错 `build_inject/`）。期望输出见下 |
| `apps/test/keyTest/` | SoC IP 定型的按键判据：空闲时 GPIO1 读口应为 `0x0f`，30 秒窗口内 KEY0~3 各要出现一次按下与松开（要人按） |
| `apps/test/displayTest/` | 扫描输出的板上判据，也是 `board_present`/`board_wait_frame` 的验证程序：IDENT、PATTERN（画定位图案上屏，这一段看屏幕）、FILL（整屏填充计时）、ANIM（120 帧移动方块，打帧率）、STATS（120 场快照）。走 SW1 的分层构建，产物 `build/displayTest-O2.bin`。期望输出见下 |
| `apps/test/platformTest/` | PC 平台层（SW4）的判据程序：十一个整帧用例、600 帧帧节拍、掉帧注错、实时输入与关窗退出、录制的往返与两次回放。**一段要人动手**（按 F1~F3、点关闭按钮），其余自己判并打印 `fail N`。默认编 PC 版（`make` 即 `PLATFORM=host`）；串起来跑的是 `Main/tools/check/check_sw4_pc.sh`。期望输出见下 |
| `apps/test/assetTest/` | 素材流水线（SW5）的判据程序：经板级读入口把素材包装进 DDR 素材区、完整校验、与生成的 `assets.h` 对账、挑条目登记表面，再用软件执行器画一整帧并打印整帧 CRC 与一张 64×24 字符画。PC 与板上跑同一份源码、同一份包，整帧 CRC 必须相同；默认编 PC 版（`make`），板上是 `make PLATFORM=target RISCV_BIN=<RISCV_BIN>`。串起来跑的是 `Main/tools/check/check_sw5_assets.sh`。期望输出见下 |
| `apps/probe/ddrAliasTest/` | 实验 E2：逐位翻转地址检查 DDR 地址译码与回绕，并演示数据缓存遮蔽别名的现象。E8 复核把 `rd8_fresh` 从单次异标签读改为按地址失效（SDK 指令覆盖所有可能路）；2026-09-27 修改版已在 4KB/16KB 各完整运行一次，均 `fail 0`/`done`。**依赖 S1 探针，B6 撤探针（D22）后需要 B6 前的比特流才能跑** |
| `apps/probe/ddrBurstProbe/` | 实验 E4、E5：经 APB 驱动 `axi_probe_master`，做 CPU 与第二个主机的交叉读、突发长度扫描、并发实验（E5 的 SHARE/TRI/INJECT 阶段）。E8 复核把只适用单路的读后驱逐改为按地址失效，2026-09-27 修改版已在 4KB/16KB 各完整运行一次，均 `fail 0`/`done`。**依赖 S1 探针，B6 撤探针（D22）后需要 B6 前的比特流才能跑** |
| `apps/probe/execSwTest/` | E7 固定软件基线已于 2026-09-27 收尾。生产版 36,090 字节、SHA256 `81f859988862e66489884b19302743ec38983539d75a1ec0a49f5a45c9cf632d`，精确 S1 约 805 ms；生产数字仅对应该固定产物。本次 VERIFY_ONLY 的 ALPHA 正常判分与错配自检均为 26,370 字节，分别 `fail 0` 与 `fail 8`，原始 CRC 独立核对通过。开关已实装，注错收尾不会执行 THR/S1/OVH。全部历史记录、构建哈希和维护边界见 [platform 第 09 章](../../Engineering/TechReports/platform/09-软件执行器像素吞吐的板上复现.md) |
| `apps/probe/entityBench/` | E8 实体更新与合法命令记录微基准，**2026-09-27 已收尾**，本次 4KB/16KB 使用同一份产物。九组：A（AoS 纯更新）、B（SoA 纯更新）、C（SoA 更新并提交，N=1000/2000/5000/10000）、D（SoA 纯更新，N=2000/5000/10000）。当前 `build/entityBench-O2.bin` **69,034 字节**、SHA256 `b0f2bbc5100b24702b750cca803ce5c94c34c8f81aee68372317f35381184e29`，gcc 13.4.0、`-O2`；计时外再运行 1000 帧，对每帧实体六字段及 C 组全部 `32×N` 命令字节与主机期望摘要校验。新产物 4KB、16KB 各九组全为 `state_bad 0 cmd_bad 0`、`fail 0`；历史日志 `logs/board_framecheck_{4k,16k}_20260925.log`，本次每档一次的日志 `logs/closeout_20260927_{4k,16k}.log`，新旧同产物最大变化 <0.08%。旧版 8KB 仅作历史对照，不补新版矩阵；候选采用与维护见 platform 第 10 章第六节 |
| `apps/probe/displayFifoProbe/` | 实验 E5 2.4 节：真实扫描 FIFO 的压力矩阵与深度定值。`idle` / 满负载 `read` / 满负载 `write` 三组各收 120 场连续快照，报最低水位的最小值、最大值、首次最小值帧号与两个中位数。**依赖 S1 探针造负载，B6 撤探针（D22）后需要 B6 前的比特流才能跑**；引擎负载下的水位复测由 `engineTest` 的 STRESS 段承担（判据：最低水位 ≥ 160 拍、欠载 0）。**target-only**，产物 `build/displayFifoProbe-O2.bin`。期望输出见下 |
| `apps/probe/cacheMechTest/` | 实验 E3 的机理实验 T1~T11：装行规则、写缓冲、缓存行几何、失效时机、被跳过的失效指令、失效与装行的代价。**依赖 S1 探针，B6 撤探针（D22）后需要 B6 前的比特流才能跑**；机理结论已归档 platform 第 05 章 |
| `apps/probe/cacheMinTest/` | 实验 E3 早期的最小实验，只作现象可复现保留，不作为结论证据 |

除 `gfx2dGame` 以外的七个诊断程序仍沿用官方的 makefile 体系
（每个的 makefile 里用 `STANDALONE` 指到 `sdk/software/standalone`，现在是往上四级的 `../../../../sdk/...`），
**没有迁到新构建**，要改它们时顺手迁移。`gfx2dGame`、`cup3d`、`displayTest`、`platformTest`
与 `displayFifoProbe` 用新构建，makefile 里 `include ../../../mk/$(PLATFORM).mk`；
`platformTest` 是 PC 侧的判据程序，`PLATFORM` 默认取 `host`（其余应用默认 `target`），
`displayFifoProbe` 是板上实验工具，直接 include `target.mk`，不走 `PLATFORM` 二选一。

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

**素材（SW5）：** 两个 `mk/*.mk` 给**应用**那一层的编译规则加
`-I$(ASSETS_BUILD)`（默认 `Main/assets/build`，由 `tools/assets/pack_assets.py` 生成
`assets.h` 与 `assets.pak`）；`lib/` 与 `bsp/` 那一层不加，分层不变。
PC 的素材包路径由 `mk/host.mk` 的 `ASSETS_PAK` 编进 `bsp/host`（取绝对路径），
运行时可以用 `GFX2D_ASSETS_PAK=<文件>` 覆盖——判据脚本用它喂坏包。
`ASSETS_PAK` 进了编译选项标记文件，换路径会触发重编（否则 `board.o` 会留着上一个路径）。

**录像（SW6）：** 应用在 makefile 里写 `REPLAYS = sw6_a sw6_b`，
两个 `mk/*.mk` 各有一条「`<应用>/replay/<名字>.g2dr` → `$(OBJDIR)/replay/<名字>.h`」的规则
（调 `tools/check/replay2c.py`），给**应用**那一层加 `-I$(OBJDIR)/replay`，
应用 `#include "sw6_a.h"` 取到 `<名字>_bytes` 与 `<名字>_LEN`。
`.g2dr` 是源数据、入库；转出来的 C 数组是构建产物、随 `build*/` 一起不入库
（板上没有文件系统，`lib/core/replay.h` 定这个格式就是为了这一步）。
两个 `mk` 的默认块（`ifeq ($(APP),)` 那一段）也把 `LIBS` 写成 `core gfx` 并补上 `REPLAYS`，
`make -f mk/host.mk`、`make -f mk/target.mk` 这两条直接调用的路子才编得过。
所以 gfx2dGame 的构建现在要 `python3`（`replay2c.py` 与素材打包都要它）。

## 主机侧单元测试与结构判据

```
wsl -e bash -c 'cd sw/apps/game/gfx2dGame && make PLATFORM=host test'    # 或 cd sw && make -f mk/host.mk test
wsl -e bash -c 'RISCV_BIN=<RISCV_BIN> bash Main/tools/check/check_sw_layers.sh'
```

`lib/test/` 的判据分五组，都加在 `test` 目标上：

- `lib/gfx/` 的三个开关：

```
# 加上工具导出的 159 条一致性向量（生成物不入库，先跑一次工具）
wsl -e bash -c 'cd <REPO> && python3 Main/tools/check/check_cmd_semantics.py --emit /tmp/vec.txt'
wsl -e bash -c 'cd sw && make -f mk/host.mk test VEC=/tmp/vec.txt'

# 逐方程注错：用到该方程的整帧用例 CRC 必须变，其余必须不变
wsl -e bash -c 'cd sw && make -f mk/host.mk test INJECT=EQ_COPY'   # EQ_ALPHA / EQ_ADD / EQ_CONST / EQ_MUL / EQ_ALPHA_STRAIGHT

# 重新生成整帧用例的期望 CRC（只在用例或规格有意改动之后跑）
wsl -e bash -c 'cd sw && make -f mk/host.mk gen-golden'
```

- 录像格式那一组（[sw 第 03 章](../../Engineering/TechReports/sw/03-PC平台层与帧节拍.md) 5.2 节的第 1、7 条判据）不用开关：`test` 目标先跑
  `python3 Main/tools/check/replay2c.py`，把入库的 `lib/test/replay_fixture.g2dr`
  转成 C 数组放进 `$(OBJDIR)/test/`，再与直接解析 `.g2dr` 的结果逐帧对账。
  **脚本进判据路径而不是一次性生成器**，这样这条判据测的是转换本身。它需要 `python3`。

- 数值基础那一组（`lib/test/test_fixmath.c`，[sw 第 06 章](../../Engineering/TechReports/sw/06-游戏本体.md) 3.4 节）不用开关：
  拿 libm 的双精度 `sin`、`atan2` 当参考，所以 `test` 目标链 `-lm`；双精度只出现在测试里。
  正弦表 `lib/core/sin_tab.c` 是 `Main/tools/check/gen_sin_tab.py` 的产物，不手改。
  四个注错开关各让指定的行见红：

```
wsl -e bash -c 'cd sw && make -f mk/host.mk test INJECT=FX_IDX_TRUNC'   # FX_VEL_Q4 / FX_ATAN_NOSCALE / FX_ATAN_UNIT
wsl -e bash -c 'bash Main/tools/check/check_sw7_game.sh'                # 重生成正弦表比对、64/32 比对、四个注错
```

- 批量提交那一组（`lib/test/test_blit_run.c`，sw 第 06 章 3.6 节）不用开关：随机 1000 组，`gfx_blit_run()` 与逐条
  `gfx_blit()` 的缓冲区与计数逐字节相同；另有 200 组在两次调用之间改写表面表。
  注错 `INJECT=BLIT_RUN_W7`、`BLIT_RUN_SXY`、`BLIT_RUN_NO_FALLBACK`，由 `check_sw7_game.sh` 的 G6 跑。

- 普查那一组（`lib/test/test_census.c`，[sw 第 07 章](../../Engineering/TechReports/sw/07-演示模式与统计埋点.md) 3.2 节）不用开关：
  `lib/gfx/census` 按方程数出的条数与面积，在十一个整帧用例与随机合法命令表上与软件执行器的 `cmd`、`pixel_total` 对账，
  注错 `INJECT=CENSUS_NO_SETCLIP`、`CENSUS_NO_SURF`。
  同一文件里还有模拟时钟（`gfx_cost_cycles`，sw 第 07 章 3.4 节）的手算样例与单调性，注错 `INJECT=COST_NO_B`。
  帧时间直方图那一组在 `lib/test/test_stats.c`（`lib/core/framehist`，sw 第 07 章 3.4 节），注错 `INJECT=FHIST_P99_LOWER`、`FHIST_DROP_OVER`；
  同一文件里还有日志队列（`lib/core/logq`，sw 第 07 章 3.3 节）对一份线性参考的随机比对，注错 `INJECT=LOGQ_SPLIT`、`LOGQ_WRAP`。
  `board.h` 的 `board_tx_room()`（sw 第 07 章 3.3 节）PC 实现要 SDL，进不了 `lib/test`，由主机探针
  `Main/tools/check/check_sw8_tx.c` 链真实的 `bsp/host/board.c` 测：PC 上默认不限，
  设 `GFX2D_TX_BUDGET=N` 时按场补满额度、`board_puts()` 从额度扣，退出时在 stderr 打超额汇总。
  `N_max` 搜索的状态机（`lib/core/nmax`，sw 第 07 章 3.5 节）在 `lib/test/test_nmax.c`：综合负载下对解析解、单调、下界、上限、
  涨潮超时、确认期尖峰与重试用完，注错 `INJECT=NMAX_BISECT_SIDE`、`NMAX_NO_STEPBACK`、`NMAX_NO_SETTLE`。
  SW8 的判据串起来跑的是（`RISCV_BIN` 给了才做板上构建那一项，跳过不算通过）：

```
wsl -e bash -c 'RISCV_BIN=<RISCV_BIN> bash Main/tools/check/check_sw8_stats.sh'
```

不给 `VEC` 时只跳过那一组，其余照跑：导出向量是生成物、不入库，不能当默认判据的前置。
`lib/test/golden.txt` 是十一个整帧用例的 CRC32 期望值，入库，E7 与引擎仿真拿它当金标准；
它变了就是结论变了，不是噪声。

前者把 `lib/test/` 在 64 位（挂 ASan+UBSan）与 `-m32` 下各编一份并比对输出，必须逐字节相同；
后者查七项：分层可见范围、同名目标文件、软浮点符号、`sdk/` 无改动、
**改了头文件必须重编并重链**、**缓存行区间折算**，其中四项带注错、两项带对照。
第 2、3、4、6 项跑的是**真实的板上构建**（临时 `OBJDIR`），不用主机 gcc 加手写的 `-I` 去模拟——
判据必须与被测的真实构建用同一组参数，理由见
[`../../Engineering/TechReports/sw/01-软件平台分层与双目标构建.md`](../../Engineering/TechReports/sw/01-软件平台分层与双目标构建.md) 4.6 节；
第 7 项跑主机探针，测的是 `bsp/include/dcache_line.h` 里那段真实的纯算术。
脚本需要交叉工具链，`RISCV_BIN=<RISCV_BIN>` 传进去。

## gfx2dGame 的期望值

SW6 起它是**基础 Demo**（赛题基础要求 3），SW8 C 批起默认构建是**模式框架**（启动横幅 `gfx2dGame MODES`）：
三个模式 1 基础 Demo（SW6 的演示：实时输入，A／B 两个阶段每 600 场自动轮换，HUD 右上角显示 SW FPS、N／k、命令数、
写入像素数与三段耗时）、2 游戏（SW7）、3 压力测试（终符上的 `N_max` 搜索，模拟时钟或板上真实时钟）。
上电进 `-DGFX2D_START_MODE=<1|2|3>`（缺省 2）。PC 上数字键 1~3 直接选模式、Tab 切后端标签；板上切换的是真实执行器。
板上用和弦：
上 + 下按住 8 帧切下一个模式，左 + 右按住 8 帧切后端，松开之后才会再触发。模式 2 里连续 600 帧无按键时自动驾驶接管，
任何按键立即交还。每次进入一个模式，第一帧之后打一行
`mode <m> enter frame <帧号> crc <整帧 CRC32> pool <四个池> clip <裁剪框>`。
每 60 场打一行统计 CSV（`csvh,` 表头、`csv,` 每秒一行、结束时 `csvsum,` 汇总，列见 sw 第 07 章 3.9 节）；
这些行与框架自己的行先进日志队列，每帧只取出 `board_tx_room()` 那么多字节（模式 1 里 SW6 的逐帧日志照旧直接打）。
`-DSW6_JUDGE=1` 是**判据模式**：不走模式框架，不读实时按键，按入库的两段录像各回放两遍，逐帧打日志与入库期望值对账。

判据、日志格式与逐帧契约见
[`../../Engineering/TechReports/sw/05-基础Demo.md`](../../Engineering/TechReports/sw/05-基础Demo.md)；
跑全部十项判据用 `Main/tools/check/check_sw6_demo.sh`。

```
# 模式框架：窗口里玩游戏（方向键、左 Shift 低速、空格炸弹），1/2/3 切模式
wsl -e bash -c 'cd sw/apps/game/gfx2dGame && make PLATFORM=host && ./build_host/gfx2dGame'

# 上电直接进 SW6 的演示（模式 1）
wsl -e bash -c 'cd sw/apps/game/gfx2dGame && make PLATFORM=host OBJDIR=build_host_m1 EXTRA_CFLAGS=-DGFX2D_START_MODE=1 && ./build_host_m1/gfx2dGame'

# 模式 3 两遍搜索跑完就退出（模拟时钟，无窗口不等场；本机约 6 分半）
wsl -e bash -c 'cd sw/apps/game/gfx2dGame && make PLATFORM=host OBJDIR=build_host_m3 EXTRA_CFLAGS="-DGFX2D_START_MODE=3 -DGFX2D_PRESS_EXIT=1" && SDL_VIDEODRIVER=dummy GFX2D_UNPACED=1 ./build_host_m3/gfx2dGame'

# 切换判据：合成按键来回切 150 次，每行 mode ... enter 与冷启动的第一帧比（check_sw8_stats.sh 的 M10）
wsl -e bash -c 'cd sw/apps/game/gfx2dGame && make PLATFORM=host OBJDIR=build_host_swj EXTRA_CFLAGS=-DGFX2D_SWITCH_JUDGE=1 && SDL_VIDEODRIVER=dummy GFX2D_UNPACED=1 ./build_host_swj/gfx2dGame'

# 日志不阻塞长跑：模式 2、36000 帧、每场 192 字节的串口额度，按 60 Hz 节拍约 10 分钟（check_sw8_stats.sh 的 M14）
wsl -e bash -c 'cd sw/apps/game/gfx2dGame && make PLATFORM=host OBJDIR=build_host_long EXTRA_CFLAGS="-DGFX2D_START_MODE=2 -DGFX2D_FRAMES=36000" && SDL_VIDEODRIVER=dummy GFX2D_TX_BUDGET=192 ./build_host_long/gfx2dGame | grep ^csv'

# 自动驾驶打一局（游戏的判据入口，受击打开）：逐帧 g 行、每段 seg 行，末尾 auto 行与 fail 0（check_sw8_stats.sh 的 M16）
wsl -e bash -c 'cd sw/apps/game/gfx2dGame && make PLATFORM=host OBJDIR=build_host_auto EXTRA_CFLAGS="-DSTG_FIELD=4 -DSTG_JUDGE_AUTO=1" && SDL_VIDEODRIVER=dummy GFX2D_UNPACED=1 ./build_host_auto/gfx2dGame | grep -E "^(seg|auto|fail)"'

# 回放工具辅助的通关录像 replay/stg_clear.g2dr（游戏的判据入口，受击打开）：9208 帧打到 CLEAR，
# 末尾 clear、end 两行与 Main/tools/check/gen_stg_clear.c --verify 的摘要同格式（check_sw7_game.sh 的 G16）
wsl -e bash -c 'cd sw/apps/game/gfx2dGame && make PLATFORM=host OBJDIR=build_host_rep EXTRA_CFLAGS="-DSTG_FIELD=4 -DSTG_JUDGE_REPLAY=1" && SDL_VIDEODRIVER=dummy GFX2D_UNPACED=1 ./build_host_rep/gfx2dGame | grep -E "^(seg|clear|end|fail)"'

# S1 规模：模式 3 不搜索、16x16、旋钮固定 1744（约 2040 颗敌弹），窗口里看；统计 CSV 照打（check_sw7_game.sh 的 G17）
wsl -e bash -c 'cd sw/apps/game/gfx2dGame && make PLATFORM=host OBJDIR=build_host_s1 EXTRA_CFLAGS="-DGFX2D_START_MODE=3 -DGFX2D_PRESS_KNOB=1744" && ./build_host_s1/gfx2dGame'

# 判据模式全量一遍：约 22 秒、1226 帧，末尾 fail 0 与 done
wsl -e bash -c 'cd sw/apps/game/gfx2dGame && make PLATFORM=host EXTRA_CFLAGS=-DSW6_JUDGE=1 && ./build_host/gfx2dGame'

# 判据模式快速档（每段每遍 120 帧，A 段仍覆盖 N=5000）
wsl -e bash -c 'cd sw/apps/game/gfx2dGame && make PLATFORM=host EXTRA_CFLAGS="-DSW6_JUDGE=1 -DSW6_FRAMES=120" && ./build_host/gfx2dGame'

# 十项判据串起来（录像可重生成 → 编过跑通 → 对期望日志 → 有窗无窗 → 两遍 → HUD → 场景 B
#                  → 计数 → 四个方程的注错见红 → Sanitizer 与板上目标构建）
# 不传 RISCV_BIN 时板上目标构建会跳过（工具链不在 PATH 上），跳过不算通过
wsl -e bash -c 'RISCV_BIN=<RISCV_BIN> bash Main/tools/check/check_sw6_demo.sh'
wsl -e bash -c 'RISCV_BIN=<RISCV_BIN> SW6_QUICK=1 bash Main/tools/check/check_sw6_demo.sh'
```

`logs/` 下这一条目的日志：

| 文件 | 是什么 |
| --- | --- |
| `sw6_judge.log` | 判据模式**全量**一遍的期望值：1236 行（`gfx2dGame SW6`、`pak`、三行 `scene`、`eq P`、A 两遍各 246 帧、B 两遍各 367 帧、两行 `eq A`、`fail 0`、`done`） |
| `sw6_judge_quick.log` | 快速档（`-DSW6_FRAMES=120`）的期望值；它的逐帧行按段、按遍必须是全量档的前缀 |
| `sw6_board_quick.log` | 板上判据模式快速档的串口原始输出（2026-09-28，产物 77,158 字节、sha256 前 16 位 `cb5af521cc1d6a34`，素材由程序从 Flash 读 `pak 2840320 b370ba0a`）：`fail 0`，从 `pak` 行起 489 行去掉 `cyc` 后与 `sw6_judge_quick.log` 逐行相同 |
| `sw8_stats_demo.log`、`sw8_stats_game.log`、`sw8_stats_s1.log`、`sw8_stats_press.log` | SW8 第一版统计与出图的四份样例（PC、无窗口、按 60 Hz 节拍）：模式 1 的 1300 帧、模式 2 无人操作 5400 帧、S1 规模（`GFX2D_PRESS_KNOB=1744`）1800 帧、模式 3 两遍搜索；`Main/tools/stats/plot_stats.py` 由它们画出 `Deliverables/Assert/figures/sw8_*.png`，录制开关见 sw 第 07 章 3.11 节。CSV 里的帧时间与周期数是 PC 的墙钟，只验证流程 |
| `pc_normal.log`、`pc_inject.log`、`board_normal.log`、`board_inject.log` | **SW1 骨架的旧日志**，原地保留：它们是 60 帧图案那个版本的产物，当前二进制不再产生它们，**不要拿它们当期望值**。SW1 的记述在 [sw 第 01 章](../../Engineering/TechReports/sw/01-软件平台分层与双目标构建.md) |

固定值（`logs/sw6_judge.log` 的头几行与第 0 帧）：

```
gfx2dGame SW6
pak 2840320 b370ba0a
scene 0 scrc d90fb7b2
scene 1 scrc 70fd3e3d
scene 2 scrc 0211c146
eq P 53 18 0 2
f 0 A p 1 s - keys 00000000 n 100 k 0 cmd 159 pix 391496 ksk 5935 hud - - off 4 cyc 27 497 176435 crc 8295c1f0 scrc 62772be8 hcrc ecc28265
...
```

- `cyc` 三列是 `board_cycles()` 的差值，每台机器、每次运行都不同，**比对前必须去掉**；
  其余每一列都是确定的。
- A 段第 106 帧起 `n 5000`；N=5000 时 `cmd` 约 5067（每张命令表上限 32768 条）。
- `scene i` 是三张离屏图预合成后的参考值；B 段每个 `f` 行的 `scrc` 等于它本帧 `s` 那一张。
- `scrc` 是**排除 HUD 矩形**（`x=400..639, y=0..159`）后的场景区 CRC，`crc` 是整帧，
  `hcrc` 是 HUD 矩形本身；三者都逐行喂、跳过行跨间隙，口径见 sw 第 05 章 3.11 节。
- 判据模式下 HUD 的实时量（`SW FPS` 与三段耗时）显示 `--`：实测值进画面，
  同一份输入两次就跑不出相同的整帧 CRC。演示模式显示实测值。

**板上**（xPack 的 `riscv-none-elf-gcc` 13.4.0 装在 WSL 里、不在 PATH 上，
`<RISCV_BIN>` 的取值见 `../doc/开发环境.md` 第 11 节；`board_run.py` 的解释器设置见 `../tools/board/README.md`）：

```
wsl -e bash -c 'cd sw/apps/game/gfx2dGame && make RISCV_BIN=<RISCV_BIN> OBJDIR=build_board_quick EXTRA_CFLAGS="-DSW6_JUDGE=1 -DSW6_FRAMES=120"'
<EFINITY>\python311\bin\python.exe tools\board\board_run.py --bin sw\apps\game\gfx2dGame\build_board_quick\gfx2dGame-O2.bin --stop-on done --seconds 2700 --log <串口日志>
wsl -e python3 Main/tools/check/check_sw6_log.py --board <串口日志> Main/sw/apps/game/gfx2dGame/logs/sw6_judge_quick.log
```

判据模式**不读 `board_keys()`**，所以板上不需要接按键就能跑。程序用 `--bin` 经 JTAG 写进 DDR 运行，
素材包照样由程序从 Flash 素材分区读，所以 Flash 里要有与 `assets.h` 同一版的包（`pak` 行会核对）；
不用 `--flash-boot`，因为它跑的是 Flash 应用区里固化的程序（现在是 SW5 的 `assetTest`），不是这次的产物。
`--board` 从最后一个 `gfx2dGame SW6` 横幅之后的 `pak` 行起比，`board_run.py` 的 `#` 标记行与
Flash 旧程序在 `reset halt` 之前的输出都跳过。这一步是 SW9 之前唯一一次「RV32 上的软件执行器 +
素材装载 + 整条组帧路径与 PC 等价」的证据；快速档在板上一次约 16 分钟。
板上软件执行器每帧执行 0.4~4.4 秒，加上三遍 CRC 与一行串口，一帧 1~5 秒，演示模式一段 600 帧在板上要十分钟以上，
所以演示模式的 HDMI 画面与软硬件帧率对比留给 SW9（引擎接上之后）。

### 弹幕场入口（SW7 B 批）与游戏的判据入口（C 批）

`EXTRA_CFLAGS=-DSTG_FIELD=<n>` 编出 SW7 B 批的弹幕场（五种弹型、静止自机；C 批起用游戏的霓虹素材与表面槽）或游戏的判据入口，
启动横幅换成 `gfx2dGame STG`；不给这个开关时仍是上面的 SW6，行为与日志不变。口径见
[sw 第 06 章](../../Engineering/TechReports/sw/06-游戏本体.md) 3.7、3.9 节。

| n | 是什么 |
| --: | --- |
| 1 | 演示：圆环、螺旋、自机狙、扇形、随机雨各 300 帧轮流，再是五个一起的合并段 600 帧，循环；每 60 帧一行 `d`（活动元素数与三段周期数） |
| 2 | 判据：不读按键，五段各 120 帧、合并段 240 帧，每段一行 `seg`、每帧一行 `f`（帧号、段号、四个池的存活数、活动元素数、本段累计的命中与擦弹、游戏区 400×480 的 CRC），末尾 `fail`、`done`；每帧自查追加的拒绝数为 0、普查的 ALPHA 条数等于发出的敌弹数 |
| 3 | `N_max` 搜索（sw 第 07 章 3.6 节）：无窗口、不执行命令表、不呈现，只做普查与模拟时钟，16×16 与 8×8 两种负载各搜一遍，打 `nmax cfg`、逐档 `nmax probe` 与 `nmax result`；结果不是 `OK` 记一次 fail。PC 上约 1.5 秒 |
| 4 | 游戏的判据入口（sw 第 06 章 3.13 节）：输入是固定的按键脚本（`src/game/stg_script.c`），受击默认关掉（`-DSTG_JUDGE_HITS=1` 打开），跑完一局回到标题为止（约 10,185 帧）。每段一行 `seg <帧号> <段名>`，每帧一行 `g`（阶段、段号、三个池的存活数、五类活动元素、残机、炸弹、得分、擦弹、整帧 640×480 的 CRC），末尾 `fail`、`done`；每帧自查追加的拒绝数为 0、普查的各方程条数等于组帧记下的。HUD 的实时量一律 `--` |

```
# 演示：窗口里看五种弹型轮流（绿色方块是静止的自机）
wsl -e bash -c 'cd sw/apps/game/gfx2dGame && make PLATFORM=host OBJDIR=build_host_stg EXTRA_CFLAGS=-DSTG_FIELD=1 && ./build_host_stg/gfx2dGame'

# 判据：约 14 秒、840 帧，末尾 fail 0 与 done；无窗口时加 SDL_VIDEODRIVER=dummy
wsl -e bash -c 'cd sw/apps/game/gfx2dGame && make PLATFORM=host OBJDIR=build_host_stg2 EXTRA_CFLAGS=-DSTG_FIELD=2 && SDL_VIDEODRIVER=dummy ./build_host_stg2/gfx2dGame'

# N_max 搜索（模拟时钟）：两遍各一行 nmax result，末尾 fail 0 与 done
wsl -e bash -c 'cd sw/apps/game/gfx2dGame && make PLATFORM=host OBJDIR=build_host_stg3 EXTRA_CFLAGS=-DSTG_FIELD=3 && SDL_VIDEODRIVER=dummy ./build_host_stg3/gfx2dGame'

# 没有显示器时截帧看画面：第 0、300、839 次呈现各写一张 PPM
wsl -e bash -c 'cd sw/apps/game/gfx2dGame && SDL_VIDEODRIVER=dummy GFX2D_SNAPSHOT=0,300,839 GFX2D_SNAPSHOT_DIR=/tmp ./build_host_stg2/gfx2dGame'

# 游戏的判据入口：一局约 10,185 帧；GFX2D_UNPACED=1 让呈现不等场，PC 上约 75 秒
wsl -e bash -c 'cd sw/apps/game/gfx2dGame && make PLATFORM=host OBJDIR=build_host_stg4 EXTRA_CFLAGS=-DSTG_FIELD=4 && SDL_VIDEODRIVER=dummy GFX2D_UNPACED=1 ./build_host_stg4/gfx2dGame'

# 截各段的画面：标题、杂兵段、Boss 进场、四张符卡、CLEAR
wsl -e bash -c 'cd sw/apps/game/gfx2dGame && SDL_VIDEODRIVER=dummy GFX2D_UNPACED=1 GFX2D_SNAPSHOT=60,700,1900,2600,4600,6600,9500,10100 GFX2D_SNAPSHOT_DIR=/tmp ./build_host_stg4/gfx2dGame'
```

- 判据模式的日志两遍逐字节相同；`check_sw7_game.sh` 的 G10 跑两遍比对。
- `GFX2D_SNAPSHOT=<k,k,...>` 只在 PC 的 `bsp/host` 里有：到第 k 次成功的 `board_present()`（从 0 起）时，
  把要呈现的那一面写成 `snap_<k>.ppm`（640×480，P6），目录由 `GFX2D_SNAPSHOT_DIR` 给，缺省当前目录；
  stderr 里打一行 `board: snapshot k -> 路径`。不改 `board.h`，也不改应用的输出，任何应用都能用。
- `GFX2D_UNPACED=1` 也只在 PC 的 `bsp/host` 里有：`board_present()` 照常上传、计数、截帧，但不等下一个场起始；
  `board_wait_frame()` 也不等。场号仍按墙钟走，所以不等场时场数增量没有意义；只给判据入口这类不看帧率的运行用。
- `STG_FIELD=3` 的期望输出（2026-09-26，B 批的常量与模拟时钟模型，见 sw 第 07 章 3.6 节）：

  ```
  nmax result OK n_max 2429 knob 2448 ft_max 1659652 ft_p99 1659652 probes 15 timeouts 0
  nmax result OK n_max 6419 knob 6576 ft_max 1663388 ft_p99 1663388 probes 17 timeouts 0
  ```

  前一行是 16×16 负载、后一行是 8×8。**这是模拟时钟下的流程验证，不是性能数字**；板上的数字在 SW9 出。
- 板上构建与 SW6 相同，另加 `EXTRA_CFLAGS=-DSTG_FIELD=2`（或 `4`）；`-DSTG_FRAMES=N` 限总帧数。

## SW9 板上移植

SW9 于 2026-10-04 按本次范围验收归档；完整硬件回放重跑与现场观察的边界由 sw 第 08 章第六节维护。

板上 `gfx2dGame` 已接入真实硬件引擎。普通构建保留逐帧 CRC 对账；`PERF=1` 生成带
`-perf` 后缀的性能 BIN，逐帧整屏 CRC 停用，每秒一次的普查、CSV 与硬件 `PERF_*` 计数保留。
硬件性能路径交替使用两张命令表，CPU 组下一帧时引擎执行上一帧；引擎中断确认完成，
场中断确认双缓冲交换。软件路径仍串行执行。`SW9_REPLAY_HW=1` 的录像入口使用串行硬件路径做逐帧对账。
`board_run.py --forward-input` 把键盘与 XInput 手柄经同一串口转发，协议、实体键映射与输入脚本见
[`tools/board/README.md`](../tools/board/README.md)。

修复后板上软件前 600 帧的 `g` 行与同版 PC 参考录像逐帧相同；完整硬件 9208 帧未完成本轮重跑，
入库旧日志只作历史证据。
同一张命令表的软硬件 CRC 一致，跳过失效注错后不同。4KB/1 路主线比特流上，
性能构建的普通游戏 240 帧短测预热后为 60 fps，软件约 2.5 fps；16×16 与 8×8 的硬件真实时钟
`N_max` 分别为 1014、1365，软件均为 `BELOW_N0`。S1 的 1744 旋钮约 2030 颗活动弹时，
硬件约 30.6 fps，软件预热后的完整帧约 0.73 fps。场扫描欠载与中断故障计数均为 0。
这些是当前比特流和程序的板上数字，不等同于更换缓存或重编比特流后的结果。

SW9 入库日志的离线复核入口是 `tools/check/check_sw9_logs.py`。正式交互版程序已写入 Flash
应用分区，`board_run.py --flash-boot` 自启动和三模式、两后端脚本切换通过；详细构建、
比特流身份、原始日志、计数口径、完整回放与尚未记录的 HDMI/实体键现场观察边界见
[`sw 第 08 章`](../../Engineering/TechReports/sw/08-板上移植.md)。

## 编译（八个诊断程序）

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

2026-09-27 按地址失效版为 **2538 字节**，SHA256 `63db2b19dc6c4a3ae8d118a4994645ed8594e54c38ed7b45ae9b1a4e6b00d573`，4KB/16KB 均 `fail 0`/`done`；原始日志为 `apps/probe/ddrAliasTest/logs/closeout_20260927_{4k,16k}.log`。新机制与回归见 [platform 第 10 章](../../Engineering/TechReports/platform/10-CPU每弹幕开销与缓存对照的复现.md) 5.9。

以下为旧版期望记录：gcc 13.4.0 编译，bin 2562 字节（`sha256:9d179bece4f1ff31…`，gcc 8.3.0 时 2418 字节），2026-09-19 上板输出与下面一致。串口逐行打印 `LOW k=0..27`、`LOW k=-1`、`HIGH k=28..31`、`CACHE_NOEVICT`、`CACHE_EVICT`，全部为 `ok`，最后一行之前是 `base 0x0c800000 fail 0`。其中 `CACHE_NOEVICT` 读到 `11`（缓存里的旧值）是预期现象，不计入 `fail`。实验设计与原始输出见 `../../Engineering/TechReports/platform/02-DDR地址译码与AXI-ID实测.md`。

## ddrBurstProbe 的期望值

需要带 `axi_probe_master`、`axi_mux_guard`（VERSION 2）与扫描输出 `display` 的比特流
（2026-09-21 起；2026-09-19~20 那批数据用的是 `scanout_stub` 替身，同一个 APB 窗口）。
2026-09-27 按地址失效版为 **6722 字节**，SHA256 `46b492e865551b6327c00f727c903f587abcf9490cb31b42265ecfbc93461b61`，4KB/16KB 均 `fail 0`/`done`，SHARE/TRI/INJECT 阶段全部通过。日志为 `apps/probe/ddrBurstProbe/logs/closeout_20260927_{4k,16k}.log`。

旧版 gcc 13.4.0 编译的有界轮询版 bin 6730 字节（sha256 前 16 位 `2b2a06117bb68e38`；
适配 display 的初版是 6602 字节 `65797eb605112cdf`，更早是 `b31871c65b8e3fea`），`resume` 后约 3 秒输出完毕。
串口末尾为 `fail 0` 与 `done`，除下列各处外每行为 `ok`。
2026-09-21 两轮的原始输出在 `apps/probe/ddrBurstProbe/logs/e5_display_run_{1,2}.log`（3535 字节），
独立复现审查后的有界轮询版两轮在 `e5_display_review_run{1,2}.log`，同样六阶段全 `ok`、末行 `fail 0`/`done`；
2026-09-19 三轮（替身时代）在 `e5_v2_run_*.log`，判据改前那次假红在 `e5_v2_mask_race.log`。

**采集时 `--stop-on` 不要用 `done`。** 这个程序会打 `probe_done`，而 `done` 是它的子串：
`board_run.py --stop-on done` 会在第一行 `INJECT ... probe_done` 处就停止采集，日志缺尾、看起来像程序卡住
（2026-09-21 复现两次）。用只出现在末行的 `--stop-on "fail "`。

- `IDENT guard ctrl=3 mask=7 age=64 ok`：guard 的 ID、`VERSION == 2` 与上电默认值，加载旧比特流（VERSION 1）时这一行 FAIL；
  `IDENT scan id=0x44495350 base=0x02000000 ok`：扫描输出窗口（`"DISP"`）。**配置归属这条判据就在这里。**
- `CROSS`、`SWEEP` 与 E4 相同，每行 `ok`；`SHARE errs=0x00000000 ok`。
- `SHARE cpu ticks alone=... shared=...`：CPU 拷贝在探针满负载读时约 **1.64 倍**（实测 52.0 ms 到 85.2 ms；E4 轮询基线约 1.94 倍）。
- `TRI rd` / `TRI wr`：三方并发（扫描输出 + 探针读或写 + CPU 拷贝）。CPU 拷贝 88.7 ms（1.70 倍）/ 99.3 ms（1.92 倍）；
  guard `max_wait_ar` 303 / 491、`max_wait_aw` 476 / 507、`hits` 约 17300 / 16900、`status=0`；
  扫描 `beat = txn × 160`、`skip=0`、`resperr=0`，最坏 `max_first` 78 / 约 364、`max_full` 244 / 约 530、`max_gap` 42~44 / 44。
  写方向时扫描首拍等待变长，来自控制器内部读写共用 DDR 端口（platform 第 06 章 5.3、5.4 节）。
- `INJECT age1_rd` / `age1_wr`：`AGE_LIMIT` 设 1，两个方向命中数都必须显著增加（实测 278536 / 278535）且 `probe_done`；
  `age64_*` 只记录（实测 5455 / 5185，替身时代约 5600）。这一条只证明 guard 接到了真多路器，写方向 guard 不计为等待保证。
- `INJECT mask_s0 txn a b c d e ok`：清掉 guard 掩码的 S0 位。清位时在途的那一笔会照常完成，所以 `c - b` 最多为 1；
  之后必须冻住（`d == c`），恢复后继续增长（`e > d`）。实测 `13440 13771 13771 13771 14362` 与 `12000 12158 12158 12158 12750` 都判 ok。

周期数随板子会有约 2% 的波动；E4 各阶段的参考值与解释见
`../../Engineering/TechReports/platform/03-DDR3上的第二个主机与突发预算.md` 5.4 节，
E5 两个阶段的判据、实测值与解释见 `../../Engineering/TechReports/platform/06-Axi_Mux三端口仲裁与SoC防饿死保护.md` 5.3 节，
扫描输出从替身换成真实模块之后的复测见同一章 5.4 节。
所有探针 DONE 轮询现在都有 5 秒总上限并检查 TIMEOUT/RESP_ERR/LAST_ERR；错误或越界时 ABORT 后判 FAIL，
`age64_rd/wr` 也不再忽略返回值。若 AXI 总线本身已经挂死到 CPU 无法继续执行，仍需重新加载比特流恢复。

## displayFifoProbe 的期望值

实验 E5 2.4 节的判据程序：真实扫描 FIFO 的压力矩阵与深度定值。**只在 Ti60 上跑**，
`make RISCV_BIN=<RISCV_BIN>` 直接调 `target.mk`，产物 `build/displayFifoProbe-O2.bin`；
需要带扫描输出 `display`（VERSION 1）、`axi_mux_guard`（VERSION 2）与 `axi_probe_master` 的比特流。
gcc 13.4.0，最终复核版 bin 11322 字节（sha256 前 16 位 `e5cf7dabcfe782af`）；`resume` 后约 11 秒输出完毕
（`idle` 约 2 秒，`read`/`write` 各约 3.4 秒，其余是等探针跑满 `COUNT`）。
**每次上板都从完整烧写比特流开始**，不要用 `--no-program` 复用上一次的配置：core 复位不碰像素域，
上一次留下的 `SRC` 与累计值会灌进这一轮的量测。
采集的 `--stop-on` 用只在末行出现的 `"fail "`，不要用 `done`：失败现场字段 `ptxdone` 也包含
这个子串，会像 `ddrBurstProbe` 的 `probe_done` 一样让采集提前关闭。

初版三轮在 `e5_2_4_run{1,2,3}.log`，但其中 display `status=0x4`，说明旧 `scan_off()` 没有真正停机；
复核阶段六次完整启动在 `e5_2_4_review_run{1..6}.log`：run 1~5 使用已修正压力路径的 11306 字节版
（`f832cb6e2e5f56b1`），run 3 又抓到探针尚未启动时失败清理会误等并误写 ABORT；最终 11322 字节版
只修该失败分支，并由 run 6 复核正常路径。run 1、2、4、5、6 末行 `fail 0`/`done`，run 3 在压力开始前
报 `no_frame`/`fail 3`，完整重烧后恢复。失败轮没有样本，不混入下面的压力数据：

- 前三行 `IDENT` 必须全 `ok`：`disp id=44495350 ver=00000001`、
  `guard id=4d555847 ver=00000002 ctrl=00000003 mask=00000007 age=00000040`、`probe id=41584950`。
  任一行 FAIL 就是比特流不对，程序打 `fail 1` 直接结束，不再测后面的。
- `SETUP fb0=02000000 front=02000000 ctrl=00000003`。
- 每个场景出六行（摘要 / bad / probe / scan / guard / 结论），三组的期望值：

| 场景 | `min` / `max` / `med_lo` / `med_hi` | 首次最小值帧号 | 扫描 `max_first` / `max_full` / `max_gap` | `txn` / `beat` | 探针 |
| --- | --- | --- | --- | --- | --- |
| `idle` | 347 / 347 / 347 / 347 | 127~130 | 66 / 239 / 44 | 59520 / 9523200 | 不启动（没有 `probe` 行） |
| `read` | 331 / 331 / 331 / 331 | 255~258 | 65~66 / 237~239 / 44 | 97440 / 15590400 | `status=0x2`、`txdone=0x100000`、`errs=0`、`cycles≈0x1405xxxx`（3.36 s） |
| `write` | 327~329 / 329 / 329 / 329 | 464~568 | 353~378 / 521~544 / 44 | 99360 / 15897600 | 同上，`cycles≈0x1467xxxx`（3.42 s） |

- 每组的 `bad beats=0 end=0 low=0`、`zero=0`、`under=0`、`resperr=0`、`skip=0`；
  `scan` 行必须 `status=0` 且 `beat == txn × 160`；CLEAR 也必须在停稳后执行，不能从半笔事务中间开始计数。
  guard 的 `status=0`；命中数只作诊断，最终有效会话的累计末值约 824~835，不设阈值。
- 采样期不打印：串口上先是 `IDENT`/`SETUP`，中间静默约 10 秒（三个场景在跑），再一次性出全部结果。
- `first`（首次最小值的帧号）是三组里唯一明显变化的一项，它跟着像素域的 `SNAP_FRAME` 走，
  与 `FRAME_COUNT` 原点不同、不能相减；它只回答「最小值出现在哪一场」，不作判据。
- 失败时该场景多打一行 `SCEN <名字> scene aborted=... pstatus=... snap_*...`。程序先保存现场；
  健康探针在原 5 秒预算内自然 DONE，只有硬件错误或真超时才 ABORT，避免在健康写事务中途撤销 valid。
- 每个场景的固定起点是：两侧停稳 → 事务边界 CLEAR 并确认累计值为 0 → 无压力预装 2 场 →
  启动探针 → 正式压力预热 2 场 → 收 120 场。`no_frame`、欠载或坏样本直接 FAIL，不能用第二轮抹掉。

结果表、解释与适用边界见 `../../Engineering/TechReports/platform/08-扫描FIFO的压力矩阵与深度定值.md`。

## cacheCoherencyTest 的期望值（B6 改写版，引擎当外部写入者）

现行比特流（引擎在网表、探针已撤）跑的是 B6 改写版。gcc 13.4.0、`-O2`，target.mk 构建，产物落 `build/`、`build_inject/`、`build_inject_selftest/`。**加载前必须 `reset halt`**（`tools/board/board_run.py` 已包含）。程序整体只跑几百毫秒；复核实测原先的两秒启动等待不足，完整输出会落在 `board_run.py` 的「程序已加载」分界之前，导致 `--stop-on done` 误报超时。现等八秒再打横幅，三构建的完整输出与 `done` 都在分界之后，见下方新日志。

| 产物 | 编译 | bin 字节 | SHA256 前 16 位 | 末尾 | 日志 |
| --- | --- | ---: | --- | --- | --- |
| 正常版 | （默认） | 39826 | `08a61c0342ce8381` | `fail 0` | `logs/b6_review_20261002.log` |
| 注错版 | `INJECT=1 OBJDIR=build_inject` | 39818 | `5dd9716e1229fdfa` | `fail 0`，四行 `FAIL (inject)` | `logs/b6_review_inject_20261002.log` |
| 注错自检版 | `INJECT=2 OBJDIR=build_inject_selftest` | 39794 | `e9e87b9ce5e70cf` | `fail 4`，四行 `NOT_RED (inject)` | `logs/b6_review_selftest_20261002.log` |

`b6_board_20261002.log` 等原开发日志保留现场，但 `done` 位于采集分界之前；验收本轮改用上表三份日志。

每行带 `got=<8 位十六进制>`（该次读回的实际值）。正常版逐行：

- `IDENT ok`：引擎在位（ID 读回 `0x47465801`）；
- `P ... ok`：引擎 FILL 写入后失效读到新值 `00c0def1`；
- `A1 ... ok`、`A2 ... ok`：读回旧值（`c0de00a1`/`c0de00a2`），缓存遮蔽了引擎的写入；A 组跑两遍，第二遍验证反复性；
- `A3 ... ok`（两遍）、`B ... ok`、`C ... ok`：失效全部/单地址之后读到新值 `00c0def1`；
- `D ... ok`：CPU 写源表面、引擎 BLIT 拷到另一面、CPU 失效后逐字读回。

判据常数必须取**最高字节为 0** 的值（`V_NEW = 0x00C0DEF1`）：像素规格规定写 XRGB8888 时高字节恒写 0（软件执行器与硬件一致），带非零高字节的期望值永远读不回来。装行（`arm_line`）走**别名写**（翻转 bit28）加**换行读**（读 `a^0x2000_0000`），与探针版读数序列等价——被观测行不能 CPU 直写，不换行会让上一轮的新值遮住装行。

注错版变红的恰好是 A3 两行加 B、C 四行，P、A1、A2、D 与正常版相同。注错自检版保留失效调用、按注错模式判定，四行计入 `fail`，末尾必须是 `fail 4`；它证明注错宏没生效的产物不会得到 `fail 0`。

探针版的期望值（外部写入者 = `axi_probe_master`，gcc 8.3.0/-Og 与 gcc 13.4.0/-Og 两批）见 `logs/normal_*.log`、`inject*.log`、`o0_*.log` 与 platform 第 05 章；探针版随 D22 撤探针停用。

## engineTest 的期望值

引擎 P1 B6 的板上判据（见上表 `apps/test/engineTest/` 行）。gcc 13.4.0、`-O2`，target.mk 构建。

| 产物 | 编译 | bin 字节 | SHA256 前 16 位 | 末尾 | 日志 |
| --- | --- | ---: | --- | --- | --- |
| 正常版 | （默认） | 43210 | `482aaccf10cc2d06` | `fail 0`，`COUNT32 ok` | `logs/b6_review_20261002.log` |
| 注错版 | `INJECT=1 OBJDIR=build_inject` | 43290 | `699253ee80e55b21` | `fail 0`，`COUNT32 ok` 且 INJ-COH 行 `differ FAIL (inject)` | `logs/b6_review_inject_20261002.log` |

正常版逐段：`ENG id ok`；十一行金标准 `名字 引擎面CRC 软件面CRC ok`（两面 CRC 相同且与 `sw/lib/test/golden.txt` 一致，本轮复核 `fill-screen 9cd34cb7`、`blit-copy bc9be6ff` 等 11/11）；`COUNT32 ok` 用 FILL+END 与 `LIST_COUNT=0x10000` 验证驱动没有截成 16 位；`DIR narrow-x1x2x3 ok`、`DIR key-sparse ok`（首末拍部分掩码与色键稀疏掩码）；`INJ-COH ok`；`PERF-BUSY ok` 加 `PERF busy=<hex>`（16 颗 16×16 预乘 ALPHA 的 `PERF_BUSY`，除以 16 得每颗周期数，本轮 `0x49f8` = 18936，约 1184 拍/颗；初次开发日志为 `0x4a10`）；`STRESS ok` 加 `wm=欠载=ar=aw=`（扫描 FIFO 最低水位，判据 ≥ 160 拍；欠载 0；guard 的 SoC 最大地址等待登记，本轮 wm=0x14f=335、z=0、ar=0xd3=211、aw=0xe0=224）。注错版仅 INJ-COH 一行变为 `differ FAIL (inject)`（不计入 `fail`），其余相同。

G1 的现行正常版追加 TEX-CAPS/BUSY/LOAD、十一行 TEX 金标准、六个 TEXERR
错误三元组及 TEX-PERF。最终日志 `engineTest/logs/g1_board.log` 为 `fail 0/done`，
DDR/TEX 同轮为 1183.625/657.375 周期每颗；上表两个 BIN 身份属于历史 B6。

**G1 已验收的产物身份**在 `sim/engine/tex/logs/g1_artifacts.json`，那里记的路径是
`build_g1/engineTest-O2.bin`，49002 字节、SHA256 `9e6555528875ca9f...`。

**跑回归之前先核对 `build/engineTest-O2.bin` 的身份**：它必须与 `build_g1/` 那份逐字节相同。
2026-10-08 踩过一次：`build/` 里留的是 G1 之前的旧版本（48346 字节、`4c8bf102...`），
而回归表指向的正是 `build/`，于是在正确的比特流上跑出 `fail 17`（十一行 TEX 金标准与
六个 TEXERR 全红、引擎面 CRC 却与归档一致），排查了半天才发现是产物陈旧而不是 RTL 缺陷。
重编一次即 `fail 0`。这类「源码与产物脱钩」不能靠人记得，判据要自己比对身份：
`sha256sum build/engineTest-O2.bin build_g1/engineTest-O2.bin` 两行相同才算可以开跑。
当前 G1 的完整身份集中在 `sim/engine/tex/logs/g1_artifacts.json`。
外部判据运行 `tools/check/check_g1_logs.py --board-only`。

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
[display 第 01 章 3.10 节](../../Engineering/TechReports/display/01-从DDR3帧缓冲到HDMI的扫描输出.md)与 E7。
判据只卡一个很宽的上界（`per_px_x100` 不超过 10000），不拿它当性能结论。

## platformTest 的期望值

PC 平台层（SW4）的判据程序，只跑在 PC 上，开一个 640×480 窗口：

```
wsl -e bash -c 'cd sw/apps/test/platformTest && make && ./build_host/platformTest'
wsl -e bash -c 'cd sw/apps/test/platformTest && SDL_VIDEODRIVER=dummy ./build_host/platformTest'   # 无窗，同一个二进制
wsl -e bash -c 'bash Main/tools/check/check_sw4_pc.sh'                                             # 全部判据串起来
```

一次跑完六段，末尾 `fail =0` 与 `done`，退出码 0。各段的期望：

- `cases`：十一行 `case <n> <名字> crc <八位十六进制>`，**必须与 `lib/test/golden.txt` 逐行相同**
  （判据脚本拿来对）；这一段每帧仍然 present，所以窗口里能看到每个用例画出来的整屏图案。
- `pace`：`dfield` 与 `dswap` 都必须等于 600，`missed=0`，`min_us`/`max_us` 落在 15~18 毫秒
  （实测 16.5~16.8 毫秒，一场是 16.667 毫秒），六行 `chk P1~P6` 全 `ok`。
- `drop`：`dfield=2`、`dswap=1`，三行 `chk D1~D3` 全 `ok`。**掉帧数是两者增量之差，先在 32 位上减完**。
- `live`：每帧一行 `live <帧> field <场号> df <增量> keys <位图>`。
  **这一段要人动手**：按 F1~F3 看窗口在 1×/2×/3× 之间切换（`keys` 那一列必须一位都不变），
  点窗口的关闭按钮验退出。关窗之后会多打一行 `live quit_frame=` 与 `live quit_df=0`，
  并且 `chk L1 关窗当帧没有等场 ok`；不点关闭按钮就一直跑到 300 帧，
  打 `live 没有关窗（退出那一条要人点窗口的关闭按钮）`，不算失败。
- `replay`：先打一组 `g2dr`/`key` 行（**录制结果就是日志**，可用
  `python3 Main/tools/check/replay2c.py --from-text` 收成 `.g2dr`），再打 `play1`/`play2` 两串
  各 1800 行 `<帧> crc <八位> keys <位图>`，`replay diff=0` 与 `chk R3` 表示两次逐帧相同。
- 注错版（`make INJECT=REPLAY_CLOCK`，产物 `platformTest-inject`）把时刻混进画面，
  `replay diff` 必须大于 0，`chk R3 注错后两次回放必须不同 ok`；
  十一用例的 CRC 与 `pace`/`drop` 各段不受影响。

**有窗与无窗跑的是同一个二进制**（`SDL_VIDEODRIVER=dummy` 只是换驱动），
两条 CRC 序列必须逐行相同；这是 [sw 第 03 章](../../Engineering/TechReports/sw/03-PC平台层与帧节拍.md) 5.2 节的第 8 条判据，由 `check_sw4_pc.sh` 比。
日志行尾是 `\r\n`，比对前先做 CRLF 归一。

## assetTest 的期望值

素材流水线（SW5）的判据程序。PC 上开一个 640×480 窗口显示合成出来的那一帧：

```
wsl -e bash -c 'cd sw/apps/test/assetTest && make && ./build_host/assetTest'
wsl -e bash -c 'bash Main/tools/check/check_sw5_assets.sh'        # 全部判据串起来
wsl -e bash -c 'cd sw/apps/test/assetTest && make PLATFORM=target RISCV_BIN=<RISCV_BIN>'
```

一次跑完，末尾 `fail 0` 与 `done`、退出码 0。期望输出（入库日志
`logs/pc_normal.log`，板上是 `logs/board_normal.log`，两者**判据行与 24 行字符画应逐行相同**。
`pc_normal.log` 于 2026-09-27 按 20 项素材包与 Spleen 字库重录，`board_normal.log` 于 2026-09-28 在同一个包下重新上板录下，
去掉 `board_run.py` 的标记行之后两者 76 行逐行相同）：

| 行 | 期望 |
| --- | --- |
| `pak bytes/entries=` | `2840320 20`（当前 20 项素材包：SW5 的 8 项加 SW7 的 12 项；重打素材后跟着变） |
| `pak crc=` | `b370ba0a` |
| `chk P1~P3` | 版本、整包 CRC、总长与条目数与生成的 `assets.h` 三项一致 |
| `chk E1~E8` | 八项素材的几何与 `assets.h` 一致；`BULLET_PM`/`BULLET_ST` 分别是预乘/直通；`KEY_DEMO` 带色键且键字相符；`SOLID8` 是 8×8 |
| `entry 0..19` | 逐项 `id/fmt/宽高/sl/len/crc`，**与 `check_sw5_pak.py` 的独立实现逐行相同** |
| `chk M1` | 相邻分区指针之差等于分区表偏移之差（`gfx_mem` 的窗口才成立） |
| `chk S1~S7` | 六个素材条目与帧缓冲各自 `gfx_surface_set()` 成功——**按需要挑，不是按 ID 全登** |
| `chk C1` | 48 条命令全被构造层接受；`list used/cap= 48 32768` |
| `chk X1` | 软件执行器无错；`exec cmd/pixel= 48 710824` |
| `frame crc=` | `351937af`（**PC 与板上必须相同**，这是跨平台判据） |
| `win crc=` | `223df92a`（图集那 4 个半透明格子所在的 64×64 窗口） |
| `thumb 64x24` | 24 行 64 列的字符画，由帧缓冲采样得到，留档的那张「可目视核对」的图 |

板上产物 `build/assetTest-O2.bin`，2026-09-28 是 41,258 字节
（sha256 前 16 位 `0e306b6cb089a9ca`；板上构建把 `lib/` 全部链进来，SW7、SW8 往 `lib/` 加的代码让它比 2026-09-23 的 25,770 字节大）。
板上跑要先把镜像写进 Flash（`tools/board/make_flash_image.py` + `ftdi_pgm`），
再用 `tools/board/board_run.py --flash-boot` 采集——`--bin` 会走 JTAG 把程序直接写进 DDR，
把要验的 Flash 启动路径绕过去。破坏素材包里一项数据再烧进 Flash，板上必须报
`pak err 14 entry_crc id=<编号>` 并拒绝使用（日志 `logs/board_broken.log`）。

## 第三方：lib/s3l/small3dlib.h

上游是 small3dlib（作者 drummyfish，**CC0**，公有领域），header-only、纯整数、不用堆，
正好能在这颗没有 FPU 的 rv32im 上跑。本仓这一份相对上游只有一处改动：
第 1353 行起 `S3L_sin` 里加了一段 `#if defined(S3L_USE_LACC) && S3L_USE_LACC` 的分支，
那是另一个平台的硬件超越函数接口；本仓不定义这个宏，走的是上游的整数查表路径。
除此之外一字未改，要升级就整份换，改动只有这一处要重新加回去（或者一起去掉）。

它是 header-only，所以 `lib/` 下没有对应的 `.c`，应用直接 `#include "s3l/small3dlib.h"`。
按分层规则应用能看到 `-Isw/lib`，所以这条包含路径成立。

## cup3d 的期望值

需要带扫描输出的比特流。gcc 13.4.0，`-O2`，bin 22,346 字节
（2026-09-21 SW4 之前是 20,394；`LIBS = core` 会链进 `lib/core/replay.c` 的约 1.3 KB，
不引用录像的应用也带上，见 [sw 第 03 章](../../Engineering/TechReports/sw/03-PC平台层与帧节拍.md) 5.4 节的已知代价）。
串口三行加 `done`，**画面要看屏幕**：屏幕正中间 320×240 的区域里是一只有明暗的茶杯，
四周是黑的（边框只在启动时清一次，之后每帧只清画面那一块）。

```
cup3d tris=396 res=320x240
frames=240 per_frame=5001653 fps_x100=1900
per_frame clear=420437 shade=24930 raster=4095472 present=460015
done
```

2026-09-21 板上实测，每帧约 5,000,000 个周期 = 50 毫秒，**20 fps**
（`fps_x100` 是整数除法，240 帧那次算出 1900、900 帧那次 2000，真值在 19.99 附近）。
四段的占比：

| 段 | 周期 | 毫秒 | 占比 | 说明 |
| --- | --- | --- | --- | --- |
| 光栅 | 4,095,472 | 41.0 | **82%** | 396 个三角面，关了背面剔除所以内外壁都画 |
| 等切换 | 460,015 | 4.6 | 9% | `board_present` 等场起始 |
| 清画面 | 420,437 | 4.2 | 8% | 320×240 的颜色缓冲加 z-buffer |
| 着色 | 24,930 | 0.25 | 0.5% | 每帧 396 次点积，按面不按像素，可以忽略 |

（四段逐次运行有千分之几的抖动，光栅那一段 900 帧那次是 4,025,887。）

这组数是软件渲染的第一份真实负载数据，比 `displayTest` 的纯填充更有参考价值，
E7 的归档报告里引了它。**瓶颈在光栅不在访存**：着色只占 0.5%，清屏加上屏合起来也只有 17%。
要提速先动光栅（背面剔除、缩小覆盖面积、减少过绘），而不是先动分辨率。

计时全部用 64 位累加，每帧取一次区间再累加，不拿首尾两个读数相减：
`board_cycles()` 是 32 位回绕的，100MHz 下 2^32 个周期只有 42.9 秒（`bsp/include/board.h` 写明了），
而本程序一帧就要五百万个周期，900 帧是 45 亿——首尾相减会回绕成一个小得离谱的数
（实际踩到过，报出 439 fps），32 位的累加器同样存不下。

## stripTest 与 ABI v2（G3）

lib/gfx 的 begin_strips 绑定 1 MiB 定长表并吸收 SET_CLIP，软件条带执行器
按 LOAD/EXEC/FLUSH 重放；旧直通 API 继续可用。构造错误必须拒绝提交。
Ti60 驱动新增 gfx_engine_submit_strips，旧 submit 显式选直通。完整约束见
spec 第 02 章 3.9 节；正式对拍入口为 tools/check/check_g3_stripctl.py。

~~~text
make -C sw/apps/test/stripTest RISCV_BIN=<RISCV_BIN>
~~~

产物 build/stripTest-O2.bin；期望：同帧与 padding CRC 相同、空桶不改画面、
SET_CLIP 错误 2/1024/cafe005a、容量错误 2/1024/0、错误后再提交与返回直通
均 ok，末行 fail 0/done。原始串口在该应用 logs/g3_board.log；
新 bit 的 engineTest 兼容性在 engineTest/logs/g3_compatibility.log。
该程序使用 JTAG，不写 Flash；游戏条带后端与 N_max 属 G4。
