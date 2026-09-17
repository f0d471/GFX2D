# cacheCoherencyTest —— 实验 E3
#
# 常规编译：
#     wsl -e bash -c 'cd sw/cacheCoherencyTest && make BSP=efinix/EfxSapphireSoc \
#         RISCV_BIN=<RISCV_BIN> OBJDIR=build'
#
# 注错编译（A3 两行加 B、C 共四行必须报 FAIL (inject)，末尾 fail 0）：
#     ... make ... OBJDIR=build_inject INJECT=1
#
# 注错自检（失效调用保留，四行必须报 NOT_RED (inject)，末尾 fail 4）：
#     ... make ... OBJDIR=build_inject_selftest INJECT=2
#
# 编译档：DEBUG=yes 加上 bsp 的 soc.mk 里 DEBUG_OG?=yes，实际是 -g3 -Og。
# 判据与编译档无关，另加 DEBUG_OG=no 得到 -O0 版，上板结果相同
# （三种失效方式各占一个函数的原因见 src/main.c 文件头）。
#
# 注错宏不能用「include 之后再 CFLAGS +=」的方式加：standalone.mk 用
# $(eval) 在解析期就把编译 rule 固化了，那时命令行上的 CFLAGS 还没赋值。
# 唯一的时机是 include 之前，经 CFLAGS_ARGS 带进去（standalone.mk 第 10 行）。

PROJ_NAME=cacheCoherencyTest
DEBUG=yes
BENCH=no

INJECT ?= 0
ifeq ($(INJECT),1)
    CFLAGS_ARGS += -DINJECT_NO_INVALIDATE
endif
ifeq ($(INJECT),2)
    CFLAGS_ARGS += -DINJECT_NO_INVALIDATE -DINJECT_REPORT_ONLY
endif

STANDALONE=../../sdk/software/standalone

SRCS = 	$(wildcard src/*.c) \
		$(wildcard src/*.cpp) \
		$(wildcard src/*.S) \
        ${STANDALONE}/common/start.S

include ${STANDALONE}/common/bsp.mk
include ${STANDALONE}/common/riscv64-unknown-elf.mk
include ${STANDALONE}/common/standalone.mk

LDSCRIPT = ${BSP_PATH}/linker/default.ld
