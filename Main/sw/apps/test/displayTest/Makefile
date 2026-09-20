# displayTest —— 扫描输出接进软件层之后的板上判据
#
# 走 SW1 的分层构建（不是 sdk 的 standalone.mk），因为它要用 sw/bsp 的
# board_fb / board_present / board_wait_frame，本程序也是这三个接口的验证程序。
#
#   make                        板上，产物 build/displayTest-O2.bin
#
# 判据见 src/main.c 文件头与 ../../../../Engineering/Todo/hw/扫描输出.md 2.5 节。
# PATTERN 那一段要看屏幕，串口只给寄存器侧的数。

APP      = displayTest
APP_SRCS = $(wildcard src/*.c)
LIBS     = core

PLATFORM ?= target
include ../../../mk/$(PLATFORM).mk
