# 三维光栅单元验证

从仓根执行（WSL，gcc、Python、iverilog/vvp）：

```sh
python3 Main/tools/check/validate.py --checks rlst rast-ports rast-rtl --profile quick --run
python3 Main/tools/check/validate.py --checks rlst rast-ports rast-rtl --profile acceptance --run
```

`rlst` 使用独立的 FD 响应器和记录字 scoreboard；`rast-ports` 验证核心长反压、
双槽深度 RAW、提交沿取消、L1 与真实 TEX L=1/2 组合的 64 像素连续提交，
以及 TEX L=1/2 的 4096 个地址连续采样。
`rast-rtl` 连接真实 `engine_mover`、内部 BAND、未修改的 AXI DMA 核和随机反压 AXI RAM。
每带执行 LOAD 与 z-clear，等待双方完成后 RAST，排空后逐像素比较 BAND/z，
FLUSH 后再比较 DDR；不会用最后一带的 RAM 代替前面各带。

quick 为 200 条，acceptance/release 为 100000 条；u16 索引将十万条分成两帧，
记录数、条带引用数和参考提交数分别对账。启动前另外验证 CMD/RAST/INST/CMD
四次 owner 交接，旧末拍停顿时改变请求侧类别/tag，返回必须仍属于原事务。
L1 保留旧 TEX 构型，记录只清 TEX_EN；
L2 分别测一拍、两拍返回。期望由 Python 折叠，并在同初值、同 atlas、同画序下
交叉两份 C 参考器。`r1-records.json` 是已冻结 R1 流前 16 条交换记录，任何不符立即失败。

`tb_engine_rast.v` 支持 `+TRIOFF=4000/4032/4064` 检查三种记录跨页重组。
23 条定向记录覆盖共享边、跨带、严格 LESS、KEY 与原始 texel 比较、CLAMP、guard
原点、负梯度、半值舍入和非零局部行。十三类变异各自重新编译；只有明确 witness
失败才记检出，编译失败、看门狗或仅非零退出码均不算。

构建与日志统一进入 `Main/sim/build/check-runs/`，由 `validate.py` 登记身份和归档。
Windows Efinity 的 `r2-synthesis` 检查 L1/L2 独立结构与 B0/B3 消费者整机四步；
该单元检查不包含三维 CSR、B7 帧调度或板上三维画面。
