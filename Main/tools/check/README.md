# check：提交前检查与规格校验

| 文件 | 用途 | 什么时候跑 |
| --- | --- | --- |
| `rebuild_frontend_baseline.py` | 从冻结源码及 `1e2cd8d` 隔离重建 F1 首轮，与唯一部署清单预期身份比较 | WSL 运行，可指定 `--riscv-prefix <RISCV_BIN>`；结果见 frontend/logs，临时源码结束后清理 |
| `research_strip_frontend.py` | 绘制前端方案研究：既有 G4 CSV 的分型回归、1680 份真实状态的分阶段记录计数，观察版与未改生产构造的完整字节/记账对照 | 仓库根在 WSL 运行 `python3 Main/tools/check/research_strip_frontend.py`；摘要与范围见 [frontend/README](../../sim/engine/frontend/README.md)，不操作板卡 |
| `check_g2_band.py` | G2 正式数据面：软件执行器金标准与 ASan/UBSan、真实全部核/协议/参数、八个定向变异；独立综合准备/采集、主综合复核、板测身份与功能登记 | 默认由 band/run.sh 跑 17 项；综合用 --prepare-syn/--collect-syn、--collect-main；新板测用 --record-board，未变产物不重复登记 |
| `localize_paths.py` | 展开、还原厂商生成物里的绝对路径；扫描整个仓库里的机器专属路径 | 克隆后 `--apply`；提交前暂存源码，`--restore` 再 `--check --tracked`；全目录检查用 `--check` |
| `regs_gen.py` | D51 的寄存器数字单一真源，生成 `rtl/engine/engine_regs_offsets.vh` 与供 BSP 驱动使用的 `sw/bsp/include/gfx_regs.h` | 改寄存器偏移或位号后重生成；提交前在 `Main/` 下运行 `python3 tools/check/regs_gen.py --check` |
| `g1_research.py` | G1 前期研究：完整 APB 地址集合、热集布局、四 bank RAM 端口实验、软件金标准对真实像素路径的同步读延迟与 valid 变异；`--prepare-syn` 生成隔离 RAM 工程；`--record-board` 核对本次 B6 板测产物身份 | WSL 中从仓库根运行 `python3 Main/tools/check/g1_research.py`；范围与原始证据见 [tex/README](../../sim/engine/tex/README.md) |
| `g2_research.py` | G2 前期研究：紧凑映射、RAM 掩码/分段、真实 geom/pix 代理、DMA 三相与排空；隔离 RAM 工程与原始报告；板测基线复核 | 从仓库根运行 `python3 Main/tools/check/g2_research.py`；`--prepare-syn`/`--collect-syn` 与 `--check-board` 用法见 [band/README](../../sim/engine/band/README.md) |
| `check_g3_stripctl.py` | G3 正式生产库对拍、错误三元组、完整 engine_top 与软件/RTL 变异；综合和板测采集 | 仓库根运行 `python3 Main/tools/check/check_g3_stripctl.py`；16/32 行同表，sanitizer/普通构建各 10000 张；`--collect-syn`/`--collect-board` 只采集；见 [stripctl/README](../../sim/engine/stripctl/README.md) |
| `g3_research.py` | G3 前期研究：按游戏几何口径重放条带分桶（16/32 行对照、峰值桶、区容量溢出边界）与直通/条带取数批模型 | 从仓库根运行 `python3 Main/tools/check/g3_research.py`；`--json` 落摘要，判据与证据见 [stripctl/README](../../sim/engine/stripctl/README.md) |
| `check_g1_logs.py` | G1 外部验收：22 个固定负载 CRC、性能/水位、真实 N_max、360 帧游戏同帧、后端切换和 CRC 失败回退 | G1 归档时；`--board-only` 只检查固定负载 |
| `test_game_control.py` | 离线核对三个后端、CSV 弹数、确认后的连续切换和目标缺失时停止 | 控制协议改动后；不打开 GUI/串口 |
| `check_sw_layers.sh` | 软件分层与构建的结构判据，七项，含三处注错 | 改了 `sw/` 的结构、构建文件或 `bsp/include` 时，提交前 |
| `check_round255.c` | 逐像素规格（设计文档 3.5 节）的穷举校验：舍入、中间量位宽、舍入顺序、预乘量化误差 | 改像素规格时；v1 不实现 tint，后续 profile 启用时再加对应穷举 |
| `check_subst.c` | 引擎 P1 B1 的六方程**代入表**校验：`x = base×255 + m1×m2` 与 `src×Fs + dst×Fd` 的逐位等价、位宽与符号、16 位模截断注错，另有替代代入（两个乘数都压进 8 位）的等价性 | 改混合方程、代入方式或引擎的位宽时，提交前 |
| `check_cmd_semantics.py` | 命令语义规格（设计文档 3.4 节、spec 第 02 章）的结构校验与 159 条一致性向量（ABI 1），带六项注错自检 | 改命令 ABI 或错误模型时，提交前 |
| `geom_model.py` | 引擎 P1 B3（几何层）的**独立模型**：五者求交、逐行迭代、16 字节对齐的读写描述符、D7 读写避让、每行边带；`--selftest` `--directed` 自检，`--vectors` `--cases` 吃两组真实输入并与软件执行器逐像素对照，`--dump-records` 导 $readmemh 记录 | B3 开工前的研究已用它；B3 的单元仿真与它逐条比对；改裁剪规则、地址公式或 D7 时重跑 |
| `geom_cases_dump.c` / `geom_cases_dump.sh` | 把十一个整帧用例的表面表与命令表导成几何模型能吃的记录（**链接 `sw/lib/gfx` 的 `cases.c` 与 `list.c`**，不照抄命令表） | 用例的命令表改了、或几何模型要重跑整帧用例时 |
| `pix_cases_dump.c` | B4 金标准导出器：链接 `sw/lib/gfx`，经软件执行器导出初始与期望的 128 位内存、命令表与表面表。四种用例：定向（目的每行跨 4KB、六方程 FILL、相位 4×4、XRGB 源配 ALPHA 类方程、色键与近似字、源越界、多行命令）、`--hazard`（跨命令写后读）、`--sprites`（16 颗 16×16 ALPHA，量周期数）、`--frame 0..10`（十一个整帧用例，附命令表边界元数据） | B4 的真实通路仿真（`sim/engine/pix/run_formal.sh` 现编现导）；B6 的整帧仿真；整帧 CRC 11/11 已对 `golden.txt` |
| `replay2c.py` | 录像（`.g2dr`）与 C 数组、录制文本之间的转换；`--check` / `--selftest` 做结构校验与文本往返 | 改录像格式时；`mk/host.mk test` 与 `check_sw4_pc.sh` 会调它 |
| `check_sw4_scale.c` | PC 平台层「缩放用最近邻」的主机侧探针：把 8×8 棋盘放大 80 倍再读回边界像素 | 改缩放或像素格式时 |
| `check_sw4_pc.sh` | PC 平台层（SW4）的提交前判据：整帧用例对 `golden.txt`、有窗与无窗逐行互比、注错构建、录像文本回灌、缩放探针 | 改 `sw/bsp/` 或 PC 侧构建时，提交前 |
| `check_sw5_assets.sh` | 素材流水线（SW5）的提交前判据：打包确定性与输入边界、独立实现对账、装载与画面、七种坏包注错、注错构建、镜像回环与 active/passive 边界、许可台账 | 改素材、打包工具、`assetpak`、`assetTest` 或镜像工具时，提交前 |
| `check_sw5_pak.py` | 素材包的**独立实现**：自己解包、自己从 PNG 与字库文本重算像素字节再逐字节比 | 由 `check_sw5_assets.sh` 调用，也可单独跑 |
| `check_sw6_demo.sh` | 基础 Demo（SW6）的提交前判据，十项：录像可重生成、判据模式编过并跑通、对期望日志、有窗与无窗、两遍一致、HUD 与字形、场景 B、`off` 与 `eq` 计数、四个方程的注错见红、Sanitizer 与板上目标构建 | 改 `gfx2dGame`、`lib/gfx` 的执行器、SW5 素材或录像时，提交前 |
| `check_sw6_log.py` | SW6 判据日志的**外部核对**：两遍比对、`hud` 对上一帧统计、`hcrc` 按字库文本独立重画、`s` 与 `scene` 参考值、`off` 与 `eq`；`--board` 把板上串口日志从 `pak` 行起与期望日志对账 | 由 `check_sw6_demo.sh` 调用；板上对账时单独跑 |
| `gen_sw6_replay.py` | 生成 SW6 的两段确定性录像（`.g2dr`）：A 把 N 从 100 升到 5000 再回到 100，B 把 k 从 30 降到 1 再升到 120；形状与停留段写在脚本里 | 录像要改时；`check_sw6_demo.sh` 每次重跑并逐字节比 |
| `check_sw7_game.sh` | 游戏本体（SW7）A~D 四批的提交前判据：正弦表重生成比对、主机测试 64/32 比对与 sanitizer、数值基础的四个注错见红、游戏代码的 app 测试（64/32、不链 bsp）与实体池的两个注错见红、批量提交的三个注错见红、弹型期望文件的重生成与逐帧比对及三个注错见红、碰撞的两个注错见红、组帧、弹幕场判据模式两遍一致、板上构建与六个逐颗循环的反汇编（含三个注错）、自机与规则（R 组）及三个注错见红、敌机 Boss 与关卡（S 组）及两个注错见红、霓虹素材可重生成（含注错）、游戏的判据入口两遍一致与 HUD 的注错、工具辅助通关录像的重生成与回放（含注错）、S1 规模 | 改 `lib/core/fixmath`、正弦表、`src/game/stg_*`、`lib/gfx/list.c` 的批量提交、`mk/host.mk` 的 `test` 目标时，提交前 |
| `check_sw8_stats.sh` | 演示模式与统计埋点（SW8）A~D 四批的提交前判据：主机测试 64/32 比对与 sanitizer、普查与软件执行器对账、模拟时钟、帧时间直方图、日志队列、`board_tx_room()` 的 PC 实现与板上构建、`N_max` 状态机的综合负载判据、搜索跑在弹幕场上（app 测试）与 `STG_FIELD=3` 的 PC 驱动、模式框架的切换判据、系统键和弦、模式 3 在终符上搜索、统计 CSV 对账与日志不阻塞长跑、自动驾驶、样例日志对账与出图（含注错）、各注错见红 | 改 `lib/gfx/census`、`lib/core/framehist`、`lib/core/logq`、`lib/core/nmax`、`board_tx_room()`、`src/game/stg_press`、`src/game/stg_auto`、`src/game/stg_chord`、模式框架（`src/main.c`、`src/stg_modes`、`src/gfx2d_stats`）、`mk/host.mk` 的 `test` 目标时，提交前 |
| `check_sw8_tx.c` | `board_tx_room()` 的 PC 实现的主机探针：链真实的 `sw/bsp/host/board.c`，不设 `GFX2D_TX_BUDGET` 时不限，设 192 时按场补满、不累加、超额计数；`-DINJECT_HOST_TX_NO_REFILL` 见红 | 由 `check_sw8_stats.sh` 的 M5 编译运行；改 `bsp/host` 的串口模拟时 |
| `gen_stg_clear.c` | SW7「可玩」的工具辅助通关录像的**搜索器**：只链 `lib/` 与 `src/game/stg_*.c`，逐帧跑游戏逻辑，每 4 帧试 18 种按法、推演 64 帧择优，走不下去时回退、实在躲不开时允许由炸弹或残机抵掉的中弹；`--out` 搜索并写 `.g2dr`，`--verify` 按录像从头跑一遍打摘要 | 游戏逻辑、关卡或规则改了、入库的录像回放不再通关时重新搜；`check_sw7_game.sh` 的 G16 每次编译（`-Werror`）、复核并重搜比对 |
| `gen_sin_tab.py` | 生成 `sw/lib/core/sin_tab.c`：1024 项加 256 项尾巴的 Q1.14 正弦表，按绝对值四舍五入；`--inject` 改一项给比对当注错 | 表的口径要改时；`check_sw7_game.sh` 每次重跑并逐字节比 |
| `gen_stg_patterns.py` | SW7 弹型判据的**独立实现**：照 spec 另写正弦表、atan2、`xorshift32`、发射器与池，生成五种弹型加合并段各 120 帧的期望文件 `sw/apps/game/gfx2dGame/test/stg_patterns.txt`；`--compare` 与 C 的轨迹先比参数行再逐帧比，不一致时两边各 dump 第一个不同帧、指出第一颗不同的弹 | 弹型参数或 2.1、2.3 节的规则要改时；`check_sw7_game.sh` 每次重跑并逐字节比 |
| `e7_key_probe.c` | 独立枚举受控棋盘纹理的色键命中数，与软件执行器统计对账 | E7 改色键负载或解释命中率时 |
| `e8_entity_golden.c` | 用同一份实体算法生成九组主机侧期望值到 `entityBench/src/golden.h`：每组 1000 帧的六字段状态 CRC 与 C 组每帧全部 `32×N` 命令字节 CRC，另保留终态摘要 | 改 E8 实体算法、命令格式或帧数时，重生成并随源码同步提交 |
| `e7_injection_check.py` | 从原始整帧 CRC 重判六个方程注错和错配自检；可追加两份 VERIFY_ONLY 板上日志，要求判分、结束标记和运行范围都一致 | E7 判分修复收尾；旧日志重判不等于修正版已上板 |
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

