# check：提交前检查与规格校验

PC自适应宽屏使用`validate.py --checks pc-wide --profile acceptance --run`，覆盖显式软件画布、
解析矩形覆盖/深度、stride哨兵、错误原子性、分桶独立计数/画序、投影比例与世界状态不变，
并要求固定640宽度变异在新增区域的覆盖判据见红。原生Windows使用`--checks pc-window`，
在实际用户桌面验证全屏/跨屏、自适应与固定入口、纹理重建、失焦释放和模拟断屏恢复。

三维演示使用`r4`（三场景、64/32、sanitizer、图集/采集器反例）与Windows的`r4-board`
（B7三场景CRC、CPU/HW、模式往返、1800帧性能及旧功能恢复）。
详细范围和字段见[三维演示验证](../../sim/engine/r4/README.md)；quick取样不代替acceptance。

检查范围、档位与状态统一见 [验证流程](../../doc/验证流程.md)。
`python3 Main/tools/check/validate.py --list` 是唯一可执行清单；表中的“提交前”指受影响项，
不要求每次运行全部工具。新复跑输出进入忽略的 `Main/sim/build/check-runs/`，历史 logs 不覆盖。

| 公共入口 | 用途 |
| --- | --- |
| `validate.py` | 显示范围、去重执行、输入身份与汇总、核对后归档 |
| `check_run.py` | 同轮输入／参数核对、PASS / FAIL / INCOMPLETE 状态、独立输出目录 |
| `check_common.sh` / `check_host.sh` | 库与应用正常测试同轮复用、缺项与不适用状态 |
| `test_validation.py` | 去重、缺项、证据过期、范围变化与板测完成边界的行为反例 |
| `check_board_smoke.py` | 当前 JTAG、两次调试连接、引擎重建与功能采集；自动定位 UART，不写 Flash |

