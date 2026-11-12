# Shared product frontend. Only the scheduler port differs between targets.
# make                        build/console-O2-perf.bin (FreeRTOS)
# make PLATFORM=host          build_host/console-perf (desktop)
APP := console
LIBS := core gfx
GRAPHICS_RUNTIME := 1
APP_RUNTIME := 1
RTOS_CONFIG_DIR := ../../firmware/console
PLATFORM ?= target
APP_PROFILE ?= full
ifeq ($(PLATFORM),host)
OBJDIR ?= build_host$(if $(filter full,$(APP_PROFILE)),,_$(APP_PROFILE))
else
OBJDIR ?= build$(if $(filter full,$(APP_PROFILE)),,_$(APP_PROFILE))
endif
CATALOG_TOOL := ../../../tools/board/console_catalog.py
APP_CATALOG ?= catalog.json
CATALOG_DIR := $(OBJDIR)/catalog
CATALOG_ARGS := --catalog '$(APP_CATALOG)' --profile $(APP_PROFILE) $(if $(filter command line environment override,$(origin APPS)),--apps '$(APPS)',) --out $(CATALOG_DIR)
ifneq ($(shell python3 $(CATALOG_TOOL) $(CATALOG_ARGS)),CATALOG_OK)
$(error Application catalog generation failed)
endif
include $(CATALOG_DIR)/selection.mk
PERF ?= 1
PIPELINE ?= 1
DIAGNOSTIC ?= 0
ifeq ($(DIAGNOSTIC):$(PIPELINE),1:1)
$(error Per-frame CRC requires PIPELINE=0; use hardware synchronous execution for diagnostics)
endif

GAME_SRC := ../game/gfx2dGame/src
# Integer geometry needs the same O3 policy on host and target; no fast-math.
APP_O3_SRCS := $(GAME_SRC)/r4_scene.c $(GAME_SRC)/r4_geometry.c $(GAME_SRC)/sh_scene.c ../../ports/myriad/r4_runtime.c
LIB_O3_SRCS := core/fixmath.c gfx/rast_setup.c
APP_SRCS := src/apps_registry.c ../../ports/myriad/console_content.c ../../ports/myriad/console_views.c src/shell.c src/app_about.c \
            ../../ports/myriad/stg_runtime.c ../../ports/myriad/console_present.c $(CONSOLE_APP_SRCS)
ifeq ($(PLATFORM),host)
APP_SRCS += ../../firmware/console/main_host.c ../../ports/host/app_port.c
else
USE_RTOS := 1
APP_SRCS += ../../firmware/console/main_rtos.c ../../ports/ti60/app_port.c
override EXTRA_CFLAGS += -DGFX2D_PRESS_CLOCK=1
endif
override APP_CPPFLAGS += -I$(GAME_SRC) -I../../ports/myriad -I../../rtos -Isrc -I../../firmware/console -I$(CATALOG_DIR) $(CONSOLE_FLAGS)
override EXTRA_CFLAGS += -DGFX_RENDER_PIPELINE=$(PIPELINE) -DGFX_FRAME_DIAGNOSTIC=$(DIAGNOSTIC) -DGFX_PRODUCT_HUD=1
include ../../mk/$(PLATFORM).mk
# The shared flags stamp may have cleared OBJDIR. Restore generated inputs before
# compilation; unchanged generated files retain their timestamps.
ifneq ($(shell python3 $(CATALOG_TOOL) $(CATALOG_ARGS)),CATALOG_OK)
$(error Application catalog regeneration failed)
endif