可选模式 `--golden <文件>` 只导出抽样金标准、不跑上面的断言：1024 条向量，前 96 条是每个方程的边界组合，
其余每个字段各取一次线性同余的最高 8 位；每行一个 43 位十六进制字 `{eq,src,dst,a,c,out}`，
`out` 用本文件的快速式算。`sim/engine/blend/run.sh` 用它给混合核做抽样对账。
不带参数时的输出与加这个模式之前逐字节相同。

## check_subst.c

```
wsl -e bash -c 'gcc -O2 -o /tmp/check_subst Main/tools/check/check_subst.c && /tmp/check_subst'
```

纯整数，不依赖板子。对应 `Engineering/TechReports/engine/01-计算核心.md` 3.1 节的代入表与第五节
「代入表」一行判据：把六个方程各自写成 `x = base × 255 + m1 × m2`，对**每个方程的全定义域**
穷举，与 `src×Fs + dst×Fd` 逐位比较，并给出实测的位宽与符号。另有三项：

- **16 位模截断注错**：按方程给期望值。只有 `x` 上限 130050 的 ADD 与 ALPHA 预乘会出错
  （32385 / 65536 与 4113532 / 16777216），其余四个方程上限 65025、`t = x + 128 ≤ 65153 < 65536`，
  截断是空操作，期望值是 0。
