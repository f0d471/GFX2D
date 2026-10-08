# G1 TEX 验证与研究

正式实现为 `rtl/engine/engine_tex.v`，已接入引擎与 SoC。规格、研究、修复和板测见
[engine 第 07 章](../../../../Engineering/TechReports/engine/07-片上纹理前期研究.md)。

## 正式验证

WSL 中从仓库根运行 `bash Main/sim/engine/tex/run.sh`。依赖 gcc、iverilog、vvp、Python 3。
软件执行器现算 512 KiB 金标准，74 条记录覆盖 TEX/DDR 精灵、五批取数和 640 像素长行。
验证 APB 全窗口、未对齐拒绝、读延迟 1/2 拍、状态反压、DDR 请求重叠及 FIFO 等待、
完整内存、PERF、错误与两种复位。四个变异须分别命中 valid/data、写副作用、状态缺失
及流碰撞，全部符合预期才输出 `SIM OK: 6 checks as expected`。

总输出为 `logs/tex_regression.log`，逐拍输出为 `logs/formal_*.log`。依赖回归为
geom 27/27、mover 17/17、pix 25/25、ctrl 4/4、frame 16/16。`host_regression.log`
核对软件 64/32 位与独立 ref_exec；`golden_sanitizer.log` 检查导出器内存访问。
构建与临时输入放忽略的 `build/`。

## 综合

`python3 Main/tools/check/g1_research.py --prepare-tex-syn` 在 `build/` 生成正式隔离工程，
全部输入为变量、输出全位观测。在该目录按开发环境手册运行
`efx_run.bat engine_tex_syn.xml`。`logs/tex_{place,timing,hier_util}.rpt` 和
`tex_synthesis.log` 为正式独立工程；`logs/full_*.rpt` 与 `full_synthesis.log`
为整机证据。单独工程不能代替整机收敛。

## 板测

固定负载在 `Main/sw/apps/test/engineTest/logs/g1_board.log`；N_max、真实游戏同帧、
切换和失败路径在 `Main/sw/apps/game/gfx2dGame/logs/g1_*.log`。采集用独立末行 done
的前导换行标记，避免 nmax done 提前结束。外部核对命令为
`python3 Main/tools/check/check_g1_logs.py`，`--board-only` 只检查固定负载。
完整结果为 `logs/acceptance_results.json`；产物身份、构建参数和消费日志集中在
`logs/g1_artifacts.json`。完整摘要只保留在该清单。中断、超时、修复前日志单独
保留，不作为最终验收输入。板测为 JTAG 配置与 DDR 程序装载，未修改 Flash。

2026-10-08 验收复核为 `logs/acceptance_review.json`，`acceptance_*.log` 保存本次
从当前源码重跑的正式仿真、依赖回归与软件检查。综合和板测复用已核对身份的
原始记录，本次没有重新综合或接板。

## 研究起点

`probe_tex_stream.v`、`probe_tex_ram.v`、`tb_probe_tex_ram.v` 保留前期实验。
`python3 Main/tools/check/g1_research.py` 复现原 DDR/本地延迟研究，`--prepare-syn`
生成 RAM 探针工程；它们不代替正式 APB、重叠和排空验证。研究结果在
`research_results.json`、`definition_checks.json`、`ram_*.rpt` 及
`tex_l1/tex_l2/tex_bad_valid.log`。原 `artifact_manifest.json` 是历史 P1 对照，
其中固定路径的当前文件已由 G1 重建；`--record-board` 只比较原 B6 身份。
