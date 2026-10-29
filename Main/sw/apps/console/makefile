# Shared product frontend. Only the scheduler port differs between targets.
# make                        build/console-O2-perf.bin (FreeRTOS)
# make PLATFORM=host          build_host/console-perf (desktop)
APP := console
LIBS := core gfx
GRAPHICS_RUNTIME := 1
APP_RUNTIME := 1
RTOS_CONFIG_DIR := ../../firmware/console
PLATFORM ?= target
PERF ?= 1
PIPELINE ?= 1
DIAGNOSTIC ?= 0
ifeq ($(DIAGNOSTIC):$(PIPELINE),1:1)
$(error Per-frame CRC requires PIPELINE=0; use hardware synchronous execution for diagnostics)
endif

GAME_SRC := ../game/gfx2dGame/src
# Integer geometry needs the same O3 policy on host and target; no fast-math.
APP_O3_SRCS := $(GAME_SRC)/r4_scene.c $(GAME_SRC)/r4_geometry.c ../../ports/myriad/r4_runtime.c
LIB_O3_SRCS := core/fixmath.c gfx/rast_setup.c
APP_SRCS := src/apps_registry.c ../../ports/myriad/console_content.c ../../ports/myriad/console_views.c src/shell.c ../../ports/myriad/app_game.c \
            ../../ports/myriad/app_r4.c src/app_about.c src/app_demo.c \
            ../../ports/myriad/stg_runtime.c \
            ../../ports/myriad/frontend_game.c \
            ../../ports/myriad/stg_modes.c $(GAME_SRC)/r4_cup.c \
            $(GAME_SRC)/r4_geometry.c ../../ports/myriad/r4_runtime.c $(GAME_SRC)/r4_scene.c \
            $(wildcard $(GAME_SRC)/game/*.c) ../../ports/myriad/sw6_input.c ../../ports/myriad/sw6_report.c
ifeq ($(PLATFORM),host)
APP_SRCS += ../../firmware/console/main_host.c ../../ports/host/app_port.c
else
USE_RTOS := 1
APP_SRCS += ../../firmware/console/main_rtos.c ../../ports/ti60/app_port.c
override EXTRA_CFLAGS += -DGFX2D_PRESS_CLOCK=1
endif
override APP_CPPFLAGS += -I$(GAME_SRC) -I../../ports/myriad -I../../rtos -Isrc -I../../firmware/console
override EXTRA_CFLAGS += -DGFX_RENDER_PIPELINE=$(PIPELINE) -DGFX_FRAME_DIAGNOSTIC=$(DIAGNOSTIC)
include ../../mk/$(PLATFORM).mk
