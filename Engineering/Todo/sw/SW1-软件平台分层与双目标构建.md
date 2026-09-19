# SW1 软件平台分层与双目标构建

状态：**已验收**（2026-09-19，用户验收）。按用户要求条目保留，暂不删除。实现、两轮复核与修复见第 4 节。
代码、判据的原始输出与四处踩坑记录见
[`../../TechReports/sw/01-软件平台分层与双目标构建.md`](../../TechReports/sw/01-软件平台分层与双目标构建.md)。
无前置。阻塞 SW2 之后全部条目（它们都要往这套分层里放代码）——分层已经可用，SW2 及以后可以直接开工。
2026-09-19 按用户审核意见重写：原稿把平台层 `hal/` 放在游戏程序目录里，已作废，原稿的问题记在第 4 节。

## 1 要解决什么

`Main/sw/` 下现在有六个诊断程序，每个都是「应用 + 直接调 sdk 驱动」，
计时、串口输出、缓存失效这些代码各写各的。接下来至少有三个程序要共用同一批板级代码与纯算法代码：
游戏（`gfx2dGame`）、E7 的 `execSwTest`、E8 的 `entityBench`，以后还有引擎测试程序。
再加上 PC 版，同一份算法与游戏代码要在两个平台上编译。

限制条件是 `sdk/`：它是 SoC IP 的生成物，重新生成后要整体同步，项目代码放进去就会被覆盖
（[`../../../Main/sdk/README.md`](../../../Main/sdk/README.md)）。
所以需要一层**属于项目、叠在 sdk 之上**的板级层，并且让分层由编译结构来保证，而不是靠约定。

2026-09-19 读 sdk 的构建片段与头文件，确认了四个必须在本条处理的事实：

| 事实 | 出处 | 影响 |
| --- | --- | --- |
| 官方规则用 `notdir` 生成目标文件名：`OBJS := $(notdir $(OBJS))` | `sdk/software/standalone/common/standalone.mk` | 不同目录下的同名 `.c` 编成同一个 `build/obj_files/x.o`。多目录后必然撞上，所以编译规则要自己写 |
| sdk 驱动头文件在文件作用域写裸 `asm(".set regnum_x0, 0")` | `sdk/software/standalone/driver/riscv.h` 第 261 行起 | 严格的 `-std=c11` 不认识 `asm` 关键字，两边统一用 `-std=gnu11` |
| 默认链接脚本的栈是 `__stack_size` 缺省 4K；该文件本身是 sdk 已登记的修改项 | `sdk/bsp/.../linker/default.ld` 第 17 行 | 项目要有自己的链接脚本，栈大小在那里定 |
| `soc.mk` 默认 `DEBUG?=yes`、`DEBUG_OG?=yes`，即 `-Og`；关掉之后是 `-Os` | `sdk/bsp/.../include/soc.mk`、`riscv64-unknown-elf.mk` | 优化级别由项目显式指定并记录，不吃默认值 |

另外，本机 WSL 的 gcc 原先编不了 32 位程序：`gcc -m32` 报找不到 `crti.o`，没有装 multilib。2026-09-19 已装 `gcc-multilib`（15.2.0），`-m32` 下 `sizeof(long) = sizeof(void *) = 4`，ASan 与 UBSan 可用。WSL 里 `sudo` 要交互输入密码，经 `!` 执行会卡住；改用 `wsl -u root -e apt-get install -y <包>`。

## 2 怎么切分

### 2.1 分层与目录

