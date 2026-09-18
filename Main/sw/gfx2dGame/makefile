# gfx2dGame —— 应用
#
# 应用只写应用：平台相关的都在 sw/bsp/，纯算法都在 sw/lib/。
# 两个目标共用这一份 makefile，靠 PLATFORM 选平台文件：
#
#   make                        板上，产物 build/gfx2dGame-O2.bin
#   make PLATFORM=host          PC，产物 build_host/gfx2dGame
#
# 板上的产物名带上优化级别，OPT 改了名字跟着改（gfx2dGame-Os.bin）；
# 打开注错开关再加一段（gfx2dGame-O2-inject.bin）。
# 也可以从 sw/ 目录直接编：make -f mk/host.mk、make -f mk/target.mk，规则一样。

APP      = gfx2dGame
APP_SRCS = $(wildcard src/*.c src/game/*.c)
LIBS     = core

# OPT=2|s|0        优化级别，写进产物名
# INJECT=<名字>    打开 lib/ 里对应的注错宏（-DINJECT_<名字>）
# EXTRA_CFLAGS=..  临时追加编译选项，例如 -DFRAMES=12000 做长跑
PLATFORM ?= target
include ../mk/$(PLATFORM).mk
