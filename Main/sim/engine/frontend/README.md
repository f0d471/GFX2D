# 绘制前端：研究、实现与复现

本目录收录 F1～F4 的独立判据与原始证据。F1～F3 已验收归档，F4 正式实现
与板测完成、待验收；候选按显式开关选择，正确性、净收益、采用分别登记。
接口规格见
[绘制前端总览](../../../../Engineering/Todo/sw/绘制前端总览.md)。

正式代码在 `Main/sw/lib/gfx/frame.h/.c`、`strip_plan.c`、
`apps/game/gfx2dGame/src/frontend_game.c` 与 `apps/probe/frontendBench/`。
F1～F3 沿原 v1/v2 与 RTL 边界；F4 新增 v1-I 与实例 RTL，SDK、时钟和 Flash
保持。16KB 对照只复用 G4 候选 bit，F4 本批以 4KB 主线验证。

## 判据与命令

从仓库根在 WSL 运行：

```text
python3 Main/tools/check/check_frontend.py --models --legacy --pipeline
python3 Main/tools/check/check_frontend.py --host flat strip planned template both static prepass combo
```

需要 gcc、Python 3、SDL2、multilib 和现有素材生成物。模型使用 ASan/UBSan。
planned 差分明确要求前 10000 张成功 seal，另 1000 张测试错误；模板另跑
所有 H/相位和六方程的实际命中，避免随机场景只测 miss。像素消费者为独立
直通执行器，包含非游戏矩形、裁剪、源越界、后置 overlay 和 padding。

旧 v1/v2 的 G3 一万场景、错误三元组与 G4 批量差分由 `--legacy` 复跑，日志
写本目录，不覆盖 G3/G4 的归档证据。手工链接 `strip.c` 的工具同时链接
`strip_plan.c`。正式应用仍由 mk 的 wildcard 收集库源码。

Windows 板测使用 Efinity Python 和既有 `board_run.py`，COM/JTAG 同时只由
一个采集进程占用。按 `Main/doc/开发环境.md` 的占位符设置解释器：

```powershell
$env:PYTHONHOME = '<EFINITY>\python311'
$env:PYTHONIOENCODING = 'utf-8'
& '<EFINITY>\python311\bin\python.exe' Main/tools/check/run_frontend_board.py --tag <批次名> --flags='-DFRONTEND_SCENE_MASK=0x411 -DFRONTEND_MODE_LAST=5'
```

首次装载 bit 后，后续同位流程序可 `--no-program`；指定既有候选用 `--bit`。
每次实际部署的 BIN 在唯一 `logs/artifacts.json` 登记，旧 bit 复用 G4 的
唯一清单。工具只执行 JTAG/DDR 装载，不写 Flash。

完整队列运行 `validate_frontend_board.py`。`--resume` 只复用具有完整结束
边界且原部署 BIN 大小/时间戳仍与登记一致的记录，仍重新检查矩阵与消费者。
改动被测实现时使用新 tag 或去掉 resume；源码新旧不靠日志名推定。
队列在开始和 16KB 对照后显式装载 4KB 主线，16KB 阶段异常也恢复主线。
Python 的中文边界读取显式使用 UTF-8，不依赖 Windows 默认代码页。

## 固定输入与测量对象

`frontendBench` 两型分别固定 K=0/384/1024/1648/1984/2400，初始化后推进
1200 步；另取通关录像 0/5000/9000/9027/9207 帧。每个状态跨方案复用
相同输入，游戏逻辑不在构造计时内。

模式编号：0 为直通/TEX，1 为原即时 v2，2 为 planned，3 为几何模板，4 为
planned+模板；5 为 B1 底色缓存，6 为 B2 动态 prepass，7/8/9 分别是 B2
与 planned/模板/两者组合。逻辑 draw、by_eq、可见敌弹、Plan、refs 与
物理记录分别输出。

计量构建先预热八帧，每配置三次各 1000 帧。`GFX_FRONTEND_PROFILE` 为粗
阶段，`GFX_FRONTEND_DETAIL` 为单独的细分诊断；正常生产构建不包含回调。
`FRONTEND_ISOLATE` 与 `G4_STRIP_COMPARE` 配合切彩条，关闭帧缓冲 DDR
扫描；`FRONTEND_EXECUTE` 开启扫描与实际引擎执行，另报 wall、各 pass
耗时和 PERF_BUSY。