```
Main/
  sdk/                      厂商上游镜像，只读；重新生成 IP 后整目录同步
  sw/
    mk/
      target.mk             板上构建：复用 sdk 的 bsp.mk 与工具链片段，编译规则自己写
      host.mk               PC 构建：WSL gcc + SDL2
    bsp/                    项目板级层
      include/board.h       板级接口，两个实现共用
      include/memmap.h      DDR 分区的偏移与大小（设计文档 2.1 节），不含绝对地址
      ti60/                 板上实现，调 sdk 的 driver 与 soc.h
      ti60/linker/gfx2d.ld  项目链接脚本
      host/                 PC 实现：SDL2 显示与键盘，DDR 分区用一块内存模拟
    lib/                    纯 C，两边都编，不依赖任何平台
      core/                 定点、CRC32、xorshift32、正弦表、轻量格式化输出
      gfx/                  命令表构造、软件执行器（SW3）
      test/                 主机侧单元测试
    gfx2dGame/              应用：main.c 与 game/
    bootSizeTest/ ...       已有六个诊断程序，本条不动
```

依赖方向只有一个：

```
  gfx2dGame 等应用 ──> lib/core, lib/gfx
        │
        └──────────> bsp/include/board.h
                          │
               ┌──────────┴──────────┐
          bsp/ti60                bsp/host
               │                     │
          sdk（只读）              SDL2
```

`lib/` 不依赖任何东西；`bsp/` 不依赖 `lib/` 与应用；只有应用把三者接在一起。
PC 版在这套结构里就是「另一块板子」，与 Zephyr 的 `native_sim`、FreeRTOS 的 POSIX 移植是同一种做法，
应用代码里不出现 `#ifdef` 平台判断。

### 2.2 板级接口 `board.h`

只放与硬件有关、两边都能实现的服务。应用层的事（素材按编号查找、录像回放逻辑）不进这里：
前者是 `lib/core` 解析素材包，后者是 `lib/core` 处理一段按键缓冲，文件读写才归 `bsp/host`。

| 组 | 接口 | `bsp/ti60` | `bsp/host` |
| --- | --- | --- | --- |
| 初始化 | `board_init()` | UART、CLINT | SDL 初始化、分配模拟的 DDR 分区 |
| 内存区 | `board_region(REGION_x)` 返回指针与大小 | `memmap.h` 偏移加 DDR 基址 | 指向模拟内存块里的同一偏移 |
| 计时 | `board_cycles()`、`BOARD_HZ` | `clint_getTimeLow` | `SDL_GetPerformanceCounter` |
| 日志 | `board_puts()` | UART | `stdout` |
| 输入 | `board_keys()` 返回本帧按键位图 | 本条返回 0，按键在 SW9 | SDL 键盘 |
| 显示 | `board_fb(i)`、`board_present(i)`、`board_wait_frame()` | 本条只返回帧缓冲指针、计数，不上屏（等 display） | SDL 纹理（SW4 补全双缓冲语义） |
| 缓存 | `board_dcache_invalidate_all()`、`_range()` | 按 E3 规则包成带 `"memory"` clobber 的函数 | 空操作 |

`board_region` 是为 PC 版准备的：PC 上没有固定的物理地址，应用只能拿到区域指针。
帧缓冲的行跨 4096 字节在两边一致（spec 第 01 章第 7 条）。
「不用 `malloc`」约束 `lib/` 与应用；`bsp/host` 分配模拟内存不受这条限制。

### 2.3 构建

**板上 `mk/target.mk`**，应用的 makefile 只写几行：

```make
APP      = gfx2dGame
APP_SRCS = $(wildcard src/*.c src/game/*.c)
LIBS     = core
include ../mk/target.mk
```

- 引入 sdk 的 `bsp.mk` 与 `riscv64-unknown-elf.mk`，拿到 `-march`、`-mabi`、sdk 头文件路径与工具链前缀；
  **不引入 `standalone.mk`**，编译规则自己写，目标文件按源文件的相对路径放（`$(OBJDIR)/lib/core/crc32.o`）。
  链接参数照抄 `standalone.mk` 的那一行（`-specs=nosys.specs -nostartfiles -ffreestanding -lc -lgcc -lm`），并链接 sdk 的 `start.S`。
- 链接脚本用 `bsp/ti60/linker/gfx2d.ld`：以 sdk 当前的 `default.ld`（含已登记的 4092K 与 512KB 断言）为底本，
  文件头写明底本来源与差异；栈改为 64KB。
