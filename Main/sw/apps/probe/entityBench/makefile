# entityBench —— E8 的板上专项探针（CPU 每弹幕开销与缓存大小）
#
# 走 SW1 的分层构建：被测的更新循环在 lib/core/entity.c，
# 板上与主机侧（Main/tools/check/e8_entity_golden.c）编的是同一份源码。
#
#   make                      板上，产物 build/entityBench-O2.bin
#   make OPT=s                可选对照档
#
# 判据与口径见 Engineering/TechReports/platform/10-CPU每弹幕开销与缓存对照的复现.md 第六节。

APP      = entityBench
APP_SRCS = $(wildcard src/*.c)
LIBS     = core

PLATFORM ?= target
include ../../../mk/$(PLATFORM).mk
