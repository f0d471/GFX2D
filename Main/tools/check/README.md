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
| `check_sw5_assets.sh` | 素材流水线（SW5）的提交前判据：打包确定性与输入边界、独立实现对账、装载与画面、七种坏包注错、注错构建、镜像回环与 active/passive 边界、许可台账 | 改素材、打包工具、`assetpak`、`assetTest` 或镜像工具时，提交前 |
| `check_sw5_pak.py` | 素材包的**独立实现**：自己解包、自己从 PNG 与字库文本重算像素字节再逐字节比 | 由 `check_sw5_assets.sh` 调用，也可单独跑 |
| `check_sw6_demo.sh` | 基础 Demo（SW6）的提交前判据，十项：录像可重生成、判据模式编过并跑通、对期望日志、有窗与无窗、两遍一致、HUD 与字形、场景 B、`off` 与 `eq` 计数、四个方程的注错见红、Sanitizer 与板上目标构建 | 改 `gfx2dGame`、`lib/gfx` 的执行器、SW5 素材或录像时，提交前 |
| `check_sw6_log.py` | SW6 判据日志的**外部核对**：两遍比对、`hud` 对上一帧统计、`hcrc` 按字库文本独立重画、`s` 与 `scene` 参考值、`off` 与 `eq`；`--board` 把板上串口日志从 `pak` 行起与期望日志对账 | 由 `check_sw6_demo.sh` 调用；板上对账时单独跑 |
| `gen_sw6_replay.py` | 生成 SW6 的两段确定性录像（`.g2dr`）：A 把 N 从 100 升到 5000 再回到 100，B 把 k 从 30 降到 1 再升到 120；形状与停留段写在脚本里 | 录像要改时；`check_sw6_demo.sh` 每次重跑并逐字节比 |
| `check_sw7_game.sh` | 游戏本体（SW7）A、B、C 三批的提交前判据：正弦表重生成比对、主机测试 64/32 比对与 sanitizer、数值基础的四个注错见红、游戏代码的 app 测试（64/32、不链 bsp）与实体池的两个注错见红、批量提交的三个注错见红、弹型期望文件的重生成与逐帧比对及三个注错见红、碰撞的两个注错见红、组帧、弹幕场判据模式两遍一致、板上构建与六个逐颗循环的反汇编（含三个注错）、自机与规则（R 组）及三个注错见红、敌机 Boss 与关卡（S 组）及两个注错见红、霓虹素材可重生成（含注错）、游戏的判据入口两遍一致与 HUD 的注错 | 改 `lib/core/fixmath`、正弦表、`src/game/stg_*`、`lib/gfx/list.c` 的批量提交、`mk/host.mk` 的 `test` 目标时，提交前 |
| `check_sw8_stats.sh` | 统计埋点（SW8）A、B 两批的提交前判据：主机测试 64/32 比对与 sanitizer、普查与软件执行器对账、模拟时钟、帧时间直方图、日志队列、`board_tx_room()` 的 PC 实现与板上构建、`N_max` 状态机的综合负载判据、搜索跑在弹幕场上（app 测试）与 `STG_FIELD=3` 的 PC 驱动、各注错见红 | 改 `lib/gfx/census`、`lib/core/framehist`、`lib/core/logq`、`lib/core/nmax`、`board_tx_room()`、`src/game/stg_press`、`mk/host.mk` 的 `test` 目标时，提交前 |
| `check_sw8_tx.c` | `board_tx_room()` 的 PC 实现的主机探针：链真实的 `sw/bsp/host/board.c`，不设 `GFX2D_TX_BUDGET` 时不限，设 192 时按场补满、不累加、超额计数；`-DINJECT_HOST_TX_NO_REFILL` 见红 | 由 `check_sw8_stats.sh` 的 M5 编译运行；改 `bsp/host` 的串口模拟时 |
| `gen_sin_tab.py` | 生成 `sw/lib/core/sin_tab.c`：1024 项加 256 项尾巴的 Q1.14 正弦表，按绝对值四舍五入；`--inject` 改一项给比对当注错 | 表的口径要改时；`check_sw7_game.sh` 每次重跑并逐字节比 |
| `gen_stg_patterns.py` | SW7 弹型判据的**独立实现**：照 spec 另写正弦表、atan2、`xorshift32`、发射器与池，生成五种弹型加合并段各 120 帧的期望文件 `sw/apps/game/gfx2dGame/test/stg_patterns.txt`；`--compare` 与 C 的轨迹先比参数行再逐帧比，不一致时两边各 dump 第一个不同帧、指出第一颗不同的弹 | 弹型参数或 2.1、2.3 节的规则要改时；`check_sw7_game.sh` 每次重跑并逐字节比 |
| `e7_key_probe.c` | 独立枚举受控棋盘纹理的色键命中数，与软件执行器统计对账 | E7 改色键负载或解释命中率时 |
| `e8_entity_golden.c` | 用同一份实体算法生成九组主机侧期望值到 `entityBench/src/golden.h`：每组 1000 帧的六字段状态 CRC 与 C 组每帧全部 `32×N` 命令字节 CRC，另保留终态摘要 | 改 E8 实体算法、命令格式或帧数时，重生成并随源码同步提交 |
| `e7_output_check.py` | E7 性能负载的**独立输出复算器**：按 spec 第 01 章的混合方程与 `list.c` 的位域，从初始帧缓冲与纹理重画一帧，逐项核对板上整帧 CRC32 和每个可见像素的 32 位加和；要求两面各九项恰好 18 行、无重复或未知项，且有 `fail 0` 和 `done` | 判定 E7 吞吐数字有效性的硬判据；板上日志格式变了（`<名字> fbN ... crc 0x... pxsum 0x...`）要同步改 `parse_log` |
| `e8_entity_abi.c` | 独立调用 `gfx_blit`，对 64 个图集索引及双记录 w7 标签逐字节检查 E8 命令打包符合 BLIT ABI，另查 8 个 x/y 出屏边界及 AoS/SoA 一致性 | 改 E8 命令生成、图集映射或 gfx ABI 时 |

