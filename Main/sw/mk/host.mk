# SW1-PC 构建：WSL 的 gcc + SDL2
#
# 用法（两种都可以）：
#   1) 直接在本目录下编 SW1 的最小应用：
#        cd Main/sw && make -f mk/host.mk
#   2) 应用的 makefile 里只写几行，然后 include 本文件：
#        APP = gfx2dGame
#        APP_SRCS = $(wildcard src/*.c src/game/*.c)
#        LIBS = core
#        include ../../../mk/host.mk
#
# 分层不是靠约定，是靠这里的 -I：
#   编 lib/   只有 -Isw/lib        —— 看不到 board.h、看不到 sdk
#   编 bsp/   只有 -Isw/bsp/include —— 看不到 lib，也没必要
#   编应用    只有 lib 与 bsp/include —— 想直接调 sdk 驱动会编不过
#
# 平台上无关的纯算法在 lib/，平台实现在 bsp/{ti60,host}/，
# 应用只把两者接起来：依赖方向见 SW1 spec 2.1 节。

MK_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
SW     := $(dir $(patsubst %/,%,$(MK_DIR)))

# 直接以 `make -f mk/host.mk` 调用时（应用 makefile 没有先定义 APP），
# 默认编 SW1 的最小应用；被应用 makefile include 时用调用方给的值。
# APP_SRCDIR 是「相对当前目录」的应用目录：被 include 时就是 .，
# 直接调用时是 ./apps/game/gfx2dGame；APP_SRCS 里的路径相对 APP_SRCDIR。
ifeq ($(APP),)
  APP        := gfx2dGame
  APP_SRCDIR := $(SW)apps/game/gfx2dGame
  LIBS       := core
  APP_SRCS   := $(patsubst $(APP_SRCDIR)/%,%,$(wildcard $(APP_SRCDIR)/src/*.c $(APP_SRCDIR)/src/game/*.c))
endif

APP_SRCDIR ?= .
LIBS       ?= core
APP_SRCS   ?= $(wildcard src/*.c src/game/*.c)

OBJDIR   ?= build_host
HOST_CC  ?= gcc
OPT      ?= 2
BITS     ?= 64
SAN      ?= 0
INJECT   ?=
EXTRA_CFLAGS ?=

# SDL2 的头文件当系统头处理（-isystem）：它们是第三方代码，
# 不该因为它的告警把 -Werror 的构建打断。
SDL_CFLAGS := $(patsubst -I%,-isystem %,$(shell sdl2-config --cflags 2>/dev/null))
SDL_LIBS   := $(shell sdl2-config --libs 2>/dev/null)

ifneq ($(strip $(SDL_LIBS)),)
else
$(error sdl2-config 不可用：PC 平台层要 SDL2，先装 libsdl2-dev)
endif

WARN := -Wall -Wextra -Werror -Werror=double-promotion -Werror=float-conversion
BASE := -std=gnu11 -O$(OPT) -g $(WARN)
ifeq ($(BITS),32)
BASE += -m32
endif
ifeq ($(SAN),1)
BASE += -fsanitize=address,undefined -fno-omit-frame-pointer
endif

# 与板上同一套注错开关，见 lib/core/xorshift32.c：
# `make INJECT=XS32_SHIFT` 把随机数的第一个移位常数改掉。
# 注错版产物名带 -inject，不会把正常版覆盖掉。
ifneq ($(INJECT),)
BASE += -DINJECT_$(INJECT)
NAME := $(APP)-inject
else
NAME := $(APP)
endif

# 头文件依赖：-MMD 让 gcc 为每个目标文件写一份同名的 .d，下面 include 进来。
# 没有它时目标文件只依赖源文件，改了头文件不会触发重编，
# 旧的目标文件被悄悄链进产物（见 TechReports sw 第 01 章 4.8）。
BASE += -MMD -MP
BASE += $(EXTRA_CFLAGS)

# 换过编译选项（打开 INJECT、换 OPT……）之后必须重编，不能只重链。
# 用标记文件记住上一轮的选项，不一致就把这一轮的目标文件清掉。
# 没有这一步时 `make INJECT=XS32_SHIFT` 会静默复用上一次的对象，
# 注错版与正常版逐位一样（第一次跑判据时踩到，见开发日志）。
FLAGS_STAMP := $(OBJDIR)/.cflags
$(shell mkdir -p $(OBJDIR); if [ "$$(cat $(FLAGS_STAMP) 2>/dev/null)" != "$(BASE)" ]; then rm -rf $(OBJDIR)/app $(OBJDIR)/lib $(OBJDIR)/bsp; printf '%s' "$(BASE)" > $(FLAGS_STAMP); fi)

APP_OBJS := $(addprefix $(OBJDIR)/app/,$(APP_SRCS:.c=.o))
LIB_SRCS := $(foreach l,$(LIBS),$(wildcard $(SW)lib/$(l)/*.c))
LIB_OBJS := $(patsubst $(SW)lib/%.c,$(OBJDIR)/lib/%.o,$(LIB_SRCS))
BSP_SRCS := $(wildcard $(SW)bsp/*.c) $(wildcard $(SW)bsp/host/*.c)
BSP_OBJS := $(patsubst $(SW)bsp/%.c,$(OBJDIR)/bsp/%.o,$(BSP_SRCS))

TARGET    := $(OBJDIR)/$(NAME)
TEST_SRCS := $(wildcard $(SW)lib/core/*.c) $(wildcard $(SW)lib/test/*.c)

# 默认目标写死：下面的 -include $(DEPS) 会把 .d 文件里的目标读进来，
# 如果不显式指定，第一个被读到的目标就成了默认目标，
# `make` 只重编那个目标文件而不链接（见 TechReports sw 第 01 章 4.8）。
.DEFAULT_GOAL := all

.PHONY: all run test clean

all: $(TARGET)

$(TARGET): $(APP_OBJS) $(LIB_OBJS) $(BSP_OBJS)
	@echo "LD $(TARGET)"
	@$(HOST_CC) $(BASE) -o $@ $^ $(SDL_LIBS)

$(OBJDIR)/app/%.o: $(APP_SRCDIR)/%.c
	@mkdir -p $(dir $@)
	@echo "CC $(patsubst $(SW)%,%,$<)"
	@$(HOST_CC) $(BASE) -I$(SW)lib -I$(SW)bsp/include -c $< -o $@

$(OBJDIR)/lib/%.o: $(SW)lib/%.c
	@mkdir -p $(dir $@)
	@echo "CC $(patsubst $(SW)%,%,$<)"
	@$(HOST_CC) $(BASE) -I$(SW)lib -c $< -o $@

$(OBJDIR)/bsp/%.o: $(SW)bsp/%.c
	@mkdir -p $(dir $@)
	@echo "CC $(patsubst $(SW)%,%,$<)"
	@$(HOST_CC) $(BASE) -I$(SW)bsp/include $(SDL_CFLAGS) -c $< -o $@

run: $(TARGET)
	@$(TARGET)

# 主机侧单元测试：64 位与 -m32 各编一份，两边输出必须逐字节相同。
# -m32 需要 gcc-multilib；16 位的 ASan 运行库不一定装，所以 -m32 那份默认不挂
# sanitizer（SAN32=1 可以强行打开）。
test:
	@set -e; \
	mkdir -p $(OBJDIR)/test; \
	echo "== lib/test 64-bit =="; \
	$(HOST_CC) -std=gnu11 -O$(OPT) -g $(WARN) -fsanitize=address,undefined \
	    -I$(SW)lib -o $(OBJDIR)/test/libTest64 $(TEST_SRCS); \
	$(OBJDIR)/test/libTest64 > $(OBJDIR)/test/out64.txt; \
	echo "== lib/test 32-bit =="; \
	$(HOST_CC) -std=gnu11 -O$(OPT) -g $(WARN) -m32 \
	    -I$(SW)lib -o $(OBJDIR)/test/libTest32 $(TEST_SRCS); \
	$(OBJDIR)/test/libTest32 > $(OBJDIR)/test/out32.txt; \
	echo "== 两份输出比对 =="; \
	if diff -u $(OBJDIR)/test/out64.txt $(OBJDIR)/test/out32.txt; then \
	    echo "libTest 64/32 identical ok"; \
	else \
	    echo "libTest 64/32 DIFFERENT FAIL"; exit 1; \
	fi; \
	tail -2 $(OBJDIR)/test/out64.txt

clean:
	@rm -rf $(OBJDIR)

# gcc 写出来的头文件依赖读到最后：-MMD 让每个目标文件带一份同名 .d，
# 读进来之后改头文件就会重编。放在文件末尾是为了不动默认目标（见上面 .DEFAULT_GOAL）。
DEPS := $(APP_OBJS:.o=.d) $(LIB_OBJS:.o=.d) $(BSP_OBJS:.o=.d)
-include $(DEPS)