- 优化级别由 `OPT` 指定，默认 `-O2`，产物名带上它；`DEBUG=no`，不吃 sdk 的 `-Og` 默认值。
  `EXTRA_CFLAGS` 是留给一次性实验的口子（例如 `-DFRAMES=12000` 做长跑），参与编译选项标记的比较。
- **头文件依赖要显式建**：编译时加 `-MMD -MP`，gcc 为每个目标文件写一份 `.d`，makefile 末尾 `-include` 进来。
  没有它时改头文件不重编，旧的目标文件被悄悄链进产物；另外要把默认目标写死成 `all`
  （`.DEFAULT_GOAL := all`），否则 `.d` 里的目标会抢走默认目标，`make` 只重编目标文件而不链接。
  两处的经过见 TechReports sw 第 01 章 4.8。
- 头文件路径按层给：编 `lib/` 只有 `-Isw/lib`；编 `bsp/ti60` 有 `bsp/include` 与 sdk；编应用有 `lib` 与 `bsp/include`，**没有 sdk**。
  应用想直接调 sdk 驱动时会编不过，这正是要的效果。sdk 的路径只出现在 `bsp/` 那一层的规则里。

**PC `mk/host.mk`**：同样按层给头文件路径，`bsp/host` 额外加 `sdl2-config --cflags`。
`lib/test/` 另编两份：默认 64 位一份，`-m32` 一份，`-m32` 依赖 `gcc-multilib`（已装，见第 1 节）。

两边都用 `-std=gnu11 -Wall -Wextra -Werror`，PC 另加 `-Werror=double-promotion -Werror=float-conversion`，
测试与调试构建加 `-fsanitize=address,undefined`。

### 2.4 最小应用

`gfx2dGame/src/main.c`：`board_init` 之后跑 60 帧，每帧向 `board_fb(i)` 写一个以帧号为种子、由 xorshift32 生成的图案，
用 `lib/core` 的 CRC32 算整帧，经 `board_puts` 打印帧号、CRC 与耗时，最后打印 `done`。
PC 上同时在窗口里显示。

这里直接写像素只是骨架，游戏正式的画面一律经命令表（总览约束 4）。
这 60 个 CRC 同时是**第一条跨平台判据**：它们只取决于 `lib/core` 的算法与帧缓冲布局，PC 与板子必须逐帧相同。

### 2.5 检查脚本

`Main/tools/check/check_sw_layers.sh` 把结构判据集中到一处，提交前跑：

1. `lib/` 在只有 `-Isw/lib` 的条件下编译通过（主机 gcc 即可）；
2. 注错：临时生成一个 `#include "board.h"` 的 `lib/` 文件，编译必须失败；另一个 `#include <soc.h>` 的应用文件，编译也必须失败。
   **这一项与下面第 3、4 项都跑真实的板上构建**（xPack 交叉工具链加 `mk/target.mk`，产物落临时目录），
   不用主机 gcc 加手写的 `-I` 去模拟；另加一个对照组：合法的 `board.h` 与 `lib` 头文件必须编得过。
   理由见 TechReports sw 第 01 章 4.6 节——第一版用模拟的 `-I` 检查，判据通过而真实构建里分层没有生效；
3. 注错：在两个不同目录放同名的 `x.c`，板上构建能正确链接两份符号（两份 `.o` 都在，ELF 里两个符号都在）；
4. `nm` 扫 `lib/` 与应用的目标文件，没有 `__adddf3`、`__mulsf3` 之类的软浮点符号；注错：临时放一个 `float` 运算，必须报出来；
5. `git diff --quiet HEAD -- Main/sdk`，本条及以后的非同步提交里 sdk **生成物**无改动；
   放行 `Main/sdk/README.md`——它本身不是生成物，而是登记「重新生成后要恢复哪些改动」的那个文件，
   2.6 节还要改它。实现时发现原判据与 2.6 节相冲突，已按本意收窄，见 TechReports sw 第 01 章 4.4；