## localize_paths.py

```
wsl -e python3 Main/tools/check/localize_paths.py --apply      占位符 -> 本机路径，克隆之后跑一次
wsl -e python3 Main/tools/check/localize_paths.py --restore    本机路径 -> 占位符，提交之前跑一次
wsl -e python3 Main/tools/check/localize_paths.py --check      扫描整个仓库，发现机器专属路径就以非零退出码结束
```

有六个厂商生成的文件里必须写绝对路径：`fpga/gfx2d_ti60/ip/soc/settings.json`、`ip/soc/source/soc_config`、
gpioDemo 的 `.cproject` 与三个 `.launch`，仓库里存的是占位符形式。占位符代表 `Main/` 的路径。
在 WSL 里运行时，写进去的仍是盘符路径，因为读这些文件的是 Windows 上的厂商工具。

`--check` 找盘符路径、`/home/<账户名>/`、`/mnt/<盘符>/` 三类写法，扫的是整个 git 仓库（`Main/` 与 `Engineering/`）。
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
**开头先跑一次 `assets/pack_assets.py`**：`gfx2dGame` 从 SW6 起包含生成物 `assets.h`，
干净克隆里不先打包，第 2、3、4、6 项那几次真实构建会因为找不到 `assets.h` 而失败（那是假红）。

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

## check_sw5_assets.sh

```
wsl -e bash -c 'bash Main/tools/check/check_sw5_assets.sh'
```

素材流水线的结构判据，九项，见 [`SW5-素材流水线.md`](../../../Engineering/Todo/sw/SW5-素材流水线.md) 第 3 节：

| 项 | 内容 |
| --- | --- |
| S1 | 打包确定性与输入边界：同一 manifest 打两次，`assets.pak` 与 `assets.h` 逐字节相同；字库行首 `#`、非 PNG 内容、大小写宏碰撞三个反例符合预期；六张占位 PNG 与生成器逐像素一致 |
| S2 | 独立实现对账：`check_sw5_pak.py` 自己解包、自己重算像素字节 |
| S3 | `assetTest` 跑通：装载 → 解析 → 与 `assets.h` 对账 → 挑条目登记表面 → 软件执行器画一整帧；C 侧逐项 CRC 行与独立实现 diff |
| S4 | 整帧 CRC、窗口 CRC 与 64×24 字符画对入库参考日志（`sw/apps/test/assetTest/logs/pc_normal.log`） |
| S5 | 七种坏包各自报指定的错误码：数据翻一位报条目 ID、magic、目录、文件比总长短、重叠、越界、头写 6 MiB 总长（必须在读正文前拒） |
| S6 | 注错构建 `INJECT=ASSET_SKIP_CRC`：关掉两轮 CRC 之后同一个坏包不再被拒，且画面 CRC 与参考不同——证明 S5 不是空转 |
| S7 | 镜像：`make_flash_image.py` 合成后解回，三个分区逐字节一致、区外全 `0xFF` |
| S8 | 镜像边界：输入末字节是 `0xFF` 时必须保留；超预算与越过 4 MiB 物理上限都必须报错退出；passive `.bit` 不得作为 active `.hex` 接受 |
| S9 | 许可：`Main/assets/` 下每个非生成文件都在 `LICENSES.md` 里登记 |