- **替代代入**：按 `src` 与 `dst` 谁大选 `base`/`m1`/`m2`，让两个乘数都落进 8 位无符号
  （为「能不能用 dual 模式一块放两个乘法」提供依据）。断言它同样恒等、且两个乘数上限 ≤ 255。
- **输入范围**：ALPHA 预乘跑满 2^24，**不限定** `src_pm ≤ a`。`src_pm ≤ a` 是素材侧约束，
  硬件会收到任意输入（BLIT 从表面读的源像素没人逐个检查），软件执行器对任意输入都按同一条规则算。
  2026-09-29 订正前只跑了受限范围，那一版因此漏掉了 ALPHA 的 16 位截断。

六个方程合计 50462976 组，全部符合时打印「总判定：全部通过」，退出码 0。

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

## geom_model.py

```
wsl -e python3 Main/tools/check/geom_model.py --selftest                      模型自己的 23 项单元检查
wsl -e python3 Main/tools/check/geom_model.py --directed                      34 条几何定向用例
wsl -e python3 Main/tools/check/check_cmd_semantics.py --emit /tmp/v.txt --no-selftest
wsl -e python3 Main/tools/check/geom_model.py --vectors /tmp/v.txt            159 条一致性向量
wsl -e bash -c 'bash Main/tools/check/geom_cases_dump.sh /tmp/c.txt'
wsl -e python3 Main/tools/check/geom_model.py --cases /tmp/c.txt              十一个整帧用例
wsl -e python3 Main/tools/check/geom_model.py --dump-records /tmp/v.txt build/v.hex   RTL 测试台的期望值与 I1 激励
wsl -e python3 Main/tools/check/geom_model.py --dump-directed build/directed.hex       定向用例的同一种记录
```