6. 改了头文件必须触发重编**并重链**：先建一次，`touch` 一下 `bsp/include/memmap.h` 再建，
   输出里要同时有 `CC main.c` 与 `LD`；对照：紧接着再建一次，两者都不该出现。
   这一项是用户复核时发现的缺陷补上的（4.8 节），只查「重编了」不够——
   第一版修法重编了但不链接，产物照样不更新；
7. 缓存行区间折算：`bsp/include/dcache_line.h` 的 `static inline` 是纯算术，
   判据脚本用主机 gcc 编一个探针包含**真实的这段代码**跑八个算例，
   第一个就是报的缺陷（`0x3F` 起 2 字节要覆盖 `0x00` 与 `0x40` 两行，见 4.9 节）。
   它放在 `bsp/include` 而不是 `board.c` 里，就是为了让这一项能在主机上验证。

### 2.6 回改文档

- 设计文档 4.4 节的分层图与最后一条（原写「程序放 `sdk/software/standalone/` 下」）改为本条的结构。
- `sw/README.md`：加分层说明、两种编译命令、`gfx2dGame` 的期望输出。
- `sdk/README.md` 与 `Main/README.md` 的「约定」：加「sdk 只读，项目对 sdk 的需求放 `sw/bsp/` 覆盖」。
  （`sdk/README.md` 是项目写的文件，不是 IP 生成物，所以改它不违反「sdk 只读」。）
- `doc/开发环境.md`：补 PC 侧依赖（`libsdl2-dev`、`gcc-multilib`）与 WSLg 的检查方法。

本条**不含**：

- 命令表与执行器（SW3）、PC 的双缓冲与录像（SW4）、板上按键与显示（SW9）。
- 六个已有诊断程序迁到新构建：等它们下次要改时顺手迁移。
- 把 sdk 里已登记的 bootloader 修改挪出去：bootloader 的配置头文件用引号引入，头文件路径盖不掉，
  要把源文件复制到 `sw/` 下单独构建。挪完之后 `sdk/` 可以完全保持官方原样，另开条目。

## 3 判据

| 项 | 判据 |
| --- | --- |
| PC 构建 | `wsl -e make -f mk/host.mk` 零告警；运行出现 640×480 窗口，打印 60 行后 `done`，退出码 0 |
| 板上构建 | xPack gcc 8.3.0 零告警；bin 小于 512KB；bin 大小、SHA256、编译命令登记 |
| 板上运行 | 串口输出 60 行帧号、CRC、耗时，末尾 `done`；原始日志入 `sw/apps/game/gfx2dGame/logs/`；日志里记明所用比特流的 SHA256 |
| 跨平台一致 | 60 个 CRC 板上与 PC 逐帧相同；注错：板上构建把 xorshift32 的一个移位常数改掉，从第 0 帧起不同 |
| 分层 | `check_sw_layers.sh` 七项全过（第 1~5 项为分层与结构，第 6 项头文件依赖、第 7 项缓存行折算），注错四处处处报红、对照两处成立 |
| 构建依赖 | 改一个头文件必须重编**并重链**：`touch bsp/include/memmap.h` 之后输出里要有 `CC main.c` 与 `LD`；没有改动时两者都不出现 |
| 32 位主机测试 | `lib/test/` 在 64 位与 `-m32` 下都通过，输出相同 |
| 链接脚本 | `.map` 里 `.stack` 为 64KB；文件头写明底本与差异 |
| 文档 | 2.6 节四处已改 |

逐项的实测结果与原始输出见 TechReports 的 sw 第 01 章 5.1 节，本文件不再重复。
第 2 项「计时」不在本节：`board_cycles` 在 PC 上的长时间正确性做了一次 200 秒的一次性对照
（TechReports sw 第 01 章 4.10），正式判据归 SW4。

## 4 开发日志

**2026-09-19 二次复核：三处缺陷与两处注释的修复逐项验证通过。**（工具链与比特流哈希不在本轮范围）

