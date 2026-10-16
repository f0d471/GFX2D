# BLIT 隔离对照

本目录保留修复前的 quick 定向诊断，不是当前源码完整验收。
B0 的 FILL 正常，B0/B1 的 BLIT 超时；B2 与仅补齐 TEX 输入的 B0 正常。
两轮实际参数及未完成范围见各自 plan.json.log/result.json.log，
原始解释与复现脚本以 .log 快照保留，候选唯一 RTL 差异见 candidate.patch。
日志与快照复制后逐字节摘要比较，身份集中在 archive-files.json。
