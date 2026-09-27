# execSwTest —— E7 的板上专项探针（软件执行器像素吞吐）
#
# 走 SW1 的分层构建：要链接 lib/gfx 的执行器与构造层，还要 bsp 的 board_fb /
# board_present / board_cycles。
#
#   make                                   板上生产基线，产物 build/execSwTest-O2.bin
#   make OPT=s                             可选对照档（测了才报告）
#
# 初始六方程注错的原始 CRC 已留档；2026-09-27 收尾只上板 ALPHA 与错配自检。
# 修改具体方程时才按影响补验，不因普通软件更新重做全部注错。
# 单个方程与错配自检的构建方式：
#
#   make OBJDIR=build_inj_copy INJECT=EQ_COPY \
#        EXTRA_CFLAGS="-DE7_VERIFY_ONLY -DE7_INJECT_EQ=0"
#   ... EQ_ALPHA=1 / EQ_ADD=2 / EQ_CONST=3 / EQ_MUL=4 / EQ_ALPHA_STRAIGHT=5
#   make OBJDIR=build_inj_self INJECT=EQ_ALPHA \
#        EXTRA_CFLAGS="-DE7_VERIFY_ONLY -DE7_INJECT_EQ=0"     # 注错自检：按 COPY 判、打开 ALPHA 注错
#
# `E7_INJECT_EQ` 的取值是 `gfx_abi.h` 里的 `GFX_EQ_*`；`INJECT=` 是 lib/gfx 的宏名。
# 两者必须指向同一个方程，否则全部用例都判「不该变却变了」。
# 注错构建只跑 VERIFY 段（`-DE7_VERIFY_ONLY`），性能数字不引用它。
#
# 判据与口径见 Engineering/TechReports/platform/09-软件执行器像素吞吐的板上复现.md 第六节；
# 旧注错 CRC 重判见 Main/tools/check/e7_injection_check.py；
# 整帧 CRC 的独立复算见 tools/check/e7_output_check.py。

APP      = execSwTest
APP_SRCS = $(wildcard src/*.c)
LIBS     = core gfx

PLATFORM ?= target
include ../../../mk/$(PLATFORM).mk
