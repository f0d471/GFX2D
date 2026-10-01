# sim/engine/geom/syn

B3 的**单独综合**工程，两份：

| 工程 | 被测 | 量的是什么 | 数据在哪 |
| --- | --- | --- | --- |
| `engine_geom_syn.xml` | `rtl/engine/engine_geom.v`（入库实现） | 资源与时序，顶层是 `syn_engine_geom.v` | engine 第 03 章 3.8 节 |
| `probe_geom_syn.xml` | `../probe_geom.v`（前期研究的探针） | 「五者求交的 32 位比较链 + 行迭代的地址加法链」的余量与资源，顶层是 `probe_geom_top` | engine 第 03 章 3.5 节 |

两份都入库、路径全相对。B1 与 B2 的单独综合建在仓库外，B2 因此丢过一次整机资源对账
（两个核在整机里是 1515 XLR、16 块 RAM，而那次实验的工程已经删掉，复现不了，见
[engine 第 02 章](../../../../Engineering/TechReports/engine/02-AXI事务层.md)第六节）；
这一次谁都能重跑拿到同一组数。

## 文件

| 文件 | 是什么 |
| --- | --- |
| `engine_geom_syn.xml` / `.sdc` / `.peri.xml` | 入库实现的工程、时钟约束、Interface Designer 的设计库（工具第一次跑时生成） |
| `syn_engine_geom.v` | 入库实现单独综合的顶层：128 位 LFSR 造 I1 激励（含裁剪框）、三个方向的 ready 与 `row_free[1:0]`，253 位输出折进 128 位观测寄存器 |
| `probe_geom_syn.xml` / `.sdc` / `.peri.xml` | 研究探针的工程、约束与设计库 |
| — | 探针的顶层 `probe_geom_top` 在 `../probe_geom.v` 里 |

设备信息与工程参数从 [`fpga/gfx2d_ti60/gfx2d_ti60.xml`](../../../../fpga/gfx2d_ti60/gfx2d_ti60.xml)
原样抄；时钟约束都是 `create_clock -period 10.000 clk`（对应 `core_clk` 的 100 MHz）。
编译输出（`outflow/`、`work_syn*/`、`work_pnr*/`、`*.vdb`、`*.map.v`）在仓库根的
`.gitignore` 里，不入库。

## 跑法

```
cd Main\sim\engine\geom\syn
call <EFINITY>\bin\setup.bat
efx_run.bat engine_geom_syn.xml      入库实现
efx_run.bat probe_geom_syn.xml       研究探针
```

四步（map、interface、pnr、pgm）全 PASS 之后看：

| 看什么 | 看哪里 |
| --- | --- |
| 时序 | `outflow/<工程名>.timing.rpt`（`Clock Frequency Summary` 与 `Setup/Hold (Max/Min) Clock Relationship`） |
| 资源 | `outflow/<工程名>.res.csv`（按模块的 FF / ADD / LUT / RAM / DSP）与 `outflow/<工程名>.hier_util.rpt`（按模块的 XLR） |
| 告警 | `outflow/<工程名>.warn.log` |

## 顶层为什么要放观测寄存器

单独综合的顶层必须让综合删不掉被测的任何一位，否则没人看的逻辑会被整块剪掉
（B2 踩过一次：只观测控制位，两个核的输出 FIFO 被剪光）。两个顶层都用
128 位 LFSR 造激励，被测的每一个输出按位异或进观测寄存器，观测寄存器再回灌 LFSR——
照 B1 3.8 节与 B2 3.13 节的做法。

**拼接位宽要数着写**，这三处各踩过一次（过程写在 engine 第 03 章第四节）：

1. 观测总线比 `obs` 宽时，Verilog 不会报错、直接截断——探针首版的 248 位只接了低 128 位；
   改成 `{120'b0, obsv[247:128]}` 是 240 位，还是错，补 8 位零才对；
2. 纯组合的求交块少给一条默认赋值，综合会推断出锁存器（探针首版漏了 `dx1`/`dy1`）；
3. 入库实现的顶层是 253 位折 128 位（`{3'b0, obsv[252:128]}`），这次没有截断告警。

**输入也一个都不能接常数。** 接成常数的输入会把它后面的逻辑一起折掉：入库实现的顶层原先把裁剪框四个字段
接成 `(0, 0, 639, 479)`，求交里与裁剪框比的两级被折掉，setup 余量虚高 1.3 ns（+1.925 ns，
改成 LFSR 驱动后是 +0.572 ns，engine 第 03 章 3.8 节）。探针顶层的裁剪框右下角也接的常数，
那一组数作研究记录保留，3.5 节有订正。