`FRONTEND_VERIFY` 单独构建，一组只测一次，完成后用软件执行器重定向到
离屏影子，检查可见像素、padding、CMD/PIXEL/KEY_SKIP。其周期受诊断影响，
只用于正确性。`fpass` 保留每 pass 原九项 PERF 与耗时，DDR 字节按 16 B/beat
解释；B2 的 prepass 和 BAND 流量均计入。

## 证据文件

| 文件 | 对象与边界 |
| --- | --- |
| `baseline/` | 首轮测量源码；隔离链接 `1e2cd8d`，重建与部署的 124430 B BIN 全字节一致 |
| `inputs/manifest.json` | 34 份固定 v1/v2 有效记录输入，表面描述符、记录数、计数及记录；与 RV32 全字段 CRC 一致 |
| `logs/f1_baseline_4k.log/.json` | 首轮 17 状态×两模式×三次×1000 帧；构造周期，不包含渲染 |
| `logs/candidates_final_4k.log/.json` | 一份登记 BIN 的三状态×十候选×三次×1000 帧，含扫描与执行；F3 初版几何探针的成本快照，最终模板模式成本看 f3_cpu |
| `logs/all_outputs_4k.log/.json` | 17 状态×十配置×三次，共 510 组独立 CRC/PERF/padding；诊断周期不作为性能 |
| `logs/f1_memory_v2_4k.log` | 相同数据/地址集合的复制、读、读改写与访问次序/冷热点矩阵 |
| `logs/f1_sets_isolated_4k.log` | 明确关闭扫描后的同组/不同组、冷/热、读/纯写/读改写，36 次各 1000 次访问 |
| `logs/f1_stages_4k.*`、`f1_detail_4k.*`、`f1_store_4k.*` | 原生产调用的粗阶段与细分探针，校准、干扰与 residual 分开解释 |
| `logs/f3_cpu_4k.*`、`f3_cpu_16k.*` | 最终几何模式及 planned 消融，同一 BIN 对照两缓存；容量与路数同时改变 |
| `logs/host_*.log` | 八构建各完整 9208 帧宿主回放，逐行对直通 |
| `logs/samples_*.log` | 七种条带/候选跨关卡各 17 帧真实硬件采样，推进 9208 帧不写成硬件执行 9208 帧 |
| `logs/cache_bad.log`、`cache_good.log` | 强制旧缓存行后，缺少失效必须被外部 CRC 判红，正常失效匹配 |
| `logs/perf_pipeline_*.log`、`production_results.json` | 最终生产流水 TEX/STRIP/prepass 各 1200 帧；一型一次复测，不等于 N_max 或冻结三次 A/B。旧 perf_* 是串行研究快照 |
| `logs/switch_uart.log`、`surface_guard_4k.log`、`prepass_failure_pipeline.log` | 正常 UART 三模式四后端、忙态 SURF 保护、故意 prepass 错误 60 帧零交换；旧 switch.log 超时不属于通过证据 |
| `logs/board_results.json` | 队列完成后的逐消费者判定，不用程序退出码替代功能条件 |
| `f4_instance_analysis.py`、`f4_instance_stats.json` | F4 研究轮：17 组 v1 输入的合包统计（共享状态键连续区间、XY4/XYUV8、字节压缩），保守口径 |
| `f4_inst_proto.c` | F4 宿主对拍器：构造+独立展开逐字对拍与边界样例；核心实现在 `sw/apps/probe/f4Bench/src/f4_core.c`（与板测共用） |
| `logs/f4_bench_proto*.log` | F4 板上三路径测量（base/inst/instb），G4 4KB 主线 bit 纯构造；proto1/2 为过程轮，proto4 为未补 DMA padding 的研究值；修复值与生产收益另列 |
| `logs/board_queue.log`、`main_bit_restore.log` | 队列结束与 16KB 对照后主线位流恢复的原始输出 |
| `logs/interactive_restore.log`、`interactive_restore_summary.json` | 注错结束后恢复当前正常 TEX 交互版；8 个启动窗口 59～60 fps、drop 最大 1，仅为短观察 |