| 文件 | 用途 | 什么时候跑 |
| --- | --- | --- |
| `check_rast.py` / `rast_model.py` | R1：128B setup、C 增量/直接求值与 Python/Fraction 独立模型；两层 fuzz、舍入穷举、三类变异和双目标构建；交换 JSON 写本轮 logs/rast | `validate.py --checks rast --profile acceptance --run`，需 RISCV_BIN；板上执行归 R3 |
| `gen_recip_tab.py` | 核对 1024 项 Q1.30 倒数种子表，纯整数生成 | 默认核对；改变生成式时用 `--write` 后执行 rast 与受影响库回归 |
| `check_frontend.py` | 绘制前端的 sanitizer、成功封表/错误差分、独立像素、生产变异、完整宿主回放、异步流水与板测日志拒绝检查 | `--models --legacy --pipeline`；`--host flat strip planned template both static prepass combo`；`--summarize --production` 汇总成本和最终流水；单份板测用 `--board-log` 配合 `--modes` / `--scenes`，见 `Main/sim/engine/frontend/README.md` |
| `run_frontend_board.py` | Windows 调用 WSL 编译并用现有 board_run 做 JTAG/DDR 装载，集中登记部署 BIN | 指定 `--tag`、`--flags=...`；`--bit` 可复用候选 bit，默认不写 Flash；首次加载 bit 后可 `--no-program` |
| `validate_frontend_board.py` | 顺序执行绘制前端的独立 CRC/PERF/padding、缓存与阶段计量、同 BIN 两缓存、采样、切换和失败回归 | Windows 的 Efinity Python 运行；可用 `--wait-log` 等已启动的成本矩阵结束；需独占 COM11/JTAG，日志入 frontend/logs |
| `frontend_report_figures.py` | 从正式接口生成绘制前端的三张原生 SVG 架构图 | 报告接口或布局变更后运行；生成后渲染目检，PNG 放忽略 build/ |
| `rebuild_frontend_baseline.py` | 从冻结源码及 `1e2cd8d` 隔离重建绘制前端首轮，与唯一部署清单预期身份比较 | WSL 运行，可指定 `--riscv-prefix <RISCV_BIN>`；结果见 frontend/logs，临时源码结束后清理 |
| `research_strip_frontend.py` | 绘制前端方案研究：既有板测 CSV 的分型回归、1680 份真实状态的分阶段记录计数，观察版与未改生产构造的完整字节/记账对照 | 仓库根在 WSL 运行 `python3 Main/tools/check/research_strip_frontend.py`；摘要与范围见 `Main/sim/engine/frontend/README.md`，不操作板卡 |
| `check_g2_band.py` | 条带缓冲正式数据面：软件执行器金标准与 ASan/UBSan、真实全部核/协议/参数、八个定向变异；独立综合准备/采集、主综合复核、板测身份与功能登记 | 默认由 band/run.sh 跑 17 项；综合用 --prepare-syn/--collect-syn、--collect-main；新板测用 --record-board，未变产物不重复登记 |
| `localize_paths.py` | 展开、还原厂商生成物里的绝对路径；扫描整个仓库里的机器专属路径 | 克隆后 `--apply`；提交前暂存源码，`--restore` 再 `--check --tracked`；全目录检查用 `--check` |
| `regs_gen.py` | 寄存器数字的单一真源，生成 `rtl/engine/engine_regs_offsets.vh` 与供 BSP 驱动使用的 `sw/bsp/include/gfx_regs.h` | 改寄存器偏移或位号后重生成；提交前在 `Main/` 下运行 `python3 tools/check/regs_gen.py --check` |
| `research_engine_features.py` | 可选机制前期研究：配置穷举、编译守卫、隔离真实混合核像素/停顿与厂商map；显式冻结位流的基线板测 | `validate.py --checks feature-research --profile quick` 先显示范围；完整分步用法见 `sim/engine/features/README.md`，不代表裁剪整机验收 |
| `feature_config.py` | 引擎可选机制的配置真源：五结构位、三像素位、交付标志、依赖边、CAPS 位映射与七个代表档；导入时自检合法计数 | 被 `feature_build.py`、`regs_gen.py`、`check_feature_caps.py` 与仿真侧脚本共用；改依赖或位映射只改这里 |
| `feature_build.py` | 引擎裁剪构建入口：按构型生成 `engine_feature.vh`、守卫片段、按构型 CAPS 与工程源列表，并铺一棵自包含源树 | `--sync` 写默认构型到 `rtl/engine/` 与 `sw/bsp/include/`，`--check` 校验一致；非法构型在生成任何产物前拒绝 |
| `check_feature_caps.py` | 特性清单、`regs_gen` 生成物与 `gfx_feature.h` 三处 CAPS 必须同源一致 | `validate.py --checks feature-caps`；防构建入口某处走独立算法导致软件按错的能力位选后端 |
| `check_feature_elab.py` | 七代表档与合法/非法结构组合的配置穷举 elaboration | `validate.py --checks feature-elab`；自建源树到临时目录，跑完删 |
| `check_feature_probe.py` | 关闭的机制离开编译输入：实例化点与源码内容两种判据 | `validate.py --checks feature-probe`；STRIP 不增减例化，只能用源码内容判 |
| `check_feature_contract.py` | 按构型的禁用契约与六条依赖守卫反例 | `validate.py --checks feature-contract`；CAPS 位、CTRL 不支持位、保留寄存器读写、无实例哨兵 |
| `check_feature_function.py` | 160 个合法构型的 FILL、六方程、色键、DDR/TEX、实例、STRIP/PB 绘制与切换、软复位；六个定向变异 | `validate.py --checks feature-function --profile acceptance --run`；日志进入本轮目录，悬空输入告警直接失败 |
| `check_feature_synthesis.py` | 本轮自建七档源树、四步、全时钟时序及网表结构检查 | Windows：`validate.py --checks feature-synthesis --profile acceptance --run`；位流身份登记于本轮 manifest |
| `check_feature_board.py` | 同一 BIN 的七档部署与独立 CRC 功能板测 | Windows：`validate.py --checks feature-board --profile acceptance --run`；自动先执行本轮综合；同轮续跑使用相同范围和 `--run-dir` |
| `check_board_game.py` | 当前位流的生产 HW/DDR 游戏 240 帧，素材版本、启动场景检查、稳态帧率与扫描诊断 | Windows：`validate.py --checks synthesis board board-game --profile acceptance --run`；不重搜 N_max，不写 Flash，不代表完整关卡逐帧回放 |
| `feature_synthesis.py` | 按构型跑整机四步并收集同阶段资源与全时钟时序 | Windows + `EFINITY_HOME`；`--tree-root` 指向构型源树根，逐档写 `synthesis.json`，超容量的档记 FAIL 并继续 |
| `feature_netlist.py` | netlist 结构检查：关闭的机制不得在综合网表里留下信号 | `validate.py --checks feature-netlist`；读 map 网表，pnr 超容量的档同样可判 |
| `g1_research.py` | TEX 装载窗口前期研究：完整 APB 地址集合、热集布局、四 bank RAM 端口实验、软件金标准对真实像素路径的同步读延迟与 valid 变异；`--prepare-syn` 生成隔离 RAM 工程；`--record-board` 核对本次整机板测产物身份 | WSL 中从仓库根运行 `python3 Main/tools/check/g1_research.py`；范围与原始证据见 `Main/sim/engine/tex/README.md` |
| `g2_research.py` | 条带缓冲前期研究：紧凑映射、RAM 掩码/分段、真实 geom/pix 代理、DMA 三相与排空；隔离 RAM 工程与原始报告；板测基线复核 | 从仓库根运行 `python3 Main/tools/check/g2_research.py`；`--prepare-syn`/`--collect-syn` 与 `--check-board` 用法见 `Main/sim/engine/band/README.md` |
| `check_g3_stripctl.py` | 条带控制与命令接口的正式生产库对拍、错误三元组、完整 engine_top 与软件/RTL 变异；综合和板测采集 | 仓库根运行 `python3 Main/tools/check/check_g3_stripctl.py`；16/32 行同表，sanitizer/普通构建各 10000 张；`--collect-syn`/`--collect-board` 只采集；见 `Main/sim/engine/stripctl/README.md` |
| `g3_research.py` | 条带控制前期研究：按游戏几何口径重放条带分桶（16/32 行对照、峰值桶、区容量溢出边界）与直通/条带取数批模型 | 从仓库根运行 `python3 Main/tools/check/g3_research.py`；`--json` 落摘要，判据与证据见 `Main/sim/engine/stripctl/README.md` |
| `check_g1_logs.py` | TEX 装载窗口外部验收：22 个固定负载 CRC、性能/水位、真实 N_max、360 帧游戏同帧、后端切换和 CRC 失败回退 | 归档验收时；`--board-only` 只检查固定负载 |
| `test_game_control.py` | 离线核对三个后端、CSV 弹数、确认后的连续切换和目标缺失时停止 | 控制协议改动后；不打开 GUI/串口 |
| `check_sw_layers.sh` | 软件分层与构建的结构判据，七项，含三处注错 | 改了 `sw/` 的结构、构建文件或 `bsp/include` 时，提交前 |
| `check_round255.c` | 逐像素规格（设计文档 3.5 节）的穷举校验：舍入、中间量位宽、舍入顺序、预乘量化误差 | 改像素规格时；v1 不实现 tint，后续 profile 启用时再加对应穷举 |
| `check_subst.c` | 引擎单通道混合核的六方程**代入表**校验：`x = base×255 + m1×m2` 与 `src×Fs + dst×Fd` 的逐位等价、位宽与符号、16 位模截断注错，另有替代代入（两个乘数都压进 8 位）的等价性 | 改混合方程、代入方式或引擎的位宽时，提交前 |
| `check_cmd_semantics.py` | 命令语义规格（设计文档 3.4 节）的结构校验与 159 条一致性向量（ABI 1），带六项注错自检 | 改命令 ABI 或错误模型时，提交前 |
| `geom_model.py` | 引擎几何层（engine_geom）的**独立模型**：五者求交、逐行迭代、16 字节对齐的读写描述符、读写避让、每行边带；`--selftest` `--directed` 自检，`--vectors` `--cases` 吃两组真实输入并与软件执行器逐像素对照，`--dump-records` 导 $readmemh 记录 | 几何层开工前的研究已用它；几何层的单元仿真与它逐条比对；改裁剪规则、地址公式或读写避让规则时重跑 |
| `geom_cases_dump.c` / `geom_cases_dump.sh` | 把十一个整帧用例的表面表与命令表导成几何模型能吃的记录（**链接 `sw/lib/gfx` 的 `cases.c` 与 `list.c`**，不照抄命令表） | 用例的命令表改了、或几何模型要重跑整帧用例时 |
| `pix_cases_dump.c` | 逐像素通路（engine_pix）金标准导出器：链接 `sw/lib/gfx`，经软件执行器导出初始与期望的 128 位内存、命令表与表面表。四种用例：定向（目的每行跨 4KB、六方程 FILL、相位 4×4、XRGB 源配 ALPHA 类方程、色键与近似字、源越界、多行命令）、`--hazard`（跨命令写后读）、`--sprites`（16 颗 16×16 ALPHA，量周期数）、`--frame 0..10`（十一个整帧用例，附命令表边界元数据） | 真实通路仿真（`sim/engine/pix/run_formal.sh` 现编现导）；整帧仿真；整帧 CRC 11/11 已对 `golden.txt` |
| `replay2c.py` | 录像（`.g2dr`）与 C 数组、录制文本之间的转换；`--check` / `--selftest` 做结构校验与文本往返 | 改录像格式时；`mk/host.mk test` 与 `check_sw4_pc.sh` 会调它 |
| `check_sw4_scale.c` | PC 平台层「缩放用最近邻」的主机侧探针：把 8×8 棋盘放大 80 倍再读回边界像素 | 改缩放或像素格式时 |
| `check_sw4_pc.sh` | PC 平台层的提交前判据：整帧用例对 `golden.txt`、有窗与无窗逐行互比、注错构建、录像文本回灌、缩放探针 | 改 `sw/bsp/` 或 PC 侧构建时，提交前 |
| `check_sw5_assets.sh` | 素材流水线的提交前判据：打包确定性与输入边界、独立实现对账、装载与画面、七种坏包注错、注错构建、镜像回环与 active/passive 边界、许可登记检查 | 改素材、打包工具、`assetpak`、`assetTest` 或镜像工具时，提交前 |
| `check_sw5_pak.py` | 素材包的**独立实现**：自己解包、自己从 PNG 与字库文本重算像素字节再逐字节比 | 由 `check_sw5_assets.sh` 调用，也可单独跑 |
| `check_sw6_demo.sh` | 基础 Demo 的提交前判据，十项：录像可重生成、判据模式编过并跑通、对期望日志、有窗与无窗、两遍一致、HUD 与字形、场景 B、`off` 与 `eq` 计数、四个方程的注错见红、Sanitizer 与板上目标构建 | 改 `gfx2dGame`、`lib/gfx` 的执行器、素材或录像时，提交前 |
| `check_sw6_log.py` | 基础 Demo 判据日志的**外部核对**：两遍比对、`hud` 对上一帧统计、`hcrc` 按字库文本独立重画、`s` 与 `scene` 参考值、`off` 与 `eq`；`--board` 把板上串口日志从 `pak` 行起与期望日志对账 | 由 `check_sw6_demo.sh` 调用；板上对账时单独跑 |
| `gen_sw6_replay.py` | 生成基础 Demo 的两段确定性录像（`.g2dr`）：A 把 N 从 100 升到 5000 再回到 100，B 把 k 从 30 降到 1 再升到 120；形状与停留段写在脚本里 | 录像要改时；`check_sw6_demo.sh` 每次重跑并逐字节比 |
| `check_sw7_game.sh` | 游戏本体的提交前判据：正弦表重生成比对、主机测试 64/32 比对与 sanitizer、数值基础的四个注错见红、游戏代码的 app 测试（64/32、不链 bsp）与实体池的两个注错见红、批量提交的三个注错见红、弹型期望文件的重生成与逐帧比对及三个注错见红、碰撞的两个注错见红、组帧、弹幕场判据模式两遍一致、板上构建与六个逐颗循环的反汇编（含三个注错）、自机与规则（R 组）及三个注错见红、敌机 Boss 与关卡（S 组）及两个注错见红、霓虹素材可重生成（含注错）、游戏的判据入口两遍一致与 HUD 的注错、工具辅助通关录像的重生成与回放（含注错）、S1 规模 | 改 `lib/core/fixmath`、正弦表、`src/game/stg_*`、`lib/gfx/list.c` 的批量提交、`mk/host.mk` 的 `test` 目标时，提交前 |
| `check_sw8_stats.sh` | 演示模式与统计埋点的提交前判据：主机测试 64/32 比对与 sanitizer、普查与软件执行器对账、模拟时钟、帧时间直方图、日志队列、`board_tx_room()` 的 PC 实现与板上构建、`N_max` 状态机的综合负载判据、搜索跑在弹幕场上（app 测试）与 `STG_FIELD=3` 的 PC 驱动、模式框架的切换判据、系统键和弦、模式 3 在终符上搜索、统计 CSV 对账与日志不阻塞长跑、自动驾驶、样例日志对账与出图（含注错）、各注错见红 | 改 `lib/gfx/census`、`lib/core/framehist`、`lib/core/logq`、`lib/core/nmax`、`board_tx_room()`、`src/game/stg_press`、`src/game/stg_auto`、`src/game/stg_chord`、模式框架（`src/main.c`、`src/stg_modes`、`src/gfx2d_stats`）、`mk/host.mk` 的 `test` 目标时，提交前 |
| `check_sw8_tx.c` | `board_tx_room()` 的 PC 实现的主机探针：链真实的 `sw/bsp/host/board.c`，不设 `GFX2D_TX_BUDGET` 时不限，设 192 时按场补满、不累加、超额计数；`-DINJECT_HOST_TX_NO_REFILL` 见红 | 由 `check_sw8_stats.sh` 的 M5 编译运行；改 `bsp/host` 的串口模拟时 |
| `gen_stg_clear.c` | 游戏「可玩」的工具辅助通关录像的**搜索器**：只链 `lib/` 与 `src/game/stg_*.c`，逐帧跑游戏逻辑，每 4 帧试 18 种按法、推演 64 帧择优，走不下去时回退、实在躲不开时允许由炸弹或残机抵掉的中弹；`--out` 搜索并写 `.g2dr`，`--verify` 按录像从头跑一遍打摘要 | 游戏逻辑、关卡或规则改了、入库的录像回放不再通关时重新搜；`check_sw7_game.sh` 的「可玩」一项每次编译（`-Werror`）、复核并重搜比对 |
| `gen_sin_tab.py` | 生成 `sw/lib/core/sin_tab.c`：1024 项加 256 项尾巴的 Q1.14 正弦表，按绝对值四舍五入；`--inject` 改一项给比对当注错 | 表的口径要改时；`check_sw7_game.sh` 每次重跑并逐字节比 |
| `gen_stg_patterns.py` | 游戏弹型判据的**独立实现**：按规格另写正弦表、atan2、`xorshift32`、发射器与池，生成五种弹型加合并段各 120 帧的期望文件 `sw/apps/game/gfx2dGame/test/stg_patterns.txt`；`--compare` 与 C 的轨迹先比参数行再逐帧比，不一致时两边各 dump 第一个不同帧、指出第一颗不同的弹 | 弹型参数或发射规则要改时；`check_sw7_game.sh` 每次重跑并逐字节比 |
| `e7_key_probe.c` | 独立枚举受控棋盘纹理的色键命中数，与软件执行器统计对账 | 改色键负载或解释命中率时 |
| `e8_entity_golden.c` | 用同一份实体算法生成九组主机侧期望值到 `entityBench/src/golden.h`：每组 1000 帧的六字段状态 CRC 与 C 组每帧全部 `32×N` 命令字节 CRC，另保留终态摘要 | 改实体算法、命令格式或帧数时，重生成并随源码同步提交 |
| `e7_injection_check.py` | 从原始整帧 CRC 重判六个方程注错和错配自检；可追加两份 VERIFY_ONLY 板上日志，要求判分、结束标记和运行范围都一致 | 判分修复收尾用；旧日志重判不等于修正版已上板 |
| `e7_output_check.py` | 软件执行器性能负载的**独立输出复算器**：按逐像素混合方程与 `list.c` 的位域，从初始帧缓冲与纹理重画一帧，逐项核对板上整帧 CRC32 和每个可见像素的 32 位加和；要求两面各九项恰好 18 行、无重复或未知项，且有 `fail 0` 和 `done` | 判定板上吞吐数字有效性的硬判据；板上日志格式变了（`<名字> fbN ... crc 0x... pxsum 0x...`）要同步改 `parse_log` |
| `e8_entity_abi.c` | 独立调用 `gfx_blit`，对 64 个图集索引及双记录 w7 标签逐字节检查实体路径命令打包符合 BLIT ABI，另查 8 个 x/y 出屏边界及 AoS/SoA 一致性 | 改实体命令生成、图集映射或 gfx ABI 时 |

