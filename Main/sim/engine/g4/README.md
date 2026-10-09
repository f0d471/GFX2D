# G4 整机条带接入与性能验证

本目录保存真实游戏研究复现、正式整帧仿真、完整回放与板上性能证据。
结论与判据见 [engine 第 10 章](../../../../Engineering/TechReports/engine/10-整机与Nmax实测.md)。

从仓库根在 WSL 运行：

~~~text
python3 Main/tools/check/check_g4_integration.py --host
python3 Main/tools/check/check_g4_integration.py --rtl
python3 Main/tools/check/check_g4_integration.py --synthesis Main/fpga/gfx2d_ti60
~~~

--host 运行 1 万张批量/逐条构造 sanitizer 差分，以及完整 9208 帧生产
v1/v2 回放逐行对拍；--rtl 使用真实素材、生产游戏终符 K1024 状态，
独立直通软件金标准对完整 engine_top，检查 TEX、条带、可见区、padding、
DDR 事务与 PERF，并要求三种变异见红。构建与工件进入忽略的 build/。

板上采集使用 tools/board/board_run.py，矩阵入口为
tools/board/run_g4_matrix.py；需先按开发环境手册构建对应程序。
--samples / --replay 复核已采集日志，不操作板卡；--register 登记
部署对象身份并按大小、修改时间缓存，同一对象不反复计算摘要。
完整摘要唯一清单为 logs/artifacts.json。

| 工件 | 用途 |
| --- | --- |
| test_batch.c | 条带批量与逐条构造的字节、返回计数、错误、关闭、别名与溢出差分 |
| tb_g4_frame.v | 完整真实游戏帧与三种 RTL 变异 |
| logs/research_k*.log、research_search_summary.log | 四档与完整研究搜索复现 |
| logs/host_results.json、rtl_results.json | 正式宿主与 RTL 判据汇总 |
| logs/4k_synthesis.json、16k_synthesis.json | 四步编译、资源与全域 slack |
| logs/compare_samples_board.log | 跨关卡 17 帧 CRC 与 PERF 对拍 |
| logs/nmax_strip_4k*.log、nmax_strip_16k.log | 修复前后与缓存对照的真实搜索 |

16KB 复制工程位于 build/cache16/，只改变数据缓存容量和路数；不替换主线
SoC 或 SDK。32 行对照属于可选阶段，尚未登记板测结论。

## 研究统计复现

在 sw/apps/game/gfx2dGame 中构建 host，追加
-DGFX2D_STRIP_RESEARCH=1 -DGFX2D_START_MODE=3 -DGFX2D_PRESS_EXIT=1
-DGFX2D_HEADLESS=1，并设置 GFX2D_UNPACED=1 运行。rcsv 由
../stripctl/g4_frame_census.awk 聚合；完整搜索按 nmax result 切分弹型。
固定档将 PRESS_EXIT 换为 GFX2D_PRESS_KNOB=2400/2800/3200/3600 与
GFX2D_FRAMES=1480，聚合时 -v fixed_k=<K>。主机周期只作过程登记。

最终汇总为 logs/board_results.json；部署身份唯一登记为 artifacts.json。
nmax_strip_4k_first.log 是定位用诊断记录，旧 BIN 未保留，不作为发布版本
的可复现性能对照。正式表只引用 nmax_strip_4k.log。