只用标准库，不依赖 RTL、板子与编译器。它是**几何层的第二份实现**：按 spec 第 01 章 3.6 节
与第 02 章规范条款 §2、§4 的规则，从命令算出五者求交的矩形、逐行的读源/读目的/写描述符
（16 字节对齐的覆盖区间、不跨 4KB、不超过 192 拍）、D7 的读写避让，以及每行的边带
（像素数、写拍数、源与目的的**行首地址拍内相位** `lead = (row & 15) >> 2`）。
`--directed` 的 34 条覆盖真实输入没有的边角：基址不按 4KB 对齐、基址只按 4 字节对齐、
行跨 4KB（FILL 一条，BLIT 的源与目的都拆、只有源拆各一条）、源基址只按 8 字节对齐、源与目的相位 4×4、
宽度对 4 取余 4 种、源矩形越出源表面、六个方程的读目的开关、零面积。每组输入跑完都断言规矩 1 与 D7 违例为 0、每行的掩码有效像素数等于 `npx`。

**命令表的走法与参考实现同口径**（`run_vector()`，对齐 `check_cmd_semantics.py` 的 `run_lists()`）：
SET_CLIP 的左上角取 `w1`；裁剪框跨命令表继承，不随表复位；向量出错时截在出错那一条之前，之后的记录与表不到几何层。
`--vectors`/`--cases` 另做两项外部对照，防止模型只和自己一致：每条 FILL/BLIT 按软件执行器的写法
（四者求交的窗口里逐像素判源越界，裁剪框在对照里独立重放）逐行比，并打印执行器口径的 `pixel_total` 与 `src_oob`；
向量带参考实现的末态裁剪框（`clip=`）时，模型走完之后的框必须与它相同。

