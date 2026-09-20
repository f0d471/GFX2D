# cup3d —— 纯软件 3D 光栅器在扫描输出的帧缓冲上转一只茶杯
#
#   make                        板上，产物 build/cup3d-O2.bin
#   make EXTRA_CFLAGS=-DFRAMES=1200   跑长一点
#
# 用途、分辨率的取法与着色方式见 src/main.c 文件头。
# 第三方 small3dlib（CC0）在 sw/lib/s3l/，登记见 sw/README.md。

APP      = cup3d
APP_SRCS = $(wildcard src/*.c)
LIBS     = core

PLATFORM ?= target
include ../../../mk/$(PLATFORM).mk