## check_r2.py / check_r2_synthesis.py / rast_stream.py

`validate.py` 的 `rlst`、`rast-ports`、`rast-rtl` 分别覆盖列表协议、核心/新端口、
真实 AXI 全链；quick 200 条，acceptance/release 十万条，L1/L2/L2二拍各跑完整流。
流生成器使用 R1 同种子，并用 Main 内固定记录校验前 16 条；同输入同时交叉两份 C
参考器。详见 [`../../sim/engine/rast/README.md`](../../sim/engine/rast/README.md)。

Windows `r2-synthesis` 自建隔离源树：L1/L2 独立四步、20 块 z RAM/1 块索引 RAM、
4/7 DSP 结构和内部时序，再完成 B0/B3 整机四步及全部时钟关系检查。
运行前按开发环境设置 Efinity。输出由公共 Session 登记，不改生产默认构型，
不部署位流或写 Flash。`rast` 仍专指 R1 软件验证。

旧 TEX、BAND、STRIP 与实例套件用 `feature_build.write_headers` 在本轮 build/include
生成 B3，PB 套件生成 B5，并将其放在默认头目录之前。命令行等价形式是
`feature_build.py --config B3 --out-dir <忽略目录>/include --headers-only`。
特性、guard 和 CAPS 来自同一真源；这避免在默认 B0 下误编译一个要求 TEX/条带的套件。