第 5 项里「文件比总长短」那一条落在**读入口**上：PC 从文件读，文件不够长时读不满声明的长度，
报的是板级读错误 `board_read`（`-2`）。解析器自己的 `bad_total` 由主机侧单测
（`sw/lib/test/test_assetpak.c` 的 B09/B26）与第 5g 条覆盖。不需要板子与交叉工具链；
板上那一条（Flash 启动 → 读出长度与整包 CRC → 画出已知图块 → 破坏一项后拒绝并报 ID）
需在板上单独执行，见 SW5 第 3 节。

## check_sw5_pak.py

```
wsl -e python3 Main/tools/check/check_sw5_pak.py [--pak <文件>] [--assets <目录>]
```

**另一份实现**，故意不复用 `pack_assets.py` 的任何函数：它自己按 `assetpak` 的字节格式解头与目录、
自己按 manifest 从 PNG（含预乘、直通、色键三种口径）与字库文本重算每一项的像素字节，
再与 `.pak` 逐字节比；也逐项核 CRC、行尾为 0、64 字节对齐、不重叠。
输出与 `assetTest` 的 C 侧逐项行同格式，所以 `check_sw5_assets.sh` 能把两边 diff 一下——
两边都算对，才说明格式没写歪。

## check_sw6_demo.sh

```
wsl -e bash -c 'RISCV_BIN=<RISCV_BIN> bash Main/tools/check/check_sw6_demo.sh'
wsl -e bash -c 'RISCV_BIN=<RISCV_BIN> SW6_QUICK=1 bash Main/tools/check/check_sw6_demo.sh'      每段每遍只跑 120 帧
```

基础 Demo（[SW6](../../../Engineering/Todo/sw/SW6-基础Demo.md)）的十项结构判据。开头先跑
`assets/pack_assets.py`（`assets.h` 与 `assets.pak` 是生成物、不入库）。默认构建是**演示模式**，
所以每一项都用 `EXTRA_CFLAGS=-DSW6_JUDGE=1` 构建判据模式；`SW6_QUICK=1` 时另加
`-DSW6_FRAMES=120` 并把期望日志换成 `logs/sw6_judge_quick.log`。

| 项 | 内容 |
| --- | --- |
| D1 | 录像可重生成：`gen_sw6_replay.py` 重打两份 `.g2dr` 与入库的逐字节相同；`replay2c.py --check` 独立解析 |
| D2 | `make PLATFORM=host` 编过；判据模式（有窗）跑一遍，`^fail 0`、`^done`、退出码 0。D2b：演示模式加帧数上限编过并冒烟，A、B 两段都出现。D2c：双缓冲不变量的反例 `INJECT=SW6_FB_PER_PASS`（按每遍帧号选面、每遍 121 帧）必须打出 `fb err` |
| D3 | 去掉 `cyc` 三列后与入库期望日志逐行相同；快速档的逐帧行按段按遍是全量档的前缀（`eq A` 除外，它是这一遍实际跑过的帧的累计） |
| D4 | 同一个二进制在 `SDL_VIDEODRIVER=dummy` 下再跑一遍，逐字节相同 |
| D5 | 每段 `p 1` 与 `p 2` 逐帧相同；反例 `INJECT=SW6_LIVE_HUD` 必须见红 |
| D6 | `hud` 列等于同一遍上一帧的 `cmd`／`pix`（每遍首帧 `- -`）；`hcrc` 等于按字库文本与本行字段独立重画的 CRC；反例 `INJECT=EQ_COPY` 见红 |
| D7 | 三个 `scene` 参考值两两不同；每个 B 帧的 `scrc` 等于本帧 `s` 那张图；k=1 段内 `s` 逐帧轮换且三张都轮到；反例 `EQ_COPY` 见红 |
| D8 | A 帧 `off ≥ 4`；`eq A` 的 copy／alpha／add、`eq P` 的 copy／alpha／straight 都非 0 |
| D9 | `EQ_COPY`／`EQ_ALPHA`／`EQ_ADD`／`EQ_ALPHA_STRAIGHT` 各构建运行一次，每次与快速档期望日志至少一行不同 |
| D10 | `SAN=1` 跑一遍，ASan／UBSan 无报告、`fail 0`；另编判据与演示两种板上构建，零告警且 bin ≤ 524288 字节。交叉工具链装在 WSL 里、不在 PATH 上，要传 `RISCV_BIN=<RISCV_BIN>`（`../../doc/开发环境.md` 第 11 节）；不传时找不到就跳过并提示，**跳过不算通过**，验收前必须带上它跑 |