**记录格式**（`REC_CMD`/`REC_SB`/`REC_RD`/`REC_WR` 四张表写在脚本里，是模型与 RTL 测试台之间的唯一真源）：
每行一条记录，第一列是类型字 `0`（命令头，33 个字：I1 的全部字段 + 期望的矩形、行数、flags）/
`1`（写描述符）/`2`（读描述符，含目标行缓冲号）/`3`（边带）/`F`（结束），其余全是 32 位十六进制；
`//` 开头是注释（每个向量、每张命令表开头各一行），`$readmemh` 直接读。
**记录的先后就是 RTL 的发射顺序**（每行：边带 → 读源 → 读目的 → 写目的），
测试台据此逐条比。只导出几何层真正见到的命令；`cidx` 是表内下标。
`--dump-directed` 与它同格式，只是输入换成 34 条定向用例。

研究的过程、算式与全部数据见 `Engineering/TechReports/engine/03-几何层.md`；
入库实现、单元仿真与逐拍测试台在 `Main/rtl/engine/engine_geom.v` 与 `Main/sim/engine/geom/`。

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

素材流水线的结构判据，九项，见 [sw 第 04 章](../../../Engineering/TechReports/sw/04-素材流水线.md) 3.12 节：

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
需在板上单独执行，见 sw 第 04 章 3.13 节。

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

基础 Demo（[sw 第 05 章](../../../Engineering/TechReports/sw/05-基础Demo.md)）的十项结构判据。开头先跑
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
**本脚本不上板**：板上对账按 sw 第 05 章 3.13 节用 `tools/board/board_run.py --bin`
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
字库解析照 `pack_assets.py` 的规则：`#` 后跟空白才是注释，`########` 是合法像素行（sw 第 04 章 4.13 节踩过）。
只用标准库（`zlib.crc32` 与 `lib/core/crc32` 同参，sw 第 04 章 3.6 节）。

## gen_sw6_replay.py

```
wsl -e python3 Main/tools/check/gen_sw6_replay.py            写进应用的 replay/
wsl -e python3 Main/tools/check/gen_sw6_replay.py --out <目录>
wsl -e python3 Main/tools/check/gen_sw6_replay.py --to-text  打成录制文本（给人看）
```

SW6 两段确定性录像的生成器，形状是 spec 定死的（sw 第 05 章 3.10 节）：
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

