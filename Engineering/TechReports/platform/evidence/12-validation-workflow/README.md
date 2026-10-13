# 验证流程改造证据

各目录是运行结束时的快照，`archive.json` 标明不据此重判当前源码；原状态与缺项保留。
`archive-files.json` 记录原始日志身份，复制后已自动核对。元数据的机器路径使用占位符，原日志字节不变。
每轮原始记录集中在 records.zip，解压回读已通过 CRC 与日志指纹检查；archives.json 是证据包身份清单。
summary.json 给出范围与状态，完整输入清单在包内；host-rtl 的 run.json 另留供复用分析直接读取。
pack_evidence.py 保留本次归整与复核方法，不属于日常验证入口。

| 目录 | 范围与结果 |
| --- | --- |
| host-rtl | 12 项 quick 组合检查通过：流程行为、寄存器、语义、库、应用、游戏、统计、Demo、实例、分桶、条带数据面与控制 |
| synthesis-build | 本轮四步编译通过；保留当时 Programmer 环境不完整导致的板测 INCOMPLETE，不冒充该轮板测通过 |
| early-banner-diagnostic | 实际程序输出完整，旧判据因横幅早于加载边界而拒绝；保留失败及原始串口 |
| instance-pb-board | 引擎 22、实例 122、PB 74 组板上 CRC；PB 四组错误判据；完整下载与结束证据 |
| timing-review | 本轮已建整机的报告复核，16 条时钟关系均非负；跨时钟负 slack 的消费者由行为反例覆盖 |
| workflow | 最终 23 项流程行为测试及 DDR 日志 sink 判据通过 |
| engine-board | 最终板测入口的 22 组独立金标准 CRC、两次调试连接与完整部署采集通过 |
| archive-review | 旧完整 F4 归档只读复核通过；保留历史版本边界，不代表重新执行当前源码 |
| reuse-analysis.json | 生产输入保持不变，15 个宿主／RTL检查生产函数与已登记的原运行源码 AST 一致；后续仅重验受影响的编排与解析 |
| reuse_audit.py、reuse-inputs/ | 冻结的原检查源码与复核程序；运行 `python3 Engineering/TechReports/platform/evidence/12-validation-workflow/reuse_audit.py` 复核，源树后续变化时可按预期拒绝 |

宿主／RTL检查的原始记录保留其运行版本。后续工具修订不改变对应生产代码与检查函数，
复用依据由 reuse-analysis.json 给出；新的状态／依赖／归档／解析逻辑以 workflow 和 engine-board 结果为准。
本轮位流包含单独保留的 engine_pix 注释改动，流程提交不包含该外部文件。
没有新增 N_max、完整硬件游戏回放、Flash 或人工 HDMI／按键结论。