## localize_paths.py

```
wsl -e python3 Main/tools/check/localize_paths.py --apply      占位符 -> 本机路径，克隆之后跑一次
wsl -e python3 Main/tools/check/localize_paths.py --restore    本机路径 -> 占位符，提交之前跑一次
wsl -e python3 Main/tools/check/localize_paths.py --check      扫描整个仓库，发现机器专属路径就以非零退出码结束
```

有六个厂商生成的文件里必须写绝对路径：`fpga/gfx2d_ti60/ip/soc/settings.json`、`ip/soc/source/soc_config`、
gpioDemo 的 `.cproject` 与三个 `.launch`，仓库里存的是占位符形式。占位符代表 `Main/` 的路径。
在 WSL 里运行时，写进去的仍是盘符路径，因为读这些文件的是 Windows 上的厂商工具。

`--check` 找盘符路径、`/home/<账户名>/`、`/mnt/<盘符>/` 三类写法，扫的是整个 git 仓库。
不扫：不入库的厂商资料包 `Efinix/`，厂商代码与 IP 生成物（它们带生成时那台机器的工具安装位置），编译产物。
豁免登记在脚本的 `ALLOW` 里，现有三项：`doc/开发环境.md`（第 11 节集中记本机取值）、本脚本、
一处原样引用厂商报错输出的文档（输出里带 Efinity 的安装路径）。

## check_sw_layers.sh

```
wsl -e bash -c 'RISCV_BIN=<RISCV_BIN> bash Main/tools/check/check_sw_layers.sh'
```

`RISCV_BIN` 是交叉工具链前缀（`doc/开发环境.md` 第 1 节占位符），不给时取 `riscv-none-elf-`，要在 PATH 里。
第 2、3、4、6 项跑真实的板上构建，第 1、7 项用主机 gcc。判据与被测构建必须用同一组参数，
否则判据通过了而分层并没有生效。

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
**开头先跑一次 `assets/pack_assets.py`**：`gfx2dGame` 包含生成物 `assets.h`，
干净克隆里不先打包，第 2、3、4、6 项那几次真实构建会因为找不到 `assets.h` 而失败（那是假红）。

## check_round255.c

```
wsl -e bash -c 'gcc -O2 -o /tmp/check_round255 Main/tools/check/check_round255.c && /tmp/check_round255'
```

纯整数，不依赖板子。把设计文档 3.5 节方程表的六个方程逐个代入，穷举比较「两项各自舍入再相加」与
「相加后一次舍入」两种写法，另校预乘与非预乘两条路径相对实数定义的量化误差。全部符合时打印「总判定：全部通过」，退出码 0。

可选模式 `--golden <文件>` 只导出抽样金标准、不跑上面的断言：1024 条向量，前 96 条是每个方程的边界组合，
其余每个字段各取一次线性同余的最高 8 位；每行一个 43 位十六进制字 `{eq,src,dst,a,c,out}`，
`out` 用本文件的快速式算。`sim/engine/blend/run.sh` 用它给混合核做抽样对账。
不带参数时的输出与加这个模式之前逐字节相同。

## check_subst.c

```
wsl -e bash -c 'gcc -O2 -o /tmp/check_subst Main/tools/check/check_subst.c && /tmp/check_subst'
```

纯整数，不依赖板子。按代入表口径校验六个混合方程：
把六个方程各自写成 `x = base × 255 + m1 × m2`，对**每个方程的全定义域**
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

只用标准库，不依赖板子、编译器与 SDL。把设计文档 3.4 节的字段矩阵、检查优先级、
错误码、方程/格式匹配规则抄成一份可执行镜像，据此生成九组共 159 条一致性向量
（ABI 1），逐条比对错误码、零基索引、`w7` 标签、取记录条数、clip 状态与内存内容。
规范正文是权威；应当先改正文，再同步这份镜像。导出格式用 `[list]` 保留多次提交边界。

期望值是**手写并注明出处**的，检查器是另一份实现；两者不一致即 FAIL。
教训是：工具不能自己判自己，两个实现也不能共享同一个错误。`(A)`~`(D)` 是结构审计
（优先级十步各有反例、字段矩阵的零集与反例集相等、18 个方程×格式组合逐个有向量、八类描述符谓词
各有失败例，其中控制字三个保留段分列，共 10 个失败位点），`(F)` 列出前期研究补入正文并经复核定案的条目，`(G)` 是六项注错自检——把每条
规则改错，向量集必须见红；`(H)` 独立读回导出的 vector/list/cmd 层，核对多命令表边界与每表
`COUNT`。全部通过时末尾是 `fail 0` 与 `done`，退出码 0。