判据与口径见 `Engineering/TechReports/sw/06-游戏本体.md` 第三节与第五节。不要板子；
要主机 gcc、`gcc-multilib`、`libsdl2-dev`（`mk/host.mk` 解析时就要 `sdl2-config`）、`python3` 与 Pillow（G10 先打包素材）；
G11 要交叉工具链，找不到时跳过，跳过不算通过。全套在本机约十几分钟，其中 G16 的重新搜索约 5 分钟（`SW7_SKIP_SEARCH=1` 跳过，跳过不算通过）。

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
| G11 | `STG_FIELD=1`、`2`、`4` 的板上构建零告警；`objdump -d` 查七个逐颗的循环 `stg_pool_step`、`stg_pool_collide`、`stg_pool_draw`、`stg_pool_hit_targets`、`stg_pool_cancel`、`stg_pool_age`、`stg_pool_danger`（SW8 自动驾驶的危险度扫描）：没有 `div`/`divu`/`rem`/`remu`、没有 `__div*`/`__mod*` 调用、没有 `ret` 以外的 `jalr`/`jr`；注错 `STG_LOOP_DIV`、`PHIT_LOOP_DIV`、`AGE_LOOP_DIV`、`DANGER_LOOP_DIV` 各在 `stg_pool_step`、`stg_pool_hit_targets`、`stg_pool_age`、`stg_pool_danger` 里加一次除法，这条检查必须在那个函数上见红。要 `RISCV_BIN`，找不到时跳过，跳过不算通过 |
| G12 | 自机与规则：G4 的输出里 R 组 28 行全 ok（移动、射击节奏、受击与无敌、自动炸弹与火花、手动炸弹的按下沿、粒子溢出、回标题与 HI-SCORE、擦弹得分、自机弹对敌机的边界与随机对 64 位朴素参考、粒子寿命与弹种位图） |
| G12i | `GAME_NO_AUTOBOMB` 让 R4 见红，`GAME_NO_INVINCIBLE` 让 R3 的无敌那一行见红，`PHIT_LE` 让 R9 的相切那一行见红 |
| G13 | 敌机、Boss 与关卡：G4 的输出里 S 组 21 行全 ok（表、移动事件表与出界、槽位、击毁、整局各段开始帧对 sw 第 06 章 3.11 节的常量另算、射击时走到 CLEAR 与得分另算、Boss 路径与间隙不受伤、压力形态、旋钮加倍时稳态弹数不减、改档之后到达稳态不超过 `SETTLE_MIN`、游戏里的门控、背景） |
| G13i | `CARD_TIMEOUT_GT` 让 S5 的各段开始帧见红，`GATE_NO_ACC` 让 S11 见红（`-m32` 构建，整局跑得快） |
| G14 | 霓虹素材：`Main/tools/assets/gen_stg_neon.py` 连跑两次，12 张 PNG 逐字节相同；`--check` 重新生成与入库的 `Main/assets/neon/` 逐像素相同（比像素不比 PNG 字节：压缩流随 Pillow 与 zlib 的版本变）；注错 `--check --inject` 改一个像素必须见红 |
| G14f | 字库：`Main/tools/assets/gen_font_txt.py --check` 由入库的 `Main/assets/font/spleen-8x16.bdf` 重新生成字库文本，与入库的 `ascii8x16.txt` 逐字节相同；注错 `--check --inject` 改一个像素必须见红 |
| G15 | 游戏的组帧与判据入口：G4 的输出里 V 组 5 行全 ok（共用文字绘制与逐字 `gfx_blit` 逐字节相同、3000 帧组帧的普查对账）；`-DSTG_FIELD=4` 的 PC 版无窗口、`GFX2D_UNPACED=1` 跑两遍，日志逐字节相同、`fail 0`、没有 `err` 行、九行 `seg` 齐全（READY、STAGE、BOSS、四张符卡、CLEAR、回到 READY） |
| G15i | `INJECT=STG_LIVE_HUD` 以 `-DSTG_FRAMES=1200` 跑一遍，`g` 行与 G15 正常那一遍的前 1200 帧比：第 0 帧相同，第一个完整的一秒之后必须不同（实时量进了画面，整帧 CRC 就随墙钟变）。原来是注错版自己跑两遍互比，机器安静时两遍可能恰好一样而假红，2026-09-28 改 |
| G16 | 工具辅助的通关录像 `apps/game/gfx2dGame/replay/stg_clear.g2dr`：a 搜索器 `gen_stg_clear.c` 以 `-Werror` 编过，按录像从头跑一遍走到 `CLEAR`、残机没用完、帧恰好用完；b 搜索器重新搜一遍，产物与入库的逐字节相同（约 5 分钟，`SW7_SKIP_SEARCH=1` 跳过，跳过不算通过）；c 以 `EXTRA_CFLAGS="-DSTG_FIELD=4 -DSTG_JUDGE_REPLAY=1"` 编 PC 版，无窗口、`GFX2D_UNPACED=1` 回放两遍：整份日志逐字节相同、`fail 0`，`seg`、`clear`、`end` 三种行与搜索器的摘要逐行相同 |
| G16i | `INJECT=REPLAY_KEY`（第 200~203 帧的按键换成别的方向）：逐帧日志必须恰好从第 200 帧起不同，摘要也必须与搜索器不同 |
| G17 | S1 规模：默认构建以 `GFX2D_START_MODE=3`、`GFX2D_PRESS_KNOB=1744`、`GFX2D_FRAMES=1200` 编，无窗口按 60 Hz 跑 20 秒（`GFX2D_SNAPSHOT=900` 截帧）：第 10 秒以后每秒的抽样帧敌弹不少于 1900、溢出 0、`fail 0`、截出 640×480 的图；方程像素的均值作登记 |

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

