# 构型功能判据

同一个程序在七个代表构型运行，不要求 TEX、STRIP、INST 或 PB 存在。
六方程与色键按代表档共同具备的像素能力执行，DDR 的多行非对齐读写、多命令序列、
错误恢复与保护字逐项检查。TEX、实例和 PB 根据运行时 CAPS 执行或验证关闭态。
STRIP 通过实际控制位读回识别，并覆盖 PB 完成后直接切换到 STRIP。

板测入口是 `tools/check/check_feature_board.py`，经 `validate.py --checks feature-board`
编排。外部检查器逐案计算 CRC，要求每种用例恰好一行、错误三元组正确、
串口未丢失及唯一 `fail 0` 与 `done`；程序自身的退出码不能代替功能判据。
PB 用例覆盖一条 16 行条带的 LOAD/EXEC/FLUSH 与行尾填充保护。

DDR 使用既有分区：CMDLIST_A 放命令，SHARED 放源像素与 PB 工作区，
FB0 放结果，FB1 放 PB 背景。构型板测不测性能，不写 Flash。
