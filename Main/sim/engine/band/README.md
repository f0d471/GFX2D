# G2 BAND 前期研究

本目录仅含前期研究探针与证据，不是正式 `engine_band` 实现，未接入主工程。
研究入口为 `Main/tools/check/g2_research.py`；过程与解释归入
[engine 第 08 章](../../../../Engineering/TechReports/engine/08-条带缓冲前期研究.md)。

## 复现

WSL 中从仓库根运行，需 gcc、ASan/UBSan、iverilog、vvp 和 Python 3：

```text
bash Main/sim/engine/band/run.sh
python3 Main/tools/check/g2_research.py --prepare-syn
```

默认脚本检查全帧有效地址、RAM/分段与掩码变异、实际 geom/pix 的局部合成、
实际 mover/DMA 的装载/写回和中途排空。软件执行器现算金标准，导出器挂 sanitizer。
正常与针对性变异共 11 项符合预期才输出完成；运行失败、挂起不能算注错通过。

隔离综合工程生成于 `build/{compact_masked,compact_wide,padded_masked,segmented_masked,segmented_wide}/`。
按开发环境手册设置 Efinity 后，在各目录运行 `efx_run.bat <名称>.xml`，再从仓库根运行：

```text
python3 Main/tools/check/g2_research.py --collect-syn
```

它校验四步与时序、复制报告并逐字节核对，从网表读取物理 RAM 模式数量对上 place。
隔离工程不写入主 FPGA XML，生成 bit 仅用于工具流程，未配置到板上。

## 探针

| 文件 | 范围 |
| --- | --- |
| `probe_band_ram.v` | 一读一写、四像素掩码；直接与 2048+512 分段的组织实验 |
| `tb_probe_band_ram.v` | 2560 字全容量、掩码、6400 顺序读与 1280 异址并发、两个变异 |
| `syn_probe_band_ram.v` | 全变量激励/全位观测的隔离包装，参数由生成工程选择 |
| `probe_band_local.v` | 计算代理：源/目的同步流、紧凑地址、带内标记和写反压；没有 DMA/ABI/排空 |
| `g2_research.py` 生成的 `tb_band_path.v` | 保留实际 geom/pix/blend；6 条带边界拷贝不计时；对拍完整 512 KiB |
| 生成的 `tb_probe_band_dma.v` | 保留实际 mover 与 DMA；推荐分段 RAM 接 E4 校准从机，独立测装载/写回/取消 |

计算和搬运是两个分开的实验，尚无统一 BAND 控制器。全部数据和行协议相符，
不等于主工程已经支持 BAND，也不等于板上性能提升。

## 证据索引

| 文件或组 | 消费方 |
| --- | --- |
| `logs/source_refs.json` | 两份代码级参考、固定版本/许可及厂商 RAM 依据 |
| `definition_checks.json` | 76,800 拍无别名、stride 和 I3 数据下界 |
| `ram_*.log`、`band_*.log` | 原始正常/对应变异；值、内存、握手与完成状态 |
| `research_results.json` | 对齐/跨条带周期、DMA 三相、排空和独立模型偏差 |
| 五组 `*_place/timing/hier_util.rpt`、`*_synthesis.log` | 原始资源及时序/四步输出 |
| `synthesis_results.json`、`ram_physical_modes.json` | 工具结果与模式数量，推荐 36 块研究 RAM |
| `g2_board_baseline.log`、`board_results.json` | 现行 G1 基线的 22 个 CRC/六项错误/性能/扫描，不是 G2 板测 |
| `board_artifacts.json` | 当前 bit 和新编 BIN 的唯一完整摘要及消费者，首次 --record-board 写入 |
| `dependency_{geom,mover,pix}.log` | 正式依赖回归 27/27、17/17、25/25 |
| `segmented_parse_fail.log` | 分段探针初次 map 失败，修正保留字之后重新通过 |

`python3 Main/tools/check/g2_research.py --check-board` 只复核已保存板测，不重复计算摘要。
硬件或程序重新构建、复制或板测对象变化时需重新核对身份，不能把旧清单当成新对象。
本轮仅 JTAG 配置当前基线与 DDR 程序装载，Flash 未改，9208 帧回放未重跑。

构建物使用忽略的 `build/`，可复用研究源码与原始结果保留在本目录。
正式 G2 判据和接续范围见
[G2 条目](../../../../Engineering/Todo/hw/引擎P3-G2条带缓冲.md)。