全量一次约 2 分 40 秒，`SW6_QUICK=1` 约 2 分 10 秒（2026-09-24 实测，带 `RISCV_BIN`，含 D2c 与两种板上构建；
D2 与 D4 在全量档各跑 1226 帧，约 22 秒一遍；D5、D9、D10 两档都用快速档的帧数）。
**本脚本不上板**：板上对账按 SW6 第 3 节「板上对账」用 `tools/board/board_run.py --bin`
单独跑判据模式快速档，再用 `check_sw6_log.py --board <串口日志> logs/sw6_judge_quick.log`
对账，原始串口日志入库 `logs/sw6_board_quick.log`。

## check_sw6_log.py

```
wsl -e python3 Main/tools/check/check_sw6_log.py --pair      Main/sw/apps/game/gfx2dGame/logs/sw6_judge.log
wsl -e python3 Main/tools/check/check_sw6_log.py --hud  <日志> --font Main/assets/font/ascii8x16.txt
wsl -e python3 Main/tools/check/check_sw6_log.py --scene <日志>
wsl -e python3 Main/tools/check/check_sw6_log.py --counts <日志>
wsl -e python3 Main/tools/check/check_sw6_log.py --compare <期望> <实际>
wsl -e python3 Main/tools/check/check_sw6_log.py --quick-prefix <全量> <快速档>
wsl -e python3 Main/tools/check/check_sw6_log.py --board <串口日志> <期望>
```

`--board` 给板上对账用。`board_run.py` 的串口日志里还有三样别的东西：它自己的 `#` 标记行、
加载之前 Flash 里旧程序在 `reset halt` 之前打的几行，以及常常抢在「程序已加载」标记之前出现的
本程序横幅。所以它去掉 `#` 行与空行，取**最后一个** `gfx2dGame SW6` 横幅之后、从 `pak` 行起的部分，
与期望日志从 `pak` 行起逐行比（去掉 `cyc` 三列）。`--counts` 在一行 `eq` 都没有时报错，不空转。

日志格式的权威是 SW6 的 2.7 节，这个脚本是它的**外部实现**：按字段位置解析 `f` 行
（行里不出现字符本身，按空白切分即可），`hcrc` 那条按 `Main/assets/font/ascii8x16.txt`
与本行的 `n`／`k`／`hud` 字段独立重画整块 HUD 矩形再算 CRC——C 侧与 Python 侧各算一遍，
两边都对才说明 HUD 的路径（字形、位置、数字、`--`、纯黑底）没写歪。
字库解析照 `pack_assets.py` 的规则：`#` 后跟空白才是注释，`########` 是合法像素行（SW5 4.21 第 1 条踩过）。
只用标准库（`zlib.crc32` 与 `lib/core/crc32` 同参，SW5 4.10 节对过账）。

## gen_sw6_replay.py

```
wsl -e python3 Main/tools/check/gen_sw6_replay.py            写进应用的 replay/
wsl -e python3 Main/tools/check/gen_sw6_replay.py --out <目录>
wsl -e python3 Main/tools/check/gen_sw6_replay.py --to-text  打成录制文本（给人看）
```

SW6 两段确定性录像的生成器，形状是 spec 定死的（SW6 2.5 节与 4.11 第 8 条）：
A 停 10 帧、上键 49 次升到 N=5000、停 30 帧、下键 49 次回到 100、停 10 帧，共 246 帧；
B 停 31 帧、下键 29 次降到 k=1、停 30 帧、上键 119 次升到 120、停 10 帧，共 367 帧。
每步「按 1 帧、松 1 帧」，应用按按下沿判，两帧一步。

