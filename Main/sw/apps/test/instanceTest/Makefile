APP = instanceTest
APP_SRCDIR = ../../..
APP_SRCS := $(patsubst $(APP_SRCDIR)/%,%,$(wildcard $(APP_SRCDIR)/apps/test/instanceTest/src/*.c))
LIBS = core gfx
PLATFORM ?= target
include ../../../mk/$(PLATFORM).mk
