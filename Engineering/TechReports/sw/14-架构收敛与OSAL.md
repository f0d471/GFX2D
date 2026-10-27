# 14. 架构收敛与 OSAL（SW14）

状态：**本轮范围完成（2026-10-28），启动序列去重单独立包待开工**。
条目：[`../../Todo/sw/启动序列去重.md`](../../Todo/sw/启动序列去重.md)。

## 1. 本轮做了什么

SW10 判据收尾（第 13 章 4.1）暴露的架构债，本轮清理与记账：

### 1.1 已落地

- **HOME 和弦逻辑 OSAL 化**：`apps/console/src/chord.[ch]`——纯 C 的 armed
  计帧器（组合键不齐复位计数并重武装、触发后保持按住不重触发），appfw.c
  经它实例化（combo=三模式键、hold=8 帧）。宿主单测
  `apps/console/test/`（make → `build_host/test_chord`）11 项断言全过：
  第 7 帧不触发/第 8 帧触发、触发后不重复、松开重武装、中途断开复位、
  组合外多余键不影响、单键 100 帧不触发、hold=3 的 armed 语义。
  chord_test.c 在 console 本体的 wildcard 里 filter-out（test/makefile 以
  单一 `APP_SRCDIR=../src` 构建两份源，避开 `../` 混合源的模式规则问题——
  那正是第 13 章 4.1① stamp 漏洞的同族形态，整删 OBJDIR 后已不构成风险）。
- **r4 键源注入**（第 13 章 4.1③）：`r4_set_key_source()` 落在 r4.h 公共
  API，裸机缺省零变化。
- **mk stamp 整删**（4.1①）：`target.mk`/`host.mk` 的换档清理改为整删
  OBJDIR，派生头（build/replay）由规则重生成。

### 1.2 决策记录（明确不做或延后，含理由）

- **启动序列去重 → 单独立包**：盘点发现 main.c 本地版携带 stg_runtime
  缺少的判据能力（INJECT_TEX_CRC、G1/G4 对拍影子、mode 感知
  backend_apply），机械换用会删对拍/注错基础设施。升级为能力整合工作包
  （[`../../Todo/sw/启动序列去重.md`](../../Todo/sw/启动序列去重.md)），
  含完整判据链（sw6 期望再生 → 全量判据 → 板上回归 → console 复跑）。
- **七个早期诊断程序保留旧 STANDALONE makefile 体系**：ddrBurstProbe/
  ddrAliasTest/displayFifoProbe/cacheMech 等依赖已退役的探针位流，迁移后
  无法重验产物；且其 bin 身份（SHA256）已按旧档登记。维持「要改时顺手迁」。
- **lib/core/replay.c 通用链接保留**：拆分独立 LIBS 会让所有应用的产物
  身份（SHA256 记录）失效，重录成本高于 1.3KB 的收益。
- **r4_set_hooks 全局单例保留**：stg_runtime_set_active 已缓解；完整上下文化
  会牵动 R1~R4 检查链，收益不足。
- **engine_pix 可读性重构**（hw 侧在途条目）不在本批：RTL 验证按用户指令
  待统一执行。

## 2. 判据

- host 单测 `test_chord`：11 项断言，末行 `fail 0`（2026-10-28 实测）；
- console 判据全链在 chord 重构后复跑 PASS（第 13 章 4 的四项）；
- console 本体重编零告警。