它是**独立于 `replay2c.py` 的第二份写法**，只按格式写字节；`check_sw6_demo.sh` 的 D1
既用它重生成再逐字节比，也用 `replay2c.py --check` 独立解析一遍。
入库的是 `.g2dr`（源数据），构建时由两个 `mk` 调 `replay2c.py` 转成 C 数组落到 `$(OBJDIR)/replay/`。

## check_sw7_game.sh

```
wsl -e bash -c 'RISCV_BIN=<RISCV_BIN> bash Main/tools/check/check_sw7_game.sh'
```

判据见 `Engineering/Todo/sw/SW7-游戏本体.md` 第 3 节，口径见 2.1~2.6 节与 4.5、4.11、4.17 节。不要板子；
要主机 gcc、`gcc-multilib`、`libsdl2-dev`（`mk/host.mk` 解析时就要 `sdl2-config`）、`python3` 与 Pillow（G10 先打包素材）；
G11 要交叉工具链，找不到时跳过，跳过不算通过。全套在本机约四分钟。

| 项 | 内容 |
| --- | --- |
| G1 | `gen_sin_tab.py` 重打一份，与入库的 `lib/core/sin_tab.c` 逐字节相同；注错 `--inject` 改一项之后比对必须见红 |
| G2 | `mk/host.mk` 的 `test` 目标：退出码 0、64 位（ASan/UBSan）与 `-m32` 输出逐字节相同、`fail 0`；stdout 与 stderr 一起收，里面不能有 sanitizer 报告（`test` 目标本身不看 stderr）；数值基础 F 组 21 行、批量提交 K 组 8 行全 ok |
| G3 | `FX_IDX_TRUNC`、`FX_VEL_Q4`、`FX_ATAN_NOSCALE`、`FX_ATAN_UNIT` 各编一次，指定的那一行必须 FAIL |
| G4 | 游戏代码的 app 测试：`apps/game/gfx2dGame/test/*.c`、`src/game/stg_*.c`、`lib/core`、`lib/gfx` 按 `test` 目标的选项直接编（选项用 `make --eval` 从 `mk/host.mk` 取），64 位挂 ASan/UBSan、`-m32` 各一份，两份输出逐字节相同、`fail 0`、无 sanitizer 报告；不链 bsp |
| G5 | `STG_POOL_STALL`、`STG_POOL_SWAP` 各编一次，指定的那一行必须 FAIL |
| G6 | `BLIT_RUN_W7`、`BLIT_RUN_SXY`、`BLIT_RUN_NO_FALLBACK` 各编一次，指定的那一行必须 FAIL；最后一个要让「源等于目的」那一行见红，证明随机输入真的走到了逐条回退 |
| G7 | 弹型：`gen_stg_patterns.py` 重打期望文件，与入库的 `test/stg_patterns.txt` 逐字节相同；G4 的两份产物各跑 `pat` 打轨迹，64/32 逐字节相同，再用 `--compare` 与期望文件先比参数行、再逐帧比、末行比溢出计数 |
| G7i | `EMIT_SEQ_NOACC`、`EMIT_AIM_SWAP`、`EMIT_RAIN_ORDER` 各编一次，比对必须在指定的段（spiral、aim、rain）第一个报出不一致，并指出第一颗不同的弹 |
| G8 | 碰撞：G4 的输出里 X 组 13 行全 ok（边界、勾股数相切、大坐标差、擦弹一次、受击关掉、已标记跳过、随机对 64 位朴素参考、场里的自机狙段） |
| G8i | `COLLIDE_LE` 让 X1 的相切那一行见红；`COLLIDE_NO_CULL` 让 X3 见红，且 stderr 里有 UBSan 的平方溢出报告 |
| G9 | 按池发出与组帧：G4 的输出里 W 组 12 行全 ok（剔除边界、分段扫 = 一次扫 = 独立重数、源坐标（米弹、小圆弹的颜色号、火花的寿命帧）、六段各 300 帧组帧时拒绝为 0 且普查的 ALPHA 条数 = 发出的敌弹数） |
| G10 | 先打包素材，再以 `EXTRA_CFLAGS=-DSTG_FIELD=2` 编 gfx2dGame 的 PC 版，无窗口（`SDL_VIDEODRIVER=dummy`）跑两遍：去掉 `\r` 之后两遍逐字节相同、`fail 0`、`done`、六段 840 帧、没有 `err` 行 |
| G11 | `STG_FIELD=1`、`2`、`4` 的板上构建零告警；`objdump -d` 查六个逐颗的循环 `stg_pool_step`、`stg_pool_collide`、`stg_pool_draw`、`stg_pool_hit_targets`、`stg_pool_cancel`、`stg_pool_age`：没有 `div`/`divu`/`rem`/`remu`、没有 `__div*`/`__mod*` 调用、没有 `ret` 以外的 `jalr`/`jr`；注错 `STG_LOOP_DIV`、`PHIT_LOOP_DIV`、`AGE_LOOP_DIV` 各在 `stg_pool_step`、`stg_pool_hit_targets`、`stg_pool_age` 里加一次除法，这条检查必须在那个函数上见红。要 `RISCV_BIN`，找不到时跳过，跳过不算通过 |
| G12 | 自机与规则：G4 的输出里 R 组 28 行全 ok（移动、射击节奏、受击与无敌、自动炸弹与火花、手动炸弹的按下沿、粒子溢出、回标题与 HI-SCORE、擦弹得分、自机弹对敌机的边界与随机对 64 位朴素参考、粒子寿命与弹种位图） |
| G12i | `GAME_NO_AUTOBOMB` 让 R4 见红，`GAME_NO_INVINCIBLE` 让 R3 的无敌那一行见红，`PHIT_LE` 让 R9 的相切那一行见红 |
| G13 | 敌机、Boss 与关卡：G4 的输出里 S 组 21 行全 ok（表、移动事件表与出界、槽位、击毁、整局各段开始帧对 SW7 2.5 节的常量另算、射击时走到 CLEAR 与得分另算、Boss 路径与间隙不受伤、压力形态、旋钮加倍时稳态弹数不减、改档之后到达稳态不超过 `SETTLE_MIN`、游戏里的门控、背景） |
| G13i | `CARD_TIMEOUT_GT` 让 S5 的各段开始帧见红，`GATE_NO_ACC` 让 S11 见红（`-m32` 构建，整局跑得快） |
| G14 | 霓虹素材：`Main/tools/assets/gen_stg_neon.py` 连跑两次，12 张 PNG 逐字节相同；`--check` 重新生成与入库的 `Main/assets/neon/` 逐像素相同（比像素不比 PNG 字节：压缩流随 Pillow 与 zlib 的版本变）；注错 `--check --inject` 改一个像素必须见红 |
| G15 | 游戏的组帧与判据入口：G4 的输出里 V 组 5 行全 ok（共用文字绘制与逐字 `gfx_blit` 逐字节相同、3000 帧组帧的普查对账）；`-DSTG_FIELD=4` 的 PC 版无窗口、`GFX2D_UNPACED=1` 跑两遍，日志逐字节相同、`fail 0`、没有 `err` 行、九行 `seg` 齐全（READY、STAGE、BOSS、四张符卡、CLEAR、回到 READY） |
| G15i | `INJECT=STG_LIVE_HUD` 以 `-DSTG_FRAMES=1200` 跑两遍，日志必须不同（实时量进了画面，整帧 CRC 就不可复现） |

