# 2026-10-15 可选机制研究证据

档位quick，范围为配置守卫、隔离混合核与既有全功能位流板测。
本轮四项必需检查通过；生产特性开关、裁剪整机、Flash启动与新性能不在范围内。

| 文件 | 消费者与结论 |
| --- | --- |
| [result.json](result.json) | 研究范围的四项必需结果 |
| [plan.json](plan.json) | 实际动作、范围及未覆盖项 |
| [run.json](run.json) | 本轮输入与位流/BIN/映射网表的唯一身份登记 |
| [archive.json](archive.json) | 历史快照的版本边界 |
| [archive-files.json](archive-files.json) | 原始日志传输的唯一摘要清单；复制后逐文件校验 |
| [models.json](logs/features/models.json) | 十个合法结构组合、256守卫判定、宏与指令反例、工具版本 |
| [blend_models.json](logs/features/blend_models.json) | 四种真实隔离核共119988次像素/停顿检查，0差异 |
| [synthesis.json](logs/features/synthesis.json) | 四种厂商map通过，DSP实例1/1/1/0 |
| [board.json](logs/features/board.json) | 冻结全功能位流、22组DDR/TEX golden与本次能力读取 |
| [board_engine.log](logs/features/board_engine.log) | 本次加载边界后的fail 0/done与功能行 |
| [release_identity.json](logs/features/release_identity.json) | 已登记位流身份与复现包manifest一致，复用摘要 |
| [xml_generation_first_failure.log](logs/features/xml_generation_first_failure.log) | 首轮XML解析失败原始输出，修复后另起新轮 |

其余编译、拒绝反例、像素、map、下载与寄存器原始输出均在logs/features/。
原始日志保持字节不变，可能包含工具打印的本机路径；运行元数据已使用占位符。
快照保留运行当时的输入和结果，不表示后续源码重跑，也不表示裁剪整机通过。
重现入口见[目录说明](../../README.md)；中间工程由研究工具重新生成。
