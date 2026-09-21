# platformTest —— PC 平台层（SW4）的判据程序
#
# 一次跑完全部场景，不加开关：十一个整帧用例、600 帧帧节拍、掉帧注错、
# 实时输入与关窗退出、录制的往返与两次回放。判据与取舍见
# Engineering/Todo/sw/SW4-PC平台层.md 第 3 节。
#
#   make PLATFORM=host                   PC，产物 build_host/platformTest
#   SDL_VIDEODRIVER=dummy ./build_host/platformTest    无窗口跑同一个二进制
#   make PLATFORM=host EXTRA_CFLAGS=-DREPLAY_FRAMES=180 少跑几帧，快速回归
#
# 这一段是 PC 侧的判据，串起来跑的是 Main/tools/check/check_sw4_pc.sh。
# 它同样能编出板上产物（分层判据要求所有应用两边都编得过），
# 但板上没有 SDL，帧节拍与退出那几条只有在 PC 上才有意义。

APP      = platformTest
APP_SRCS = $(wildcard src/*.c)
LIBS     = core gfx

PLATFORM ?= host
include ../../../mk/$(PLATFORM).mk