| 缺陷 | 判据之外的独立验证 | 结果 |
| --- | --- | --- |
| 头文件依赖 | 仓库外副本里**改内容**（不是 `touch`）：`memmap.h` 的 `FB_HEIGHT` 480→240，板上与 PC 各编一次；再改回；再分别改 `lib/core/xorshift32.h`、`bsp/include/dcache_line.h` | 480→240：`main.c`、`memmap.c`、`board.c` 重编并重链，bin 由 `98fb37c9dc9bc060` 变为 `1c43193b1c6cac0a`，PC 同样重编；改回后 bin 回到 `98fb37c9dc9bc060`；两个头各自只触发依赖它的文件重编；不改动时两边都不编不链 |
| 缓存行区间 | 读 `bsp/ti60/board.c` 的循环：从 `span.first` 起先失效再判 `line >= span.last`，末行包含在内 | 与 `check_sw_layers.sh` 第 7 项的 8 个算例一致 |
| PC 周期溢出 | 用 `-Wl,--wrap` 替换 SDL 计数器，直接构造 dt = 0、1、42.95、184.0、184.4、184.5、184.6、200、1000、36000 秒，与 `dt/10` 按 32 位回绕比对 | 固定版 10 个点全部相等；注错版（`INJECT_HOST_CYCLES_NAIVE`）从 184.5 秒起 5 个点全错，与推算一致 |
| 注释 | 读 `gfx2dGame/makefile` 与 `crc32.c` 文件头 | 产物名与实际一致；`crc32.c` 的理由已改为镜像拷贝量 |

其余：`check_sw_layers.sh` 11 行全 ok、`fail 0`；新产物 `98fb37c9dc9bc060`（注错版 `577cacb50a0b29bd`）
经 `setup.bat` 环境重新烧写比特流后上板，正常版与注错版各 60 帧，与 PC 期望逐帧相同，两者 0 帧相同。

一处保留意见，不阻塞验收：`dcache_line.h` 是 bsp 内部的辅助头，放在 `bsp/include/` 里，应用也能包含它。
这样放是为了让主机能测到它（见文件头）。以后 bsp 内部的头多起来时，可以再分出 `bsp/include/internal/`。

---

**2026-09-19 复核查出的问题已修完，进入待验收。**

三处缺陷的修法与第一现场在 TechReports sw 第 01 章 4.8~4.10，两处注释订正在 4.11。

| 问题 | 修法 | 判据 |
| --- | --- | --- |
| 构建不跟踪头文件 | 编译加 `-MMD -MP`，末尾 `-include` 依赖；默认目标写死 `all` | 第 6 项：`touch memmap.h` 后 `CC main.c` 与 `LD` 都要出现；对照另一半 |
| 缓存行区间漏末尾一行 | 起点与**最后一个字节**都向下对齐到行首，逐行失效；折算放进 `bsp/include/dcache_line.h` 的 inline | 第 7 项：主机探针跑 8 个算例，含 `0x3F` + 2 → `0x00..0x40` |
| PC 计时 184.5 秒后溢出 | 先拆整秒与余数再乘；`board.h` 写明 32 位回绕与 42.9 秒的用法约束 | 12000 帧（约 200 秒）对照：固定版最大 762100 周期、0 帧超限；注错版第 11068 帧出现 3028353532，与 184.5 秒的推算对上 |
| 两处注释与实现不符 | makefile 写清产物名构成与三个开关；`crc32.c` 的理由改成镜像拷贝量 | 注释不参与编译，只能靠改实现时顺手读一遍 |

修 `-MMD` 时又带出第二个缺陷：`-include` 放在 `all:` 之前会让 `.d` 里的目标抢走默认目标，
`make` 只重编目标文件而不链接，症状与原来的缺陷一样是「bin 不变」。
所以第 6 项查两件事（重编**与**重链），只查 `CC` 会漏掉这一半。

