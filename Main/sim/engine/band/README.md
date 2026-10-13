# BAND 条带数据面验证与前期研究

正式数据面已实现并于 2026-10-08 验收，默认 `run.sh` 为正式验证。主帧控制已接入，
研究探针与历史证据继续保留。

## 正式复现

WSL 中从仓库根执行，需 gcc、ASan/UBSan、iverilog、vvp 和 Python 3：

范围以 `Main/doc/验证流程.md` 为准。新运行的 build 与 logs 位于
`Main/sim/build/check-runs/<运行号>/`，续接命令必须传同一 `--run-dir`；本目录 logs 保留历史证据。

```text
bash Main/sim/engine/band/run.sh
python3 Main/tools/check/check_g2_band.py --prepare-syn
```

9 个正常检查、8 个针对性变异全部符合预期才输出 SIM OK。完整通路实际例化
geom/pix/blend/mover/BAND/TEX 与两个 DMA 核；内存逐 128 位对软件执行器，
每组完整比较 512 KiB。含混合源、负 x、跨条带、重叠、色键、取数并发、空条带、
原始高字节及 8/16/32 行；控制测试含 4KB 拆分、迟到数据超时、取消与错误重试。

| 文件 | 职责 |
| --- | --- |
| `tb_engine_band.v` | 首尾/分段边界、本地状态/标签、地址/keep 拒绝、同址冲突、装载取消与独立背景/目的 stride 重试 |
| `tb_engine_band_mover.v` | 真实 DMA/AXI、超时/取消/错误恢复、FIFO 十拍前缀与排空 WSTRB |
| `tb_engine_band_path.v` | 全部真实数据面核、软件金标准、三相计时和数据/流纪律 |
| `syn_engine_band.v` | 全变量输入、按 valid 分相观测，防止同源输出 XOR 裁剪 RAM |

独立工程在该轮 build/band/syn，按开发环境手册运行四步后（使用准备时的同一 --run-dir）：

```text
python3 Main/tools/check/check_g2_band.py --collect-syn
python3 Main/tools/check/check_g2_band.py --collect-main
python3 Main/tools/check/check_g2_band.py --record-board
```

--collect-syn 要求 36 RAM 与网表物理数量一致、setup 为正、hold ≥0.026 ns。
--collect-main 核对新四步、全部时序、六个已登记未用接口和最新告警。
主工程编译时将四步控制台输出重定向到其忽略目录 outflow/g2_synthesis.log，
例如在主工程目录执行 `efx_run.bat gfx2d_ti60.xml > outflow/g2_synthesis.log 2>&1`。
--record-board 复核新 formal_board_legacy.log 后登记当前 bit/BIN 完整身份一次；
产物未变时不重复登记。它不是上传/Flash 工具，也不证明 BAND 已上板启用。
单组定位用 --case path_timed 等；单组会更新结果汇总，交付前用默认完整运行恢复。

正式证据：formal_results.json、formal_*.log、formal_synthesis_results.json，
独立 formal_{place,timing,hier_util}.rpt 与主工程 formal_main_*。
formal_bad_observer_* 是 RAM=0 的失败观测报告，保留为试错，不能计通过。
formal_board_results.json 与 formal_artifacts.json 绑定新 bit 的 DDR/TEX 22 CRC
兼容性板测，BAND 帧模式/N_max 归条带控制与整机轮次。formal_regression_*.log 为原十二套回归。

## 研究复现

WSL 中从仓库根运行，需 gcc、ASan/UBSan、iverilog、vvp 和 Python 3：

```text
python3 Main/tools/check/g2_research.py
python3 Main/tools/check/g2_research.py --prepare-syn
```

研究脚本检查全帧有效地址、RAM/分段与掩码变异、实际 geom/pix 的局部合成、
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
| 生成的 `tb_probe_band_dma.v` | 保留实际 mover 与 DMA；推荐分段 RAM 接校准从机，独立测装载/写回/取消 |

前期研究的计算和搬运是两个分开的实验。其数据和行协议相符，
不等于主工程已经支持 BAND，也不等于板上性能提升。

## 证据索引

| 文件或组 | 消费方 |
| --- | --- |
| `logs/source_refs.json` | 两份代码级参考、固定版本/许可及厂商 RAM 依据 |
| `definition_checks.json` | 76,800 拍无别名、stride 和读流数据下界 |
| `ram_*.log`、`band_*.log` | 原始正常/对应变异；值、内存、握手与完成状态 |
| `research_results.json` | 对齐/跨条带周期、DMA 三相、排空和独立模型偏差 |
| 五组 `*_place/timing/hier_util.rpt`、`*_synthesis.log` | 原始资源及时序/四步输出 |
| `synthesis_results.json`、`ram_physical_modes.json` | 工具结果与模式数量，推荐 36 块研究 RAM |
| `g2_board_baseline.log`、`board_results.json` | 现行 TEX 基线的 22 个 CRC/六项错误/性能/扫描，不是条带板测 |
| `board_artifacts.json` | 当前 bit 和新编 BIN 的唯一完整摘要及消费者，首次 --record-board 写入 |
| `dependency_{geom,mover,pix}.log` | 正式依赖回归 27/27、17/17、25/25 |
| `segmented_parse_fail.log` | 分段探针初次 map 失败，修正保留字之后重新通过 |

`python3 Main/tools/check/g2_research.py --check-board` 只复核已保存板测，不重复计算摘要。
硬件或程序重新构建、复制或板测对象变化时需重新核对身份，不能把旧清单当成新对象。
本轮仅 JTAG 配置当前基线与 DDR 程序装载，Flash 未改，9208 帧回放未重跑。

构建物使用忽略的 `build/`，可复用研究源码与原始结果保留在本目录。
