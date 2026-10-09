# 绘制前端研究与后续验证

本目录目前只有研究探针与摘要，没有前端优化或新增 RTL 的验收结果。
路线与实施接口见 [绘制前端总览](../../../../Engineering/Todo/sw/绘制前端总览.md)，
研究过程见 [engine 第 11 章](../../../../Engineering/TechReports/engine/11-绘制前端方案研究.md)。

从仓库根在 WSL 运行：

```text
python3 Main/tools/check/research_strip_frontend.py
```

需要 gcc、Python 3 和现有 `Main/assets/build/assets.pak` / `assets.h`。
工具重新分析 G4 三份 N_max CSV，并链接真实游戏与构造库：固定种子、两种弹型、
七个 K 档、推进 1200 步、取 120 帧，共 1680 份状态。观察点只插入 build/ 内的
源码副本；每份都与未改生产入口比较完整直通表/条带表与构造记账。失败退出非零。

`research_results.json` 保存参数、源码版本、编译器、回归样本数与阶段均值。
`probe_frontend.c` 负责观察与生产输出的等价检查。原始 CSV 已在 G4 中登记，
这里仅引用路径，不复制原始日志或重复登记摘要。生成的可执行文件、插桩副本与
逐帧中间 CSV 在忽略 build/ 中，成功后自动清理。

计数实验只证明记录规模；历史回归只是观测模型。这里没有新的板上周期、
N_max、同帧像素、综合或资源结论。F1~F6 的实现证据按各 spec 的门槛另行增加。

`spec_review.json` 记录本轮规格审查：相对链接、旧条目移除、生产路径未改、拟定
寄存器偏移无冲突，以及 SVG 的渲染/目检。它不作为硬件或性能验收记录。
