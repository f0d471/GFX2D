# sim/engine/geom

引擎 P1 **B3（几何层）** 的仿真目录。里面有两样东西：入库实现的**单元仿真**，
与 B3 前期研究的**探针**（研究做完了，保留为回归）。

B3 已于 2026-10-01 验收，Todo 条目已删除；研究、定案、实现与全部数据在 [`../../../Engineering/TechReports/engine/03-几何层.md`](../../../Engineering/TechReports/engine/03-几何层.md)。

## 单元仿真（B3 的交付物）

| 文件 | 是什么 |
| --- | --- |
| `tb_engine_geom.v` | 被测是 `rtl/engine/engine_geom.v`。测试台按 I1 喂命令，收 I2 的描述符与 I5 的边带，与独立模型给的期望值**逐字段比**；另查 D3 规矩 1、D7 的读源/读目的有无、零面积命令的完成脉冲、命令发完之后 `busy` 落下来。另有与消费循环无关的**握手监视器**（逐拍数三个通道的握手与 `cmd_done`，抓多发的描述符）与 **B4 行缓冲替身**（给 `row_free[1:0]`，抓开在还占着的那一半上的行） |
| `run.sh` | 一次跑完：现算期望值 → 探针 → 单元仿真（三组输入）→ 两个握手用例 → 五个注错 |

**期望值不写死**，每次由独立模型现算：

| 输入 | 命令 | 期望记录 |
| --- | --: | --: |
| 定向用例 34 条（`--dump-directed`） | 34 | 1020 |
| 159 条向量里几何层见到的命令 | 59 | 338 |
| 十一整帧用例的全部命令 | 17 | 5224 |

记录格式（类型 0 命令头 / 1 写描述符 / 2 读描述符 / 3 边带 / F 结束）与字段表
（`REC_CMD`/`REC_SB`/`REC_RD`/`REC_WR`）在 `Main/tools/check/geom_model.py` 里，
是模型与测试台之间的唯一真源；记录的先后就是 RTL 的发射顺序
（每行：边带 → 读源 → 读目的 → 写目的）。

**两个握手用例**（判据都是「序列与恒 ready 时逐条相同」）：
`+STALL` 给三个方向各压出反压，`+RFSTALL` 把 B4 行缓冲替身的排空拉长到 23 拍，
下一行与下一条命令的第 0 行都必须真的等 `row_free`（定向、向量、整帧三组都跑）。

**五个注错**：`INJECT=1` 漏第五项求交、`INJECT=2` 跨 4KB 不拆、`INJECT=4` ALPHA 误开
读目的避让、`INJECT=8` COPY 多发读目的、`INJECT=16` 行开始不看 `row_free`（配 `+RFSTALL`）。
位序与 `rtl/engine/engine_geom.v` 的文件头一致；改的都是那一段真实逻辑，
`run.sh` 逐个查 FAIL 的标记，确认它们红在**对应的**判据上。

全套 `SIM OK: 27/27 checks as expected`。

## 研究探针（保留为回归）

| 文件 | 是什么 |
| --- | --- |
| `probe_geom.v` | 探针 RTL。四个模块：`probe_geom_intersect`（纯组合的五者求交）、`probe_geom_d7`（D7 读写避让的判定）、`probe_geom_rowiter`（多拍顺序的行迭代 + 描述符拆分 + I5 边带）、`probe_geom_top`（单独综合的顶层）。**不是**入库实现 |
| `tb_probe_geom.v` | 逐拍测试台：核对 D7 判定表，量每行/每命令的拍数（FILL COPY 5.06、COPY BLIT 5.06、ALPHA BLIT 6.06、整屏 FILL 5.00），检查 D3 规矩 1 与 D7，三条注错各自见红（14/14） |
| `syn/` | 两个单独综合工程：探针与入库实现各一份，见 `syn/README.md` |

## 跑法

```bash
wsl -e bash -c 'cd Main/sim/engine/geom && bash run.sh'
```

产物落在 `build/`（不入库）。`syn/` 要 Windows 上的 Efinity
（`<EFINITY>` 是 `Main/doc/开发环境.md` 第 1 节的占位符）：

```
cd Main\sim\engine\geom\syn
call <EFINITY>\bin\setup.bat
efx_run.bat engine_geom_syn.xml      入库实现的资源与时序
efx_run.bat probe_geom_syn.xml       研究探针的资源与时序（3.5 节）
```