编译产物落临时目录（`OBJDIR` 指过去），不动 `sw/build_host`。全部通过时末尾是 `fail 0` 与 `done`，退出码 0。

## gen_sin_tab.py

```
wsl -e python3 Main/tools/check/gen_sin_tab.py              写进 sw/lib/core/sin_tab.c（入库的那份）
wsl -e python3 Main/tools/check/gen_sin_tab.py --out F      写到别处
wsl -e python3 Main/tools/check/gen_sin_tab.py --inject --out F   注错：第 1 项最低位取反
```

只用标准库。`sin_tab[i] = sign(v) × floor(|v| + 0.5)`，`v = 16384 × sin(2πi / 1024)`，
`i` 从 0 到 1023，再把前 256 项接在后面作尾巴（余弦取 `sin_tab[i + 256]`）。
不用 `round()`：它是银行家舍入。生成的 `.c` 不写数组长度，与 `fixmath.h` 里带长度的声明对不上时编译报错。

## gen_stg_patterns.py

```
wsl -e python3 Main/tools/check/gen_stg_patterns.py                  写进 sw/apps/game/gfx2dGame/test/stg_patterns.txt（入库的那份）
wsl -e python3 Main/tools/check/gen_stg_patterns.py --out F          写到别处
wsl -e python3 Main/tools/check/gen_stg_patterns.py --dump SEG FRAME   某段某帧之后的全部敌弹
wsl -e python3 Main/tools/check/gen_stg_patterns.py --compare C_TRACE [--expect F] [--c-bin stgTest]
```