只用标准库，只照 `Engineering/TechReports/sw/06-游戏本体.md` 3.4、3.5、3.7 节写，不读 C 的源码与生成物。
C 的轨迹由 app 测试 `stgTest pat` 打出（格式写在 `test/test_stg_emit.c` 的文件头），`stgTest dump SEG FRAME` 打某一帧的全部敌弹。
`--compare` 一致时末行 `compare ok`、退出码 0；不一致时退出码 1，给了 `--c-bin` 就两边各 dump 第一个不同帧、打出第一颗不同的弹。

## gen_stg_clear.c

```
A=Main/sw/apps/game/gfx2dGame
gcc -O2 -std=gnu11 -Wall -Wextra -Werror -IMain/sw/lib -I$A/src -o gen_stg_clear \
    Main/tools/check/gen_stg_clear.c $A/src/game/stg_*.c Main/sw/lib/core/*.c Main/sw/lib/gfx/*.c -lm
./gen_stg_clear --out $A/replay/stg_clear.g2dr      重新搜索并写入库的那份（约 5 分钟）
./gen_stg_clear --verify $A/replay/stg_clear.g2dr   按录像从头跑一遍，打摘要
```

只链 `lib/` 与游戏代码，不链 bsp、不组帧，一帧游戏逻辑在 PC 上约 1 微秒，所以每一步都在真实的模拟上试。
搜索规则写在文件头：每 4 帧一块、18 个候选（九个方向各分高低速）、块末推演 64 帧择优；走不下去时退 d 块并禁用那一块的按键，
同一个前沿上 d 按 1、1、2、2……加倍；同一个前沿失败满 14 次就允许这一块中弹（由炸弹或残机抵掉，不能导致残机用完）。
全是整数与固定顺序，同一份游戏代码两次搜出同一串按键，所以入库的录像可以由 G16 重新搜出来逐字节比。

`stg_game` 里四个池的 `ent` 指向同一个结构体里的存储，存档与推演都经 `snap_copy`（拷完把指针改回副本自己的存储）；
直接 `memcpy` 的话推演改的是当前状态的池。`--out` 搜完会按录像从头重跑一遍打摘要，拷贝有漏的话摘要就对不上。

摘要（`seg`、`clear`、`end` 三种行）与 gfx2dGame 回放入口（`-DSTG_FIELD=4 -DSTG_JUDGE_REPLAY=1`）同格式；
末尾 `fail 0` 的条件是走到 `CLEAR`、回到标题、残机没用完、录像的帧恰好用完。

## check_sw8_stats.sh

```
wsl -e bash -c 'RISCV_BIN=<RISCV_BIN> bash Main/tools/check/check_sw8_stats.sh'
```

