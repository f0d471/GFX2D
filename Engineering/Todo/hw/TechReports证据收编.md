# TechReports 证据收编

## 状态

未开工（2026-10-28 立项，随全仓文件治理裁决产生）。不阻塞任何代码线，可穿插做。

## 1 要解决什么

三个 TechReport 系列各有一个 `evidence/` 目录，判据原始日志按轮次整包入库，
共约 40 MB / 2980 个文件，正文按路径引用 68 处。读者要从正文跳进 evidence 翻原始行，
目录树深、命名按轮次自增，检索靠猜，认知负担大。2026-10-28 裁决定下方针：
**证据的决定性行收进对应章正文，目录撤销。**

| 目录 | 规模 | 对应章（正文引用处数） |
| --- | --- | --- |
| `engine/evidence/` | 32 MB / 2722 文件 / 40 子目录（R1~R4 十轮） | engine 16（1）、17（12）、18（15）、19（20） |
| `sw/evidence/` | 6.9 MB / 218 文件（11-pc-wide、12-visual-gameplay） | sw 11（14）、12 |
| `platform/evidence/` | 944 KB / 40 文件（含 8 个 records.zip） | platform 12（6） |

## 2 怎么切分

1. platform 12：先解开 8 个 `records.zip` 逐一确认正文已覆盖其内容，再收编删目录。
2. sw 11：delivery / review / software 等 8 组收编后删。
3. sw 12：snapshots 为图片证据，收编时决定是引用 Deliverables 已有图还是少量关键图入库。
4. engine 16~19：按章逐个收编（大头，40 个子目录；16 只 1 处引用，19 有 20 处）。
5. 每收编一章：正文引用点改为内联关键行/数字 → 删对应 evidence 子树 →
   `grep "evidence/"` 确认该章正文零残留。

## 3 判据

- 三个 `evidence/` 目录从仓库消失；三个系列的 `.md` 正文 `evidence/` 引用零命中。
- 收编后的章正文自含决定性数字与关键日志行，读者不开原始日志就能核对该章结论。
- **不在收编范围**（判据体系直接消费或产物身份文件，保持原状）：
  `Main/sim/engine/*/logs/`（2950 个）、`Main/sw/apps/*/logs/`（152 个）、
  `Main/sim/engine/tex/logs/g1_artifacts.json`。

## 4 开发日志

- 2026-10-28 立项。背景：全仓文件治理盘点与清理（`Main/E:` 意外目录、约 190 个 sw
  构建目录、12 GB `Main/sim/build` 暂存区、sim/sw 构建产物、assets 烧录 hex 等
  约 15 GB 已于当日清理，均为 git 忽略/未跟踪内容，版本库零变化）。
  evidence 收编因涉及 git 跟踪内容与正文改写，按裁决单独立条目。