## geom_model.py

```
wsl -e python3 Main/tools/check/geom_model.py --selftest                      模型自己的 23 项单元检查
wsl -e python3 Main/tools/check/geom_model.py --directed                      34 条几何定向用例
wsl -e python3 Main/tools/check/check_cmd_semantics.py --emit /tmp/v.txt --no-selftest
wsl -e python3 Main/tools/check/geom_model.py --vectors /tmp/v.txt            159 条一致性向量
wsl -e bash -c 'bash Main/tools/check/geom_cases_dump.sh /tmp/c.txt'
wsl -e python3 Main/tools/check/geom_model.py --cases /tmp/c.txt              十一个整帧用例
wsl -e python3 Main/tools/check/geom_model.py --dump-records /tmp/v.txt build/v.hex   RTL 测试台的期望值与命令激励
wsl -e python3 Main/tools/check/geom_model.py --dump-directed build/directed.hex       定向用例的同一种记录
```

只用标准库，不依赖 RTL、板子与编译器。它是**几何层的第二份实现**：从命令算出五者求交的矩形、
逐行的读源/读目的/写描述符（16 字节对齐的覆盖区间、不跨 4KB、不超过 192 拍）、
读写避让（读源、读目的的有无），以及每行的边带
（像素数、写拍数、源与目的的**行首地址拍内相位** `lead = (row & 15) >> 2`）。
`--directed` 的 34 条覆盖真实输入没有的边角：基址不按 4KB 对齐、基址只按 4 字节对齐、
行跨 4KB（FILL 一条，BLIT 的源与目的都拆、只有源拆各一条）、源基址只按 8 字节对齐、源与目的相位 4×4、
宽度对 4 取余 4 种、源矩形越出源表面、六个方程的读目的开关、零面积。每组输入跑完都断言描述符规矩与读写避让违例为 0、每行的掩码有效像素数等于 `npx`。

**命令表的走法与参考实现同口径**（`run_vector()`，对齐 `check_cmd_semantics.py` 的 `run_lists()`）：
SET_CLIP 的左上角取 `w1`；裁剪框跨命令表继承，不随表复位；向量出错时截在出错那一条之前，之后的记录与表不到几何层。
`--vectors`/`--cases` 另做两项外部对照，防止模型只和自己一致：每条 FILL/BLIT 按软件执行器的写法
（四者求交的窗口里逐像素判源越界，裁剪框在对照里独立重放）逐行比，并打印执行器口径的 `pixel_total` 与 `src_oob`；
向量带参考实现的末态裁剪框（`clip=`）时，模型走完之后的框必须与它相同。

**记录格式**（`REC_CMD`/`REC_SB`/`REC_RD`/`REC_WR` 四张表写在脚本里，是模型与 RTL 测试台之间的唯一真源）：
每行一条记录，第一列是类型字 `0`（命令头，33 个字：命令接口的全部字段 + 期望的矩形、行数、flags）/
`1`（写描述符）/`2`（读描述符，含目标行缓冲号）/`3`（边带）/`F`（结束），其余全是 32 位十六进制；
`//` 开头是注释（每个向量、每张命令表开头各一行），`$readmemh` 直接读。
**记录的先后就是 RTL 的发射顺序**（每行：边带 → 读源 → 读目的 → 写目的），
测试台据此逐条比。只导出几何层真正见到的命令；`cidx` 是表内下标。
`--dump-directed` 与它同格式，只是输入换成 34 条定向用例。

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

只用标准库，不依赖板子、编译器与 SDL。格式的权威是 `sw/lib/core/replay.h`：
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

PC 平台层「要跑多次再比」的几条：
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

素材流水线的结构判据，九项：

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
需在板上单独执行。

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

基础 Demo 的十项结构判据。开头先跑
`assets/pack_assets.py`（`assets.h` 与 `assets.pak` 是生成物、不入库）。默认构建是**演示模式**，
所以每一项都用 `EXTRA_CFLAGS=-DSW6_JUDGE=1` 构建判据模式；`SW6_QUICK=1` 时另加
`-DSW6_FRAMES=120` 并把期望日志换成 `logs/sw6_judge_quick.log`。

| 项 | 内容 |
| --- | --- |
| 录像可重生成 | `gen_sw6_replay.py` 重打两份 `.g2dr` 与入库的逐字节相同；`replay2c.py --check` 独立解析 |
| 编过并跑通 | `make PLATFORM=host` 编过；判据模式（有窗）跑一遍，`^fail 0`、`^done`、退出码 0。另编演示模式加帧数上限并冒烟，A、B 两段都出现。双缓冲不变量的反例 `INJECT=SW6_FB_PER_PASS`（按每遍帧号选面、每遍 121 帧）必须打出 `fb err` |
| 对期望日志 | 去掉 `cyc` 三列后与入库期望日志逐行相同；快速档的逐帧行按段按遍是全量档的前缀（`eq A` 除外，它是这一遍实际跑过的帧的累计） |
| 有窗与无窗 | 同一个二进制在 `SDL_VIDEODRIVER=dummy` 下再跑一遍，逐字节相同 |
| 两遍一致 | 每段 `p 1` 与 `p 2` 逐帧相同；反例 `INJECT=SW6_LIVE_HUD` 必须见红 |
| HUD 与字形 | `hud` 列等于同一遍上一帧的 `cmd`／`pix`（每遍首帧 `- -`）；`hcrc` 等于按字库文本与本行字段独立重画的 CRC；反例 `INJECT=EQ_COPY` 见红 |
| 场景 B | 三个 `scene` 参考值两两不同；每个 B 帧的 `scrc` 等于本帧 `s` 那张图；k=1 段内 `s` 逐帧轮换且三张都轮到；反例 `EQ_COPY` 见红 |
| off 与 eq 计数 | A 帧 `off ≥ 4`；`eq A` 的 copy／alpha／add、`eq P` 的 copy／alpha／straight 都非 0 |
| 逐方程注错 | `EQ_COPY`／`EQ_ALPHA`／`EQ_ADD`／`EQ_ALPHA_STRAIGHT` 各构建运行一次，每次与快速档期望日志至少一行不同 |
| Sanitizer 与板上目标 | `SAN=1` 跑一遍，ASan／UBSan 无报告、`fail 0`；另编判据与演示两种板上构建，零告警且 bin ≤ 524288 字节。交叉工具链装在 WSL 里、不在 PATH 上，要传 `RISCV_BIN=<RISCV_BIN>`（`Main/doc/开发环境.md` 第 11 节）；不传时找不到就跳过并提示，**跳过不算通过**，验收前必须带上它跑 |