判据与口径见 `Engineering/TechReports/sw/07-演示模式与统计埋点.md` 第三节与第五节。
要主机 gcc、`gcc-multilib`、`libsdl2-dev`、`python3` 与 Pillow（M9 先打包素材）；M6 要交叉工具链，找不到时跳过，跳过不算通过。
M12 在本机约 7 分钟、M14 约 10 分钟，全套约 20 多分钟；`SW8_SKIP_LONG=1` 跳过 M14（跳过不算通过）。

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
| M10 | 模式切换：默认构建以 `GFX2D_START_MODE=1、2、3`、`GFX2D_FRAMES=1` 冷启动各跑一帧取进入行的整帧 CRC；`-DGFX2D_SWITCH_JUDGE=1` 的合成按键来回切 150 次（无窗口、`GFX2D_UNPACED=1`）：151 行进入行的 CRC 与同模式的冷启动相同，模式 1、2 进来时四个池都是 0，裁剪框复位，`switch done 150`、`fail 0` |
| M10i | `INJECT=MODE_KEEP_POOL`（重置游戏状态时漏清敌弹池）：进入行必须有与冷启动不符的 |
| M11 | 系统键和弦：M8 的 app 测试输出里 H 组 5 行全 ok；注错 `CHORD_RETRIGGER` 必须让 H1 的「不再触发」见红 |
| M12 | 模式 3 在终符上搜索：默认构建以 `GFX2D_START_MODE=3`、`GFX2D_PRESS_EXIT=1` 无窗口跑两遍搜索（模拟时钟）：两行 `nmax cfg`（`SETTLE_MIN=300`）、两行 `nmax result OK`、`nmax done`、`fail 0`；同一产物同时跑两个进程，两份日志的 `nmax` 行逐字节相同（`csv` 行是真实周期数，本来就不同）；本机约 7 分钟 |
| M13 | 统计 CSV 对账：默认构建以 `GFX2D_START_MODE=1、2、3` 各跑 600 帧（无窗口、`GFX2D_UNPACED=1`），脚本自己解析 `csv,` 行：表头逐字相同、每行 30 列、`t` 从 0 连续、`cmd_eq` 之和加 `cmd_other` 等于 `cmd`、`pix_eq` 之和等于 `pixel`、没有被别的日志夹断的行；`csvsum` 的 `rows` 等于行数、`lines_dropped` 0；三个模式都出过行，应用 `fail 0` |
| M13i | `INJECT=CSV_DROP_EQ`（打印时漏掉方程 1 那两列）：脚本的解析必须见红（应用按内部值自查，看不见这一处） |
| M14 | 日志不阻塞长跑：默认构建以 `GFX2D_START_MODE=2`、`GFX2D_FRAMES=36000` 编，`GFX2D_TX_BUDGET=192`、无窗口、按 60 Hz 节拍跑约 10 分钟（600 帧之后自动驾驶接管）：`lines_dropped` 0、`fail 0`、stderr 的超额汇总 `over 0 times 0 bytes`。`SW8_SKIP_LONG=1` 跳过，跳过不算通过 |
| M15 | 自动驾驶的 app 测试：M8 的输出里 A 组 8 行全 ok（接管与交还、偏好与平局、躲弹、打一局两遍相同；危险度扫描对朴素参考在 SW7 的 P7）；注错 `AUTO_ORDER`（候选顺序倒过来）必须让 A2 的平局那一行见红 |
| M16 | 自动驾驶打一局：以 `EXTRA_CFLAGS="-DSTG_FIELD=4 -DSTG_JUDGE_AUTO=1"` 编 PC 版（输入换成自动驾驶、受击打开），无窗口、`GFX2D_UNPACED=1` 跑两遍：`seg` 与 `g` 行逐字节相同、`fail 0`、`done`、没有 `err` 行，打出活到哪一段与每帧平均开销（PC 上的周期数只作登记）|
| M16i | `INJECT=AUTO_ORDER` 编同一个入口：`g` 行的 CRC 序列必须与 M16 不同 |
| M17 | 出图与第一版统计：入库的四份样例日志 `apps/game/gfx2dGame/logs/sw8_stats_{demo,game,s1,press}.log` 按 M13 的解析逐行对账、`fail 0`；`Main/tools/stats/plot_stats.py` 按固定参数（写在 `tools/stats/README.md`）重出三张图，三份图源与 `Deliverables/Assert/figures/src/sw8_*.html` 逐字节相同 |
| M17i | 只给一份没有搜索的日志：`plot_stats.py` 必须报 `figures 2 missing 1`、退出码非 0、不出搜索轨迹图 |

编译产物落临时目录，不动 `sw/build_host`。全部通过时末尾是 `fail 0` 与 `done`，退出码 0。

## G4 整机复现

check_g4_integration.py 从生产游戏和素材构建证据：--host 运行完整
9208 帧 v1/v2 对拍与 1 万张批量构造差分；--rtl 运行真实游戏整帧和三变异；
--performance 复核三档 N_max 与 S1 扫描，--samples / --replay 复核已有板测，
--replay-progress 只检查正在采集的完整前缀，不据此通过最终回放验收。

~~~text
RISCV_BIN=<RISCV_BIN> python3 Main/tools/check/check_g4_integration.py --build-target
python3 Main/tools/check/check_g4_integration.py --host --rtl
python3 Main/tools/check/check_g4_integration.py --performance
~~~

--build-target 在 WSL 构建同帧、回放、两种后端 N_max、S1、交互与注错程序，
并构建缓存正常/失效注错/反例自检。--target-suffix 可追加独立 OBJDIR 后缀，
复现时避免覆盖已登记的板测对象。--register 接收实际部署文件，身份集中
写入 sim/engine/g4/logs/artifacts.json，按大小和修改时间缓存摘要。
g4_game_dump.c 是生产游戏的 RTL 工件导出器，由 --rtl 编译调用。
