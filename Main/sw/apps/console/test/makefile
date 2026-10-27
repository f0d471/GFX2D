# test_chord —— HOME 和弦逻辑的宿主单测（OSAL，SW14）
#
# 纯逻辑单测：只编 src/chord.c 与 src/chord_test.c，不上板、不开 RTOS。
#   make            产物 build_host/test_chord，末行 fail N（0 = 全过）
#
# 源放在 src/（单一 APP_SRCDIR 才能套 mk 的模式规则）；console 本体的
# makefile 已把 chord_test.c 从其 wildcard 里排除。

APP      = test_chord
LIBS     =
PLATFORM ?= host
APP_SRCDIR ?= ../src
APP_SRCS ?= chord.c chord_test.c
include ../../../mk/$(PLATFORM).mk
