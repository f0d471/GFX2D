# R3 顶层集成验证

从仓根运行 `python3 Main/tools/check/validate.py --checks r3 --profile acceptance --run`。
五构型含无TEX/INST的L1、保留旧TEX的L1、无INST的L2、B7 L2及TEX两拍返回。
主要三构型正式档各11帧，裁剪构型各1帧；quick每构型1帧。

场景由板测同源的 `rastTest/src/r3_cases.c` 产生；两份R1 C执行器独立产生并逐字节
核对期望。近处色键几何、后画的远处几何、共享边、跨带及最后一像素覆盖深度与色/z原子性。
真实engine_top连接未改的DMA与随机反压AXI RAM，每帧LOAD/clear/RAST/HUD/FLUSH后
逐点比较DDR以及行外保护字。

协议组检查清零先/后于LOAD、原始32位字段/33位末端拒绝、索引/记录/读响应/尾错、
首错优先级、软复位三处在途、控制层独立复位与再次启动、TRI_COUNT=0的部分带旧路径。
四种源码变异分别对准FD类别、实时参数误用、错误stage和末带过早DONE；
必须以明确断言见红，编译失败或看门狗不算检出。

综合使用Windows的 `r3-synthesis`，选择B0/B7四步与全时钟时序，默认构型不变。
`r3-legacy-board`检查同一旧功能BIN；`r3-board`检查B7的11帧三方CRC、
实时显示健康、真实索引错误/复位恢复和像素注错对照。均为JTAG路径，不证明Flash启动或人工HDMI观察。
