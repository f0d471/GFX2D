# 外部变异闭环证据

入口说明和因果过程见 [第 17 章](../../17-变异测试全量迁移.md)。

只读复核：从仓库根运行 `python3 Engineering/TechReports/platform/evidence/17-mutation-closure-20261114/verify_coverage.py`。
该脚本验证原始输出、变异源码摘要、工具链、影响范围及注释等价证明，不运行或修改设计。

| 目录/文件 | 范围 |
| --- | --- |
| `full-diagnostic/` | 原全量 acceptance：28 基线通过，76 KILLED/3 WEAK，原 FAIL 状态保留 |
| `affected-final/` | 两条精确标签与七条 mover 变异重验，5 基线通过、9 KILLED |
| `affected-final/logs/mutation/coverage-review/coverage.json` | 逐项 79 条索引：70 未影响冻结记录 + 9 重验，保留各自版本，不是单轮拼接 |
| `consumers/` | 六项普通完整回归冻结快照与文档/输入登记差异审查 |
| `mover-final/` | 新末拍检查器的五项正常用例 |
| `f5-fixture/` | H16 普通/定时、H32 当前素材整帧与 lease/能力回退 |
| `workflow-final/` | 八项跑器行为反例、公共 workflow、寄存器生成物一致性 |
| `migration/` | 四构型 19 文件生产展开、75 历史反演、79 非空激活 |
| `synthesis/` | 四构型真实四步与资源/时序/网表；current-input-review 对当前编译输入复核，原始报告仅保留一份 |
| `comment-review.json` | 验收后只同步五文件旧机制注释，可执行内容及行数不变 |
| `device/usb.log` | 实际 USB 扫描未识别目标；不代表板测通过 |

`archive.json` 区分当前输入重验与历史冻结快照。原始日志逐字节保留，不能仅复制其中的 PASS 文本
宣称其它源码、构型、档位或板卡状态已验证。