全量一次约 2 分 40 秒，`SW6_QUICK=1` 约 2 分 10 秒（2026-09-24 实测，带 `RISCV_BIN`，含双缓冲反例与两种板上构建；
编过并跑通与有窗无窗两项在全量档各跑 1226 帧，约 22 秒一遍；两遍一致、逐方程注错、Sanitizer 三项两档都用快速档的帧数）。
**本脚本不上板**：板上对账用 `tools/board/board_run.py --bin`
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
本程序横幅。所以它去掉 `#` 行与空行，取**最后一个**程序横幅（`gfx2dGame` 开头的那行）之后、从 `pak` 行起的部分，
与期望日志从 `pak` 行起逐行比（去掉 `cyc` 三列）。`--counts` 在一行 `eq` 都没有时报错，不空转。

日志格式的权威是 `gfx2dGame` 打日志的代码，这个脚本是它的**外部实现**：按字段位置解析 `f` 行
（行里不出现字符本身，按空白切分即可），`hcrc` 那条按 `Main/assets/font/ascii8x16.txt`
与本行的 `n`／`k`／`hud` 字段独立重画整块 HUD 矩形再算 CRC——C 侧与 Python 侧各算一遍，
两边都对才说明 HUD 的路径（字形、位置、数字、`--`、纯黑底）没写歪。
字库解析照 `pack_assets.py` 的规则：`#` 后跟空白才是注释，`########` 是合法像素行。
只用标准库（`zlib.crc32` 与 `lib/core/crc32` 同参）。

## gen_sw6_replay.py

```
wsl -e python3 Main/tools/check/gen_sw6_replay.py            写进应用的 replay/
wsl -e python3 Main/tools/check/gen_sw6_replay.py --out <目录>
wsl -e python3 Main/tools/check/gen_sw6_replay.py --to-text  打成录制文本（给人看）
```

基础 Demo 两段确定性录像的生成器，形状是定死的：
A 停 10 帧、上键 49 次升到 N=5000、停 30 帧、下键 49 次回到 100、停 10 帧，共 246 帧；
B 停 31 帧、下键 29 次降到 k=1、停 30 帧、上键 119 次升到 120、停 10 帧，共 367 帧。
每步「按 1 帧、松 1 帧」，应用按按下沿判，两帧一步。

它是**独立于 `replay2c.py` 的第二份写法**，只按格式写字节；`check_sw6_demo.sh` 的「录像可重生成」一项
既用它重生成再逐字节比，也用 `replay2c.py --check` 独立解析一遍。
入库的是 `.g2dr`（源数据），构建时由两个 `mk` 调 `replay2c.py` 转成 C 数组落到 `$(OBJDIR)/replay/`。

## check_sw7_game.sh

```
wsl -e bash -c 'RISCV_BIN=<RISCV_BIN> bash Main/tools/check/check_sw7_game.sh'
```

各项判据与口径写在下面表里。不要板子；
要主机 gcc、`gcc-multilib`、`libsdl2-dev`（`mk/host.mk` 解析时就要 `sdl2-config`）、`python3` 与 Pillow（弹幕场判据先打包素材）；
板上构建项缺交叉工具链时返回 INCOMPLETE。正式档包含约 5 分钟的录像重搜；quick 档将它记为不适用，仍验证既有录像与生产回放。显式 SW7_SKIP_SEARCH=1 返回 INCOMPLETE。

