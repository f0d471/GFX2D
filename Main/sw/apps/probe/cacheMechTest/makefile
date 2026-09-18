# cacheMechTest —— 实验 E3 的机理实验（T1~T11），需要带 axi_probe_master 的比特流
#
#     wsl -e bash -c 'cd sw/apps/probe/cacheMechTest && make BSP=efinix/EfxSapphireSoc \
#         RISCV_BIN=<RISCV_BIN> OBJDIR=build'
#
# 编译档：DEBUG=yes 加上 bsp 的 soc.mk 里 DEBUG_OG?=yes，实际是 -g3 -Og。

PROJ_NAME=cacheMechTest
DEBUG=yes
BENCH=no

STANDALONE=../../../../sdk/software/standalone

SRCS = 	$(wildcard src/*.c) \
		$(wildcard src/*.cpp) \
		$(wildcard src/*.S) \
        ${STANDALONE}/common/start.S

include ${STANDALONE}/common/bsp.mk
include ${STANDALONE}/common/riscv64-unknown-elf.mk
include ${STANDALONE}/common/standalone.mk

LDSCRIPT = ${BSP_PATH}/linker/default.ld
