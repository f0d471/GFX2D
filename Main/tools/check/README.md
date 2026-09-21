# check：提交前检查与规格校验

| 文件 | 用途 | 什么时候跑 |
| --- | --- | --- |
| `localize_paths.py` | 展开、还原厂商生成物里的绝对路径；扫描整个仓库里的机器专属路径 | 克隆后 `--apply`；提交前 `--restore` 再 `--check` |
| `check_sw_layers.sh` | 软件分层与构建的结构判据，七项，含三处注错 | 改了 `sw/` 的结构、构建文件或 `bsp/include` 时，提交前 |
| `check_round255.c` | 逐像素规格（设计文档 3.5 节）的穷举校验：舍入、中间量位宽、舍入顺序、预乘量化误差 | 改像素规格时；v1 不实现 tint，后续 profile 启用时再加对应穷举 |
| `check_cmd_semantics.py` | 命令语义规格（设计文档 3.4 节、spec 第 02 章）的结构校验与 159 条一致性向量（ABI 1），带六项注错自检 | 改命令 ABI 或错误模型时，提交前 |
| `replay2c.py` | 录像（`.g2dr`）与 C 数组、录制文本之间的转换；`--check` / `--selftest` 做结构校验与文本往返 | 改录像格式时；`mk/host.mk test` 与 `check_sw4_pc.sh` 会调它 |
| `check_sw4_scale.c` | PC 平台层「缩放用最近邻」的主机侧探针：把 8×8 棋盘放大 80 倍再读回边界像素 | 改缩放或像素格式时 |
| `check_sw4_pc.sh` | PC 平台层（SW4）的提交前判据：整帧用例对 `golden.txt`、有窗与无窗逐行互比、注错构建、录像文本回灌、缩放探针 | 改 `sw/bsp/` 或 PC 侧构建时，提交前 |

## localize_paths.py

```
wsl -e python3 Main/tools/check/localize_paths.py --apply      占位符 -> 本机路径，克隆之后跑一次
wsl -e python3 Main/tools/check/localize_paths.py --restore    本机路径 -> 占位符，提交之前跑一次
wsl -e python3 Main/tools/check/localize_paths.py --check      扫描整个仓库，发现机器专属路径就以非零退出码结束
```

有六个厂商生成的文件里必须写绝对路径：`fpga/gfx2d_ti60/ip/soc/settings.json`、`ip/soc/source/soc_config`、
gpioDemo 的 `.cproject` 与三个 `.launch`，仓库里存的是占位符形式。占位符代表 `Main/` 的路径。
在 WSL 里运行时，写进去的仍是盘符路径，因为读这些文件的是 Windows 上的厂商工具。

`--check` 找盘符路径、`/home/<用户>/`、`/mnt/<盘符>/` 三类写法，扫的是整个 git 仓库（`Main/` 与 `Engineering/`）。
不扫：不入库的厂商资料包 `Efinix/`，厂商代码与 IP 生成物（它们带生成时那台机器的工具安装位置），编译产物。
豁免登记在脚本的 `ALLOW` 里，现有三项：`doc/开发环境.md`（第 11 节集中记本机取值）、本脚本、
platform 第 07 章（原样引用的报错输出里带 Efinity 的安装路径）。

## check_sw_layers.sh

```
wsl -e bash -c 'RISCV_BIN=<RISCV_BIN> bash Main/tools/check/check_sw_layers.sh'
```

`RISCV_BIN` 是交叉工具链前缀（`doc/开发环境.md` 第 1 节占位符），不给时取 `riscv-none-elf-`，要在 PATH 里。
第 2、3、4、6 项跑真实的板上构建，第 1、7 项用主机 gcc。判据与被测构建必须用同一组参数，
理由见 [`../../../Engineering/TechReports/sw/01-软件平台分层与双目标构建.md`](../../../Engineering/TechReports/sw/01-软件平台分层与双目标构建.md) 4.6 节。

| 项 | 内容 |
| --- | --- |
| 1 | `lib/` 在只有 `-Isw/lib` 的条件下编译通过（主机 gcc） |
| 2 | 注错：`lib/` 里 `#include "board.h"` 必须编不过；应用里 `#include <soc.h>` 必须编不过。对照组：合法的 `board.h` 与 `lib` 头文件必须编得过 |
| 3 | 注错：`lib/core` 与 `bsp/ti60` 各放一个同名的 `x.c`，板上构建必须产出两份 `.o` 且链接出两份符号 |
| 4 | `nm` 扫 `lib/` 与应用的目标文件没有软浮点符号；注错：临时放一个 `float` 运算必须被扫出来 |
| 5 | `git diff --quiet HEAD -- Main/sdk`，放行 `Main/sdk/README.md` |
| 6 | 改了头文件必须重编**并重链**（`touch bsp/include/memmap.h` 后要出现 `CC main.c` 与 `LD`）；对照：没有改动时两者都不出现 |
| 7 | 缓存行区间折算：主机探针包含 `bsp/include/dcache_line.h` 跑八个算例，含 `0x3F` + 2 → `0x00..0x40` |

注错文件只在运行时临时生成，脚本退出（含中途失败）一律删干净；编译产物落临时目录，不动应用的 `build/`；
第 6 项会 `touch` 一次 `bsp/include/memmap.h`（只改时间戳）。全部通过时末尾是 `fail 0` 与 `done`，退出码 0。

## check_round255.c

```
wsl -e bash -c 'gcc -O2 -o /tmp/check_round255 Main/tools/check/check_round255.c && /tmp/check_round255'
```

纯整数，不依赖板子。把设计文档 3.5 节方程表的六个方程逐个代入，穷举比较「两项各自舍入再相加」与
「相加后一次舍入」两种写法，另校预乘与非预乘两条路径相对实数定义的量化误差。全部符合时打印「总判定：全部通过」，退出码 0。
推导与原始输出见 `Engineering/TechReports/spec/01-逐像素规格.md`。

