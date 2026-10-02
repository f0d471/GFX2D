# cacheCoherencyTest —— E3 回归（B6 改写版）：引擎当外部写入者与第二个读者
#
# B6 接入引擎时 axi_probe_master 按引擎 P1 的 D6/D22 从顶层撤下，本程序的外部
# 写入者从探针主机换成引擎（FILL COPY），第二个读者换成引擎 BLIT。构建也换成
# 项目分层（要走 gfx_engine 驱动，驱动在 bsp/ti60）：直接 include target.mk，
# 与 displayFifoProbe、engineTest 同一做法。E3 的机理实验已归档 platform 第 05 章，
# 探针版的基线日志在 logs/b6_preintegration_20261002.log（撤探针前留档）。
#
# 常规编译（末尾 fail 0）：
#     wsl -e bash -c 'cd sw/apps/test/cacheCoherencyTest && \
#         make RISCV_BIN=<RISCV_BIN>'
# 注错编译（A3 两行加 B、C 共四行必须报 FAIL (inject)，不计入 fail，末尾 fail 0）：
#     make INJECT=1 OBJDIR=build_inject
# 注错自检（失效调用保留，四行必须报 NOT_RED (inject) 并计入 fail，末尾 fail 4）：
#     make INJECT=2 OBJDIR=build_inject_selftest
#
# INJECT 经 target.mk 变成 -DINJECT_1 / -DINJECT_2。
# 三种失效方式各占一个函数的原因见 src/main.c 文件头（platform 第 05 章 T10）。

APP      = cacheCoherencyTest
APP_SRCS = $(wildcard src/*.c)
LIBS     = core gfx

include ../../../mk/target.mk
