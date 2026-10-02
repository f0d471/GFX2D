# engineTest —— 引擎 P1 B6 的板上判据：同一张命令表，软件执行器画一份、引擎画一份
#
# 只在 Ti60 上运行，不提供 host 构建：软件执行器在 PC 上有更完整的判据链
# （sw/lib/test 的 45 条向量与十一个整帧金标准），板上这版要的是「引擎这一面」
# 与「软件这一面」在同一块 DDR 上的对账，以及扫描 FIFO 在引擎负载下的水位。
# 所以这里直接 include target.mk，不走 PLATFORM 二选一（与 displayFifoProbe 同一做法）。
#
#   make                            产物 build/engineTest-O2.bin
#   make INJECT=1                   注错构建：金标准第 0 例读回前跳过失效，
#                                   该例的 CRC 比对必须不同（设计文档 3.8 节，E3 规则）
#
# 判据与期望输出见 src/main.c 文件头与 Main/sw/README.md「engineTest 的期望输出」。

APP      = engineTest
APP_SRCS = $(wildcard src/*.c)
LIBS     = core gfx

include ../../../mk/target.mk
