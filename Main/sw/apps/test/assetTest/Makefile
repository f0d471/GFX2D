# assetTest —— SW5 素材流水线的判据程序
#
# 一次跑完 SW5 第 3 节里软件侧的判据：装载、解析、与 assets.h 对账、挑条目登记表面、
# 用软件执行器画一整帧、打印整帧 CRC 与一张 64×24 字符画。
# 判据与对账见 Engineering/Todo/sw/SW5-素材流水线.md 第 3 节；
# 串起来跑的是 Main/tools/check/check_sw5_assets.sh。
#
#   make                        PC（默认），产物 build_host/assetTest，开窗显示这一帧
#   make PLATFORM=target RISCV_BIN=<RISCV_BIN>  板上，产物 build/assetTest-O2.bin
#
# PC 侧的素材包路径由 mk/host.mk 的 ASSETS_PAK 编进 bsp/host，
# 可以用 GFX2D_ASSETS_PAK=<文件> 覆盖（判据脚本用它喂坏包）。

APP      = assetTest
APP_SRCS = $(wildcard src/*.c)
LIBS     = core gfx

PLATFORM ?= host
include ../../../mk/$(PLATFORM).mk
