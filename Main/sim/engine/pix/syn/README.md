# 计算阵列与片上缓存的单独综合

两个工程，路径全相对，生成物 `outflow/`、`work_*/` 不入库：

| 工程 | 顶层 | 是什么 |
| --- | --- | --- |
| `probe_pix_syn.xml` | `syn_probe_pix.v` | 前期研究探针 `probe_pix_path`（两块 128 位行缓冲 + 旋转 + 色键 + 12 核 + 写流） |
| `engine_pix_syn.xml` | `syn_engine_pix.v` | 入库 `rtl/engine/engine_pix.v` 加 12 个 `engine_blend` |

两个顶层都用 128 位 LFSR 驱动被测的全部输入（含 `ws_tready` 与行缓冲写口），全部输出折进观测寄存器，
输入不接常数（顶层输入接常数，综合会把逻辑折掉、余量虚高）。约束 100 MHz。运行 Efinity 2026.1：

```bat
call <EFINITY>\bin\setup.bat
efx_run.bat probe_pix_syn.xml
efx_run.bat engine_pix_syn.xml
```

2026-10-01 验收重跑 `engine_pix_syn.xml`：map、interface、pnr、pgm 均 PASS；
`place.rpt` 3886 XLR、12 块 RAM、12 个 DSP（`hier_util.rpt` 里本模块 3555 XLR）；
`timing.rpt` setup +5.007 ns、hold +0.031 ns，最差路径 `r_eq` → 混合核乘法器 B 口；
`map.rpt` 的 CE 表里 `u_path/en` 扇出 1247 个 FF。12 块 RAM 是源 512×128 的 7 块加目的 512×96 的 5 块。
告警只有未用的接口位（`sb_row[15:1]`、`sb_eq[3]`、`rs_tlast`、`rs_tkeep`）被删、函数局部中间量被删、
十五个不超过 64 位的小数组映射成逻辑，以及单独综合工程固有的 IO 未分配管脚。

研究探针那一个的数：3341 XLR、13 块 RAM、setup +4.898 ns。
