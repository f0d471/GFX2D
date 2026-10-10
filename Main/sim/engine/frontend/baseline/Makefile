APP = frontendBench
APP_SRCDIR = ../../..
GAME = apps/game/gfx2dGame
APP_SRCS = $(wildcard $(APP_SRCDIR)/apps/probe/frontendBench/src/*.c)
APP_SRCS := $(patsubst $(APP_SRCDIR)/%,%,$(APP_SRCS))
APP_SRCS += $(patsubst $(APP_SRCDIR)/%,%,$(filter-out $(APP_SRCDIR)/$(GAME)/src/main.c,$(wildcard $(APP_SRCDIR)/$(GAME)/src/*.c $(APP_SRCDIR)/$(GAME)/src/game/*.c)))
LIBS = core gfx
REPLAYS = sw6_a sw6_b stg_clear
REPLAY_DIR = apps/game/gfx2dGame/replay
EXTRA_CFLAGS += -I$(abspath ../../game/gfx2dGame/src)
PLATFORM ?= target
include ../../../mk/$(PLATFORM).mk
