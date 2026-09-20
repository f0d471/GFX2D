# check：提交前检查与规格校验

| 文件 | 用途 | 什么时候跑 |
| --- | --- | --- |
| `localize_paths.py` | 展开、还原厂商生成物里的绝对路径；扫描整个仓库里的机器专属路径 | 克隆后 `--apply`；提交前 `--restore` 再 `--check` |
| `check_sw_layers.sh` | 软件分层与构建的结构判据，七项，含三处注错 | 改了 `sw/` 的结构、构建文件或 `bsp/include` 时，提交前 |
| `check_round255.c` | 逐像素规格（设计文档 3.5 节）的穷举校验：舍入、中间量位宽、舍入顺序、预乘量化误差 | 改像素规格时；v1 不实现 tint，后续 profile 启用时再加对应穷举 |
| `check_cmd_semantics.py` | 命令语义规格（设计文档 3.4 节、spec 第 02 章）的结构校验与 159 条一致性向量（ABI 1），带六项注错自检 | 改命令 ABI 或错误模型时，提交前 |

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
