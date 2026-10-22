# R2 旧实现失败诊断

对象为 `ead734c` 的 rlst/rast/tex 与当时的测试台。2026-10-22 的只读研究使用
Icarus 插入观测，并运行不经 AXI 的旧 FD 替身；原有 `engine_pix.v`、host board.c
修改未参与这些微探针。此处不是当前源码的 acceptance，也没有真实整机或板上结论。

原始输出与诊断测试台从该次忽略目录复制一次，复制后逐文件比较字节一致。
`micro.log` 对应五处数值/地址错误；`rlst.log` 对应包络、credit、偏移和跨页地址；
`tex1/tex2.log` 对应错误 bank/数据对齐；`full-confirm.log` 区分锁存拍数与动态拍数，
同时统计非法 fire 与提前条带完成；`dma-last.log` 使用真实厂商读核澄清 TLAST。
`stream_info.json` 给出 200 条记录、249 次引用及各条带分布。

测试台应在旧版本的隔离副本编译；不能拿它们的层次路径直接强行套到重构后的 RTL。
当前版本的可执行判据由 Main 的 `check_r2.py` 注册，正式日志另有 Session 身份与
result.json，见第 17 章。原始日志中的机器路径保留原样。
