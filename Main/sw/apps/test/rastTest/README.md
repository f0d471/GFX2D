# rastTest

host入口保留R1双目标最小参考测试；target入口运行R3板上验收。
目标程序要求具有RAST的B7，旧构型明确报缺能力。

`r3_cases.c`是板测与RTL共同的确定性输入，生成16条setup记录和64×64 atlas；
期望由两份R1参考执行器交叉核对。target在固定前缓冲持续扫描时向另一帧缓冲渲染，
每条带先三维、后HUD、再写回，比较全部可见像素及padding，输出11帧CRC。
健康统计在预热后清零，负载前/中/后读取ZERO/UNDER/RESPERR并核对快照场号推进。

还执行驱动拒绝、真实BAD_INDEX、软复位及恢复。`R3_INJECT_PIXEL`只用于板上
预期失败对照，修改完成帧的一个像素，必须被CRC及逐像素判据发现。
它不改变生产硬件；目标构建以 `mk/target.mk` 为准。
