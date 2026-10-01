# probe_geom 单独综合的时钟约束。
# 引擎挂在 core_clk（100 MHz，10 ns），与 SoC、DDR3 控制器同域，见总览 2.4 节。
# 只有 clk 与 rst 两个输入（rst 是同步复位，探针与顶层都在 posedge clk 里判它），
# 观测寄存器 obs 是唯一的寄存器输出。
create_clock -period 10.000 -name clk [get_ports clk]
