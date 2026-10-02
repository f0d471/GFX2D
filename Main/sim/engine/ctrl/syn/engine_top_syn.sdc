# B5 engine_top 单独综合的时钟约束。
# 引擎挂在 core_clk（100 MHz，10 ns），与 SoC、DDR3 控制器的 AXI 同域（总览 2.4 节）。
# clk/rst_ctrl/rst_ddr_n 三个输入，观测寄存器 obs 是唯一的寄存器输出。
create_clock -period 10.000 -name clk [get_ports clk]