只用标准库，只照 `Engineering/Todo/sw/SW7-游戏本体.md` 2.1~2.3 节写，不读 C 的源码与生成物。
C 的轨迹由 app 测试 `stgTest pat` 打出（格式写在 `test/test_stg_emit.c` 的文件头），`stgTest dump SEG FRAME` 打某一帧的全部敌弹。
`--compare` 一致时末行 `compare ok`、退出码 0；不一致时退出码 1，给了 `--c-bin` 就两边各 dump 第一个不同帧、打出第一颗不同的弹。

## check_sw8_stats.sh

```
wsl -e bash -c 'RISCV_BIN=<RISCV_BIN> bash Main/tools/check/check_sw8_stats.sh'
```

判据见 `Engineering/Todo/sw/SW8-演示模式与统计埋点.md` 第 3 节的「A 批」「B 批」几行，口径见 2.2~2.4 节与 4.4、4.9 节。
要主机 gcc、`gcc-multilib`、`libsdl2-dev`、`python3` 与 Pillow（M9 先打包素材）；M6 要交叉工具链，找不到时跳过，跳过不算通过。

| 项 | 内容 |
| --- | --- |
| M1 | `mk/host.mk` 的 `test` 目标：退出码 0、64 位（ASan/UBSan）与 `-m32` 输出逐字节相同、`fail 0`；stdout 与 stderr 一起收，里面不能有 sanitizer 报告；普查 C 组 19 行、模拟时钟 S 组 4 行、帧时间直方图 H 组 8 行、日志队列 Q 组 7 行、`N_max` 状态机 N 组 10 行全 ok |
| M2 | `CENSUS_NO_SETCLIP`、`CENSUS_NO_SURF` 各编一次，指定的那一行必须 FAIL |
| M3 | `FHIST_P99_LOWER`、`FHIST_DROP_OVER`、`COST_NO_B` 各编一次，指定的那一行必须 FAIL |
| M4 | `LOGQ_SPLIT`、`LOGQ_WRAP` 各编一次，指定的那一行必须 FAIL |
| M5 | 主机探针 `check_sw8_tx.c` 按 `mk/host.mk` 编 bsp 的选项编（`WARN` 用 `make --eval` 取、SDL 头按系统头），无窗跑两遍：不设 `GFX2D_TX_BUDGET` 时不限、退出不打汇总；设 192 时 X2、X3 都 ok，退出时 stderr 是 `over 1 times 18 bytes`（X2 因跨场重试时只核有汇总）；`-DINJECT_HOST_TX_NO_REFILL` 必须让 X3 见红 |
| M6 | gfx2dGame 的真实板上构建零告警，`bsp/ti60/board.o` 里有 `board_tx_room`；M6b 再做 `EXTRA_CFLAGS=-DSTG_FIELD=3` 的板上构建，零告警 |
| M7 | `NMAX_BISECT_SIDE`、`NMAX_NO_STEPBACK`、`NMAX_NO_SETTLE` 各编一次，指定的那一行（N1 或 N4）必须 FAIL |
| M8 | gfx2dGame 的 app 测试（`apps/game/gfx2dGame/test/`，选项同 `test` 目标，不链 bsp）64 位挂 ASan/UBSan、`-m32` 各一份，输出逐字节相同、`fail 0`；Y 组 9 行全 ok：定寿命负载的稳态、16×16 与 8×8 各搜一遍 `OK`、结果那一档在新场上跑满确认期零掉帧、加 `STEP_MIN` 那一档有掉帧 |
| M9 | 先打包素材，以 `EXTRA_CFLAGS=-DSTG_FIELD=3` 编 gfx2dGame 的 PC 版，无窗口跑：两行 `nmax cfg`、两行 `nmax result OK`、`fail 0`、`done`；两行 `nmax result` 与 M8 的 app 测试打出来的逐字相同 |

编译产物落临时目录，不动 `sw/build_host`。全部通过时末尾是 `fail 0` 与 `done`，退出码 0。