失败的同值回写探针、首轮几何调用、Windows 编码与矩阵计数检查的诊断记录
保留为研究证据。它们不进入正式通过的性能表。每项结果的已验证范围和采用
结论以 TechReports 正文为准。

## 图与报告

运行 `frontend_report_figures.py` 生成三张架构 SVG，`stats/plot_frontend.py`
用原值生成两张成本/记录曲线。图源在 TechReports 的 `fig/`，渲染 PNG 放
忽略 build/，普通版面检查不附加摘要。

F1 过程在 [engine 第 12 章](../../../../Engineering/TechReports/engine/12-绘制前端成本分解.md)，
F2 在 [sw 第 09 章](../../../../Engineering/TechReports/sw/09-背景面与绘制阶段.md)，
F3 在 [sw 第 10 章](../../../../Engineering/TechReports/sw/10-保序分桶与模板复用.md)，
F4 研究复核与正式实现位于 [engine 第 13 章](../../../../Engineering/TechReports/engine/13-通用实例命令.md)。
原方案研究的 `research_results.json`、`spec_review.json` 与
`probe_frontend.c` 保留，来源与决策见 engine 第 11 章；它们不作为新增板测证明。

阶段与生产汇总：`check_frontend.py --summarize --production`。生成 `cost_results.json` 和 `production_results.json`，不重复计算部署摘要。两型高负载 F3 无净收益，F2 本轮生产复测没有帧率提升，候选保持默认关闭；Todo 保留供验收。


## F4 通用实例命令

生产实现位于 lib/gfx/instances.c 与 rtl/engine/engine_inst.v，研究原型仅用于复现。
以下命令在 WSL 的仓库根运行，全部独立产物在本目录忽略的 build/f4 下：

~~~text
python3 Main/tools/check/check_f4_instances.py --models --rtl --mutations --protocols --lease --v2 --research --profile
EFINITY_HOME="<EFINITY_WSL>" python3 Main/tools/check/check_f4_instances.py --legacy
python3 Main/tools/check/check_f4_instances.py --collect
~~~

<EFINITY_WSL> 是开发环境手册所列 Efinity 安装目录的 WSL 路径。

--models 为 10000 张独立标量像素金标准与 ASan/UBSan；--rtl 为完整引擎 144 组，
--mutations 覆盖六类定向变异，--protocols 覆盖稀疏 stride 跨页、完整错误身份、
数组读错/超时排空、预取超时完整身份与软复位恢复；--lease 检查双帧独占、忙时重用与过期 generation。
--v2 在独立目录复跑 G3 的两套万张与 16/32 行 RTL，不覆盖 G3 的历史证据。

Windows 下用开发环境手册所列 Efinity Python 运行 run_f4_board.py。--build 可选
 tests-final、fallback、ab、verify、nmax-base、nmax-inst；--run 使用相同名称，
--program 为队列首项加载指定 bit；tests-final 与 fallback 始终加载各自登记的
位流。其余复用当前位流。fallback 使用研究复现时
核对并备份的 G4 位流，CAPS.INST=0；完成后用 tests-final --program 恢复 F4 位流。
工具仅执行 JTAG/DDR，不写 Flash；完整身份登记在 logs/f4_artifacts.json。

板测 tests-final 有 122 组 CRC/PERF 与 70000 实例拆包；fallback 在旧位流上先
展开成标量、保留身份映射并执行同组 CRC。ab 同一 BIN 比较 17 个冻结状态的
mode0 标量与 mode10 实例（预热 8、三轮各 30），verify 是独立诊断构建，另做
可见像素、行 padding 与 PERF 核对。finst 报告 arena 字节、两种布局的包/实例数，
不把模式 10 的记录数当逻辑 draw 数。净收益与采用结论见
[engine 第 13 章](../../../../Engineering/TechReports/engine/13-通用实例命令.md)。

--research 重建修复的研究编码器，核对 17 组与边界，并将三份输入生成到忽略目录
后做 C 编译与全字节回环。--profile 在同一 TIMED 完整引擎 fixture 上记录数组
取数、解析、几何和像素出口拍，阶段区间可重叠，不作为板上分段周期。
--collect 严格核对所有正式结果、当前程序结束边界、完整逻辑统计和旧位流身份，
生成唯一 f4_results.json。原子批构造、完整解码与实际测量产物通过其清单关联。
