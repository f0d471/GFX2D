# F1 首轮测量源码

`main.c`、`consume.c` 与 Makefile 是首轮 `frontendBench` 的冻结源码，链接
提交 `1e2cd8d` 的生产库和游戏，避免后续 F2/F3 开销进入原研究基线。
固定输入与计时边界沿 `f1_baseline_4k.log`，两型六档及五个录像状态，三次
各 1000 帧。

从仓库根在 WSL 执行：

```text
python3 Main/tools/check/rebuild_frontend_baseline.py --riscv-prefix <RISCV_BIN>
```

工具在现有忽略 build/ 内导出原提交的 `Main/sw`、`Main/sdk` 与录像生成工具，复制本目录
三份源码与现有素材生成物，不修改当前生产树；重建后与唯一产物清单的预期
身份比较，失败即退出非零，结束时清理独占临时目录。

已部署 BIN 为 124430 B。隔离重建与原文件全字节比较相同、差异 0；原结果
在 `logs/baseline_reproduction.json`。独立复现工具的登记身份比较结果在
`logs/baseline_registered_reproduction.json`，完整摘要复用 `logs/artifacts.json`，
不另抄长哈希。本目录保留原研究实现；正式功能使用 `sw/apps/probe/frontendBench`。
