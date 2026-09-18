# cacheMinTest —— E3 的前置最小实验（不含外部主机）
#
# 现象可复现用，不作为结论证据，原因见 src/main.c 文件头。
#
#     wsl -e bash -c 'cd sw/apps/probe/cacheMinTest && make BSP=efinix/EfxSapphireSoc \
#         RISCV_BIN=<RISCV_BIN> OBJDIR=build'
#
# 编译档：DEBUG=yes 加上 bsp 的 soc.mk 里 DEBUG_OG?=yes，实际是 -g3 -Og。

PROJ_NAME=cacheMinTest
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
