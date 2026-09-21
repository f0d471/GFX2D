# displayFifoProbe —— 实验 E5 2.4 节：真实扫描 FIFO 的压力矩阵与深度定值
#
# 只在 Ti60 上运行，**不提供 host 构建**：它要读 display / guard / axiprobe
# 三个 APB 窗口的量测寄存器，这些不是跨平台的游戏平台能力（见 E5 2.4.2 节）。
# 所以这里直接 include target.mk，不走 PLATFORM 二选一。
#
#   make                            产物 build/displayFifoProbe-O2.bin
#   make RISCV_BIN=<RISCV_BIN>      工具链前缀见 ../../../../doc/开发环境.md 第 11 节
#
# 判据与期望输出见 src/main.c 文件头、仓库根目录
# Engineering/Todo/hw/E5-Axi_Mux优先级与扫描FIFO水位.md
# 与 Main/sw/README.md「displayFifoProbe 的期望值」。

APP      = displayFifoProbe
APP_SRCS = $(wildcard src/*.c)
LIBS     = core

include ../../../mk/target.mk
