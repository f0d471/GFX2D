# console —— FreeRTOS 游戏机前端（SW10）
#
# 板上专用：host 侧不接 RTOS（菜单逻辑的宿主单测列为后续工作点）。
#   make                产物 build/console-O2-perf.bin
#
# USE_RTOS=1 打开 mk/target.mk 的 FreeRTOS 块（内核 + sw/rtos 集成层）。
# 素材复用同一份 assets.pak；游戏内容复用 gfx2dGame 的源文件
# （排除判据/演示专属的 sw6_scene_a/b 与 sw6_input）。共享启动层在
# apps/common/stg_runtime.c。

APP      = console
LIBS     = core gfx
USE_RTOS := 1
# 游戏帧的引擎流水（双命令表 + 引擎中断驱动）由 SW9_PERF 门控
# （renderer_pipeline_enabled，renderer.c）；不开它时游戏帧落回软件执行器，
# 板上 ~2.5fps，HOME 和弦的 8 帧窗口根本凑不满（2026-10-28 板上实测定位）。
# console 与裸机 -perf 档同语义：逐帧 CRC 对账不进帧循环，产物名带 -perf。
PERF     := 1

GAME_SRC := ../game/gfx2dGame/src
APP_SRCS := $(wildcard src/*.c) \
            ../common/stg_runtime.c \
            $(GAME_SRC)/frontend_game.c \
            $(GAME_SRC)/renderer.c \
            $(GAME_SRC)/stg_modes.c \
            $(GAME_SRC)/r4_cup.c \
            $(GAME_SRC)/r4_geometry.c \
            $(GAME_SRC)/r4_render.c \
            $(GAME_SRC)/r4_scene.c \
            $(filter-out $(GAME_SRC)/game/sw6_scene_a.c \
                         $(GAME_SRC)/game/sw6_scene_b.c \
                         $(GAME_SRC)/game/sw6_input.c, \
                         $(wildcard $(GAME_SRC)/game/*.c))

# stg_slots.h / stg_main.h 等 gfx2dGame 的头从这边引用；-Isrc 让内核头找到
# src/FreeRTOSConfig.h（EXTRA_CFLAGS 同时作用于应用与内核的编译规则）
# -I../../rtos：FreeRTOS 集成层的公共头（rtos_irq.h，不含 sdk 泄漏）
EXTRA_CFLAGS := -I$(GAME_SRC) -I../common -I../../rtos -Isrc

PLATFORM ?= target
include ../../mk/$(PLATFORM).mk