## check_cmd_semantics.py

```
wsl -e python3 Main/tools/check/check_cmd_semantics.py                 校验，含注错自检
wsl -e python3 Main/tools/check/check_cmd_semantics.py --dump-matrix   打印规范的可执行镜像，供与正文逐行对账
wsl -e python3 Main/tools/check/check_cmd_semantics.py --emit F         把向量集写到 F（C 与 Verilog 共用）
```

只用标准库，不依赖板子、编译器与 SDL。把设计文档 3.4 节与 spec 第 02 章的字段矩阵、检查优先级、
错误码、方程/格式匹配规则抄成一份可执行镜像，据此生成第 02 章规范条款 §7 的九组共 159 条一致性向量
（ABI 1），逐条比对错误码、零基索引、`w7` 标签、取记录条数、clip 状态与内存内容。
规范正文是权威；应当先改正文，再同步这份镜像。导出格式用 `[list]` 保留多次提交边界。

期望值是**手写并注明出处**的，检查器是另一份实现；两者不一致即 FAIL。这是 spec 第 01 章 4.1、4.7
两节那条教训的落地：工具不能自己判自己，两个实现也不能共享同一个错误。`(A)`~`(D)` 是结构审计
（优先级十步各有反例、字段矩阵的零集与反例集相等、18 个方程×格式组合逐个有向量、八类描述符谓词
各有失败例，其中控制字三个保留段分列，共 10 个失败位点），`(F)` 列出前期研究补入正文并经复核定案的条目，`(G)` 是六项注错自检——把每条
规则改错，向量集必须见红；`(H)` 独立读回导出的 vector/list/cmd 层，核对多命令表边界与每表
`COUNT`。全部通过时末尾是 `fail 0` 与 `done`，退出码 0。

结论、缺口与原始输出见 `Engineering/TechReports/spec/02-命令语义.md` 第三至第五节，
向量集的文本格式见同章规范条款 §7.2。

## replay2c.py

```
wsl -e python3 Main/tools/check/replay2c.py --in F.g2dr --out H.h --name replay_fixture   二进制 -> C 数组
wsl -e python3 Main/tools/check/replay2c.py --from-text run.log --out F.g2dr              录制日志 -> 二进制
wsl -e python3 Main/tools/check/replay2c.py --in F.g2dr --to-text                         二进制 -> 录制文本
wsl -e python3 Main/tools/check/replay2c.py --gen-test --frames 256 --seed 7 --out F.g2dr  合成一份固件
wsl -e python3 Main/tools/check/replay2c.py --check F.g2dr                                 结构校验
wsl -e python3 Main/tools/check/replay2c.py --selftest                                     文本往返与坏输入自检
```

只用标准库，不依赖板子、编译器与 SDL。格式的权威是 `sw/lib/core/replay.h`（sw 第 03 章 3.2 节的 D6）：
头部 16 字节记魔数 `G2DR`、版本、标志、种子与帧数，之后每帧一个 32 位按键位图，全部小端、
定长、无对齐洞。**逐字节读写，不把缓冲强制转成结构体**，所以 LP64 与 ILP32 解出同一串按键。

脚本进判据路径，不是一次性生成器：`mk/host.mk test` 每次都用它把入库的
`lib/test/replay_fixture.g2dr` 转成 C 数组，主机测试再核对「文件解析」与「C 数组解析」
两条路给出的按键序列逐帧相同。`--selftest` 另做四组文本往返与七项坏输入。

## check_sw4_scale.c

```
wsl -e bash -c 'gcc -std=gnu11 -O2 -Wall -Wextra -Werror -o /tmp/check_sw4_scale \
    Main/tools/check/check_sw4_scale.c $(sdl2-config --cflags) $(sdl2-config --libs) && \
    SDL_VIDEODRIVER=dummy /tmp/check_sw4_scale'
```

把 8×8 的黑白棋盘放大 80 倍铺满 640×480，用 `SDL_RenderReadPixels` 读回第 40 行：
最近邻下第 79、80 两个相邻像素是黑白硬边，线性插值下边界两侧都是中间值。
`SDL_HINT_RENDER_SCALE_QUALITY` 的默认值是**未设**，所以判据先确认这一点，再确认设 `"0"`
与设 `"1"` 结果不同。dummy 驱动下也能跑，不需要显示。全部通过时末尾是 `fail 0` 与 `done`。

## check_sw4_pc.sh

```
wsl -e bash -c 'bash Main/tools/check/check_sw4_pc.sh'
wsl -e bash -c 'SW4_QUICK=1 bash Main/tools/check/check_sw4_pc.sh'     少跑几帧，改代码时用
```

PC 平台层（[sw 第 03 章](../../../Engineering/TechReports/sw/03-PC平台层与帧节拍.md)）5.2 节里「要跑多次再比」的几条：
第 1 条用例 CRC 对 `lib/test/golden.txt`、第 8 条有窗与无窗用**同一个二进制**跑两次再逐行比、
第 7 条把录制文本灌回 `.g2dr`、第 2 条跑缩放探针，另跑一次注错构建
（`INJECT=REPLAY_CLOCK`，画面依赖时刻之后两次回放必须出现不同，用来证明「两次相同」不是空转）。
其余几条由判据程序 `sw/apps/test/platformTest` 自己判，脚本核它的 `fail =0`。

全量一次约四分钟（判据程序每遍 600 帧节拍加 1800 帧回放两遍）；`SW4_QUICK=1` 把三段帧数压到
60/30/180。日志行尾是 `\r\n`（与板上 UART 逐字节相同的约定），脚本比对前先做 CRLF 归一。