| 项 | 内容 |
| --- | --- |
| 正弦表 | `gen_sin_tab.py` 重打一份，与入库的 `lib/core/sin_tab.c` 逐字节相同；注错 `--inject` 改一项之后比对必须见红 |
| 主机测试 | `mk/host.mk` 的 `test` 目标：退出码 0、64 位（ASan/UBSan）与 `-m32` 输出逐字节相同、`fail 0`；stdout 与 stderr 一起收，里面不能有 sanitizer 报告（`test` 目标本身不看 stderr）；数值基础 F 组 21 行、批量提交 K 组 8 行全 ok |
| 数值基础的注错 | `FX_IDX_TRUNC`、`FX_VEL_Q4`、`FX_ATAN_NOSCALE`、`FX_ATAN_UNIT` 各编一次，指定的那一行必须 FAIL |
| 实体池 app 测试 | 游戏代码的 app 测试：`apps/game/gfx2dGame/test/*.c`、`src/game/stg_*.c`、`lib/core`、`lib/gfx` 按 `test` 目标的选项直接编（选项用 `make --eval` 从 `mk/host.mk` 取），64 位挂 ASan/UBSan、`-m32` 各一份，两份输出逐字节相同、`fail 0`、无 sanitizer 报告；不链 bsp |
| 实体池的注错 | `STG_POOL_STALL`、`STG_POOL_SWAP` 各编一次，指定的那一行必须 FAIL |
| 批量提交的注错 | `BLIT_RUN_W7`、`BLIT_RUN_SXY`、`BLIT_RUN_NO_FALLBACK` 各编一次，指定的那一行必须 FAIL；最后一个要让「源等于目的」那一行见红，证明随机输入真的走到了逐条回退 |
| 弹型 | `gen_stg_patterns.py` 重打期望文件，与入库的 `test/stg_patterns.txt` 逐字节相同；实体池 app 测试的两份产物各跑 `pat` 打轨迹，64/32 逐字节相同，再用 `--compare` 与期望文件先比参数行、再逐帧比、末行比溢出计数 |
| 弹型的注错 | `EMIT_SEQ_NOACC`、`EMIT_AIM_SWAP`、`EMIT_RAIN_ORDER` 各编一次，比对必须在指定的段（spiral、aim、rain）第一个报出不一致，并指出第一颗不同的弹 |
| 碰撞 | 实体池 app 测试的输出里 X 组 13 行全 ok（边界、勾股数相切、大坐标差、擦弹一次、受击关掉、已标记跳过、随机对 64 位朴素参考、场里的自机狙段） |
| 碰撞的注错 | `COLLIDE_LE` 让 X1 的相切那一行见红；`COLLIDE_NO_CULL` 让 X3 见红，且 stderr 里有 UBSan 的平方溢出报告 |
| 组帧（主机） | 实体池 app 测试的输出里 W 组 12 行全 ok（剔除边界、分段扫 = 一次扫 = 独立重数、源坐标（米弹、小圆弹的颜色号、火花的寿命帧）、六段各 300 帧组帧时拒绝为 0 且普查的 ALPHA 条数 = 发出的敌弹数） |
| 弹幕场判据模式 | 先打包素材，再以 `EXTRA_CFLAGS=-DSTG_FIELD=2` 编 gfx2dGame 的 PC 版，无窗口（`SDL_VIDEODRIVER=dummy`）跑两遍：去掉 `\r` 之后两遍逐字节相同、`fail 0`、`done`、六段 840 帧、没有 `err` 行 |
| 板上构建与反汇编 | `STG_FIELD=1`、`2`、`4` 的板上构建零告警；`objdump -d` 查七个逐颗的循环 `stg_pool_step`、`stg_pool_collide`、`stg_pool_draw`、`stg_pool_hit_targets`、`stg_pool_cancel`、`stg_pool_age`、`stg_pool_danger`（自动驾驶的危险度扫描）：没有 `div`/`divu`/`rem`/`remu`、没有 `__div*`/`__mod*` 调用、没有 `ret` 以外的 `jalr`/`jr`；注错 `STG_LOOP_DIV`、`PHIT_LOOP_DIV`、`AGE_LOOP_DIV`、`DANGER_LOOP_DIV` 各在 `stg_pool_step`、`stg_pool_hit_targets`、`stg_pool_age`、`stg_pool_danger` 里加一次除法，这条检查必须在那个函数上见红。要 `RISCV_BIN`，找不到时跳过，跳过不算通过 |
| 自机与规则 | 实体池 app 测试的输出里 R 组 28 行全 ok（移动、射击节奏、受击与无敌、自动炸弹与火花、手动炸弹的按下沿、粒子溢出、回标题与 HI-SCORE、擦弹得分、自机弹对敌机的边界与随机对 64 位朴素参考、粒子寿命与弹种位图） |
| 规则的注错 | `GAME_NO_AUTOBOMB` 让 R4 见红，`GAME_NO_INVINCIBLE` 让 R3 的无敌那一行见红，`PHIT_LE` 让 R9 的相切那一行见红 |
| 敌机、Boss 与关卡 | 实体池 app 测试的输出里 S 组 21 行全 ok（表、移动事件表与出界、槽位、击毁、整局各段开始帧对关卡表常量另算、射击时走到 CLEAR 与得分另算、Boss 路径与间隙不受伤、压力形态、旋钮加倍时稳态弹数不减、改档之后到达稳态不超过 `SETTLE_MIN`、游戏里的门控、背景） |
| 关卡的注错 | `CARD_TIMEOUT_GT` 让 S5 的各段开始帧见红，`GATE_NO_ACC` 让 S11 见红（`-m32` 构建，整局跑得快） |
| 霓虹素材 | `Main/tools/assets/gen_stg_neon.py` 连跑两次，12 张 PNG 逐字节相同；`--check` 重新生成与入库的 `Main/assets/neon/` 逐像素相同（比像素不比 PNG 字节：压缩流随 Pillow 与 zlib 的版本变）；注错 `--check --inject` 改一个像素必须见红 |
| 字库 | `Main/tools/assets/gen_font_txt.py --check` 由入库的 `Main/assets/font/spleen-8x16.bdf` 重新生成字库文本，与入库的 `ascii8x16.txt` 逐字节相同；注错 `--check --inject` 改一个像素必须见红 |
| 游戏判据入口 | 实体池 app 测试的输出里 V 组 5 行全 ok（共用文字绘制与逐字 `gfx_blit` 逐字节相同、3000 帧组帧的普查对账）；`-DSTG_FIELD=4` 的 PC 版无窗口、`GFX2D_UNPACED=1` 跑两遍，日志逐字节相同、`fail 0`、没有 `err` 行、九行 `seg` 齐全（READY、STAGE、BOSS、四张符卡、CLEAR、回到 READY） |
| HUD 的注错 | `INJECT=STG_LIVE_HUD` 以 `-DSTG_FRAMES=1200` 跑一遍，`g` 行与游戏判据入口正常那一遍的前 1200 帧比：第 0 帧相同，第一个完整的一秒之后必须不同（实时量进了画面，整帧 CRC 就随墙钟变）。原来是注错版自己跑两遍互比，机器安静时两遍可能恰好一样而假红，2026-09-28 改 |
| 可玩 | 工具辅助的通关录像 `apps/game/gfx2dGame/replay/stg_clear.g2dr`：a 搜索器 `gen_stg_clear.c` 以 `-Werror` 编过，按录像从头跑一遍走到 `CLEAR`、残机没用完、帧恰好用完；b 搜索器重新搜一遍，产物与入库的逐字节相同（约 5 分钟，`SW7_SKIP_SEARCH=1` 跳过，跳过不算通过）；c 以 `EXTRA_CFLAGS="-DSTG_FIELD=4 -DSTG_JUDGE_REPLAY=1"` 编 PC 版，无窗口、`GFX2D_UNPACED=1` 回放两遍：整份日志逐字节相同、`fail 0`，`seg`、`clear`、`end` 三种行与搜索器的摘要逐行相同 |
| 回放的注错 | `INJECT=REPLAY_KEY`（第 200~203 帧的按键换成别的方向）：逐帧日志必须恰好从第 200 帧起不同，摘要也必须与搜索器不同 |
| S1 规模 | 默认构建以 `GFX2D_START_MODE=3`、`GFX2D_PRESS_KNOB=1744`、`GFX2D_FRAMES=1200` 编，无窗口按 60 Hz 跑 20 秒（`GFX2D_SNAPSHOT=900` 截帧）：第 10 秒以后每秒的抽样帧敌弹不少于 1900、溢出 0、`fail 0`、截出 640×480 的图；方程像素的均值作登记 |

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

只用标准库，独立实现正弦表、atan2 与发射逻辑，不读 C 的源码与生成物。
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

各项判据与口径写在下面表里。
要主机 gcc、`gcc-multilib`、`libsdl2-dev`、`python3` 与 Pillow（M9 先打包素材）；M6 要交叉工具链，找不到时跳过，跳过不算通过。
正式档保留 M12 生产模式双进程完整搜索（原记录约 7 分钟）和 M14 十分钟长跑；
quick 档明确排除这两项，仍保留状态机、弹幕场搜索、模式与 CSV 判据。
显式 SW8_SKIP_LONG=1 使 M14 未执行，整体返回 INCOMPLETE。

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
| M15 | 自动驾驶的 app 测试：M8 的输出里 A 组 8 行全 ok（接管与交还、偏好与平局、躲弹、打一局两遍相同；危险度扫描对朴素参考的比对在实体池测试里）；注错 `AUTO_ORDER`（候选顺序倒过来）必须让 A2 的平局那一行见红 |
| M16 | 自动驾驶打一局：以 `EXTRA_CFLAGS="-DSTG_FIELD=4 -DSTG_JUDGE_AUTO=1"` 编 PC 版（输入换成自动驾驶、受击打开），无窗口、`GFX2D_UNPACED=1` 跑两遍：`seg` 与 `g` 行逐字节相同、`fail 0`、`done`、没有 `err` 行，打出活到哪一段与每帧平均开销（PC 上的周期数只作登记）|
| M16i | `INJECT=AUTO_ORDER` 编同一个入口：`g` 行的 CRC 序列必须与 M16 不同 |
| M17 | 出图与第一版统计：入库的四份样例日志 `apps/game/gfx2dGame/logs/sw8_stats_{demo,game,s1,press}.log` 按 M13 的解析逐行对账、`fail 0`；`Main/tools/stats/plot_stats.py` 按固定参数（写在 `tools/stats/README.md`）重出三张图，三份图源与 `Deliverables/Assert/figures/src/sw8_*.html` 逐字节相同 |
| M17i | 只给一份没有搜索的日志：`plot_stats.py` 必须报 `figures 2 missing 1`、退出码非 0、不出搜索轨迹图 |