改动影响与重跑：`bsp/ti60/board.c` 变了，板上产物从 2426 字节变成 2442 字节
（`98fb37c9dc9bc060`），三个变体的新值登记在 TechReports sw 第 01 章 3.9 节；
**上板结论已按新产物重跑一遍**，四份日志与最终产物对应，`frame 0` 的 CRC 仍是 `cf8e2861`。

同一轮复核里还修了工具侧一处：`board_run.py` 原先只看 `ftdi_pgm.bat` 的退出码，
而它在内部出错（跳过 `setup.bat`、python 起不来）时仍返回 0，
脚本把失败的烧写当成功、继续用 FPGA 里原有的比特流跑。
现在这一步净化子进程环境并改用「输出里出现 `finished with JTAG programming`」判定，
已用假的 `pgm`（返回 0、无成功标记）验证会报错。

---

**2026-09-19 验收复核：判据全部复现，另查出三处缺陷，验收暂缓。**

复现（产物放仓库外的临时目录，不碰 `gfx2dGame/build`）：

| 判据 | 复现结果 |
| --- | --- |
| 板上构建 | 零告警；bin 2426 字节，`f258849742fd0c43`，与已有产物逐位相同；注错版 `987ef5acc3b067e5` |
| 链接脚本 | `.map` 里 `.stack 0x10000` |
| PC 构建与运行 | 零告警；60 帧后 `done`，退出码 0；CRC 与 `logs/pc_normal.log` 逐帧相同 |
| 32 位主机测试 | 64 位与 `-m32` 输出逐字节相同，32 项 ok、0 项 FAIL |
| 分层 | `check_sw_layers.sh` 八行全 ok、`fail 0`，注错文件事后已清干净 |
| 板上运行 | 烧当前 `outflow/gfx2d_ti60.bit`（`d8af4ce13c416834`）后重跑：正常版、注错版各 60 帧，与 PC 期望逐帧相同；正常版与注错版 0 帧相同 |

判据没覆盖到、复核时查出的：

1. **构建没有头文件依赖。** `target.mk` 与 `host.mk` 只按编译选项做了标记文件，没有 `-MMD`。
   在仓库外的副本里把 `memmap.h` 的 `FB_HEIGHT` 从 480 改成 240 再 `make`，只重编了无关的 `xorshift32.c`，
   `main.c` 没重编，bin 仍是 `f258849742fd0c43`：旧的目标文件被静默链进产物。
2. **`board_dcache_invalidate_range` 起点不对齐时漏掉最后一行。** 循环从 `addr` 按 64 字节步进，
   `addr = 0x3F、bytes = 2` 只失效 `0x00` 那一行，`0x40` 那一行漏掉。SW1 只用到 `_all`，没有判据覆盖它。
3. **PC 上 `board_cycles` 约 184 秒后溢出。** 本机 `SDL_GetPerformanceFrequency()` = 1e9，
   `(now - base) × 1e8` 在 2^64 / 1e8 ns ≈ 184.5 s 时溢出 64 位，之后的数值跳变，不是干净的 2^32 回绕。SW1 跑 1 秒碰不到，SW4 之后会碰到。

另有几处小问题：`gfx2dGame/makefile` 注释里写的产物名（`build/gfx2dGame.bin`）与实际（`gfx2dGame-O2.bin`）不符；
`crc32.c` 说查表「放 .bss 比占片上 RAM 划算」，程序在 DDR 里，.bss 同样占 1KB，理由不成立；
TechReports 第 01 章 5.3 节没记录上板所用比特流的哈希。

与本条无关的工具问题：`board_run.py` 不经 Efinity 的 `setup.bat` 直接调用时，`ftdi_pgm.bat` 内部抛
`KeyError`（`EFXPGM_HOME`、`EFINITY_USER_DIR_INI`）却返回 0，脚本把烧写当成功，继续用 FPGA 里原有的比特流跑，
串口输出看不出任何异常。要在 `setup.bat` 设好环境之后调用。

---

