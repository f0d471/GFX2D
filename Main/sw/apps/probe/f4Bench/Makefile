APP = f4Bench
APP_SRCDIR = ../../..
APP_SRCS := $(patsubst $(APP_SRCDIR)/%,%,$(wildcard $(APP_SRCDIR)/apps/probe/f4Bench/src/*.c))
LIBS = core gfx
PLATFORM ?= target
include ../../../mk/$(PLATFORM).mk