编译产物落临时目录，不动 `sw/build_host`。全部通过时末尾是 `fail 0` 与 `done`，退出码 0。

## 游戏整机复现

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


## 通用实例路径

check_f4_instances.py 在 WSL 运行，公共 --run-dir 与 --profile 选择本轮证据与档位；提供 --models、--rtl、--mutations、--protocols、
--lease、--v2 与 --legacy。独立像素金标准、完整引擎、错误排空与过期 lease
判据及日志见 `Main/sim/engine/frontend/README.md`。
run_f4_board.py 在 Windows 的 Efinity Python 下重建与串行采集；部署 BIN 与 bit
集中登记在本轮 frontend/f4_artifacts.json。板测不写 Flash，结束条件使用完整 done 行。
--legacy 已包含 --v2；重复选择时仅执行一次。

实例路径的 --research 提供研究工具与输入回环复现，--timed-profile 提供 TIMED 阶段诊断，
--collect 只汇总本轮正式宿主／RTL组，输出 f4_results.json；板测与综合单独登记。
--review-archive --archive-dir <仓内历史日志目录> 复核旧完整档案，保留原目录，结果进入新轮 archive-review。

## check_f5_bin.py / run_f5_synthesis.py / run_f5_board.py

硬件保序分桶与 PB v1 的独立判据和串行板测。按开发环境手册选择 WSL、
Efinity Python 与 EFINITY_HOME，命令从仓库根运行；以下为正式全量示例，
各命令必须使用同一 --run-dir 和 acceptance 档，普通修改按验证流程选子集：

~~~text
python3 Main/tools/check/check_f5_bin.py --run-dir Main/sim/build/check-runs/<运行号> --profile acceptance --research --models --bin --frame --mutations --protocols --game --replay --lease --legacy --legacy-all --ddr-log
EFINITY_HOME="<EFINITY_WSL>" python3 Main/tools/check/check_f5_bin.py --run-dir Main/sim/build/check-runs/<运行号> --profile acceptance --platform
python3 Main/tools/check/check_f5_bin.py --run-dir Main/sim/build/check-runs/<运行号> --profile acceptance --collect
python Main/tools/check/run_f5_synthesis.py --run-dir Main/sim/build/check-runs/<运行号> --profile acceptance --build bin main --collect
python Main/tools/check/run_f5_board.py --run-dir Main/sim/build/check-runs/<运行号> --profile acceptance --build tests legacy v2 research verify ab combo pressure nmax replay
python Main/tools/check/run_f5_board.py --run-dir Main/sim/build/check-runs/<运行号> --profile acceptance --run tests legacy v2 research verify ab combo pressure nmax replay --program
python Main/tools/check/run_f5_board.py --run-dir Main/sim/build/check-runs/<运行号> --profile acceptance --collect
~~~

--research 严格复现17个冻结输入每桶计数及修复后的万张原型；--models 生成独立
标量像素与真实 PB 字节金标准；--bin 为真实 RTL 一万张逐字对拍；--frame、
--protocols 覆盖完整引擎及20个故障/模式/容量反例、8/32行；--game 为 K1984
完整640×480帧，含定时DDR、随机反压、16/32行及AXI/分类计数对账；--replay
为宿主9208帧逐行对照；--lease、--legacy/--legacy-all 检查双帧资源与旧通路。
六个真实 RTL 变异须失败；正常控制须成功。中间产物写 frontend/build/f5。

f5_game_dump.c 是真实生产输入与独立标量金标准导出器，由 --game 编译调用，
不链接到板上应用。独立综合采用动态观察 wrapper，只量资源和时序，协议验证
由真实 RTL 测试台承担。综合收录检查当前位流新鲜度、所有时钟非负及自产位宽告警。

板测只有一个进程占COM/JTAG，不写Flash。首项 --program 部署当前正时序位流；
后续复用该位流。Nmax 同一BIN按 TEX16/8、PB16/8 搜索，原搜索参数保持，各
3600场确认；压力构建 REF_CAP=32768，其余8192。--collect 独立检查结束边界、
74组CRC、153组冻结状态与27组背景组合、四个长时确认和完整9208帧逐行结果。
部署身份只登记到 logs/f5_artifacts.json，最终消费者写 f5_board_results.json。

这里的 logs 指该轮 logs/frontend；--platform 包含 --platform-hw。
跨 WSL/Windows 续用同轮时保持相同的 profile、RISCV_BIN 等已登记参数；
quick 档分桶 RTL 为 64 张，完整 --collect 拒绝快速或缺项结果。

## run_f5_jtag_capture.py

正常UART不可用时的功能诊断备用入口，仅支持tests/v2/replay；性能仍用原
run_f5_board.py的正常构建与串口证据。使用开发环境所列Efinity Python：

~~~text
python Main/tools/check/run_f5_jtag_capture.py --build tests v2 replay --run tests v2 replay --program
~~~

每项独立build_f5_*_ddr编入F5_DDR_LOG=1；BSP将board_puts原始字节写到
OFFSCREEN的+0x400000..+0x600000，预留32B header，容量2MiB；避开背景A/B和
+6MiB影子。默认构建完全不启用该路径，不访问COM，也不写Flash。
header含magic、加载器nonce、长度、DONE/OVERFLOW、CRC32、版本、容量与保留字。
发布前后使用fence；done必须是完整独立一行，overflow直接失败。
JTAG加载核对字节数，周期性短暂停CPU读header，完成后dump原始DDR日志，
检查nonce/header稳定、长度和独立zlib CRC32，再消费fail/done与逐帧结果。
该诊断暂停与日志开销使周期不适合性能比较，f5_jtag_*.json明确标记。

纯sink的ASan/UBSan判据为sim/engine/frontend/test_ddr_log.c，覆盖标准CRC32金值、
入口为 check_f5_bin.py --ddr-log 或 validate.py --checks ddr-log；完整汇总不再依赖手工生成该日志。
分块done、伪done和溢出保护。部署身份仍进入唯一f5_artifacts.json；JTAG功能
日志带单独产物键，普通UART性能日志不替换。