**2026-09-19 实现完成，转为待验收。**
按第 2 节的分层把代码写完，判据逐项跑过：PC 与板上两份构建零告警、
`lib/test/` 在 64 位与 `-m32` 下输出相同（`fail 0`）、`check_sw_layers.sh` 五项全过含三处注错、
`.map` 里 `.stack` 为 64KB、板上 60 帧 CRC 与 PC 逐帧相同、注错版从第 0 帧起不同。
四处与预期不符、写进 TechReports 第 01 章第四节的：

1. 板上这份工具链把 `uint32_t` 定义成 `unsigned long`（PC 上是 `unsigned int`），
   printf 风格的变参格式化两端不同型且检查不了，`lib/core` 的格式化改成按类型追加的一组函数；
2. `bsp.mk` 里的 `LDSCRIPT ?=` 先赋值，把项目链接脚本盖掉，第一次编译出来的栈还是 4K，
   靠「`.map` 里 `.stack` 为 64KB」这条判据才发现；
3. 同一个 `OBJDIR` 里 `make INJECT=...` 只重链不重编，注错版与正常版逐位一样，
   加了编译选项标记文件才真正重编——注错用例必须同时证明「不注错时通过」与「注错时确实会红」；
4. 判据 5 与 2.6 节互相否定（`git diff --quiet HEAD -- Main/sdk` 把要一起改的 `sdk/README.md` 也算进去），
   按本意收窄为放行这一个文件；
5. `check_sw_layers.sh` 的注错文件正好落在真实构建的通配符范围里，顺序排错会互相干扰，
   改成用完即删并把顺序固定为「正常构建 → 浮点注错 → 同名注错」；
6. **分层判据通过了而真实构建里分层没有生效**：第一版把 sdk 的 include 路径加在全局 `CFLAGS` 上，
   应用的编译行里也有它们，一个 `#include <soc.h>` 的应用文件照样编过；判据那边用的是主机 gcc
   加手写的 `-I`，两边参数不同。修法是把这组路径只给 `bsp/` 那一层，并把判据改成跑真实构建。
   这一条是本次最值得记的一处，见 TechReports sw 第 01 章 4.6；
7. 板上日志开头混进 Flash 里旧程序（`bootSizeTest`）的输出，比对时按 `frame N crc X` 过滤。

篇幅更长、带第一现场与推导的记述在
[`../../TechReports/sw/01-软件平台分层与双目标构建.md`](../../TechReports/sw/01-软件平台分层与双目标构建.md)，
本文件只留时间流水。

**2026-09-19 原稿审核，重写本条。**

原稿（文件名 `SW1-工程骨架与双目标构建.md`）的结构是 `sw/apps/game/gfx2dGame/src/` 下放 `game/`、`render/`、`exec_sw/`、`common/`、`hal/`。
用户审核指出：`sw/` 下应当只放应用，平台层混在应用目录里不符合裸机工程的分层；
而之所以会这样放，是因为 sdk 会被覆盖，板级代码进不了 sdk。审核中查出的问题：

1. 板级代码只有游戏能用，`execSwTest`、`entityBench` 要么复制，要么跨目录引用另一个应用的内部文件。
2. `hal.h` 把应用层的事（按编号取素材）和硬件服务放在同一层。
3. 分层判据靠 grep，注错用例（在 `game/` 里 include SDL.h，看板上构建会不会失败）必然变红，证明不了任何事。
4. 原稿要求两边 `-std=c11`，sdk 的 `riscv.h` 用裸 `asm`，板上会直接编不过。
5. 原稿沿用官方 makefile、只加子目录的 `wildcard`，会撞上 `notdir` 的同名冲突。
6. 原稿没有处理链接脚本的 4K 栈与 `-Og` 默认值。

重写后改为第 2 节的分层：sdk 只读，项目板级层 `sw/bsp/` 叠在上面，纯算法放 `sw/lib/`，应用只写应用；
分层由各层的头文件路径保证。总览、SW3、SW4、SW5、SW7、SW9 与 E7 里引用原路径的地方同步改了。
