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
  LIBS       := core gfx
  REPLAYS    := sw6_a sw6_b stg_clear
  APP_SRCS   := $(patsubst $(APP_SRCDIR)/%,%,$(wildcard $(APP_SRCDIR)/src/*.c $(APP_SRCDIR)/src/game/*.c))
endif

APP_SRCDIR ?= .
LIBS       ?= core
APP_SRCS   ?= $(wildcard src/*.c src/game/*.c)

# SW5：素材工具生成的 assets.h 与 assets.pak 落在 Main/assets/build/。
# 只有**应用**那一层的编译规则用 ASSETS_BUILD 这个 -I，lib/ 与 bsp/ 都看不到
# （lib/core/assetpak.h 只放通用格式，本项目的素材编号只在 assets.h 里）。
#
# ASSETS_PAK 会被 -D 编进 bsp/host，作为「没设 GFX2D_ASSETS_PAK 时读哪里」的默认值，
# 所以取绝对路径（abspath）：相对路径只在构建时的当前目录下成立，换一个目录跑就找不到包。
ASSETS_BUILD ?= $(abspath $(SW)../assets/build)
ASSETS_PAK   ?= $(ASSETS_BUILD)/assets.pak

OBJDIR   ?= build_host
HOST_CC  ?= gcc
OPT      ?= 2
BITS     ?= 64
SAN      ?= 0
INJECT   ?=
PERF     ?= 0
EXTRA_CFLAGS ?=

# SW6：录像（`.g2dr`）在构建时转成 C 数组。板上没有文件系统，SW4 定这个格式
# 就是为了「一步转成 C 数组」（replay.h 的文件头）。应用在 makefile 里写
# `REPLAYS = sw6_a sw6_b`，源文件放在 `<应用>/replay/<名字>.g2dr`（入库的源数据），
# 生成物落在 `$(OBJDIR)/replay/`、不入库，和 assets.h 一样。
REPLAY_DIR  ?= replay
REPLAYS     ?=
REPLAY2C    ?= $(SW)../tools/check/replay2c.py
REPLAY_HDRS := $(addprefix $(OBJDIR)/replay/,$(addsuffix .h,$(REPLAYS)))

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
ifeq ($(PERF),1)
BASE += -DSW9_PERF=1
NAME := $(NAME)-perf
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
# 标记里还要带上 ASSETS_PAK：它被 -D 编进 bsp/host/board.c，
# 换了素材包路径而标记不变的话，board.o 会留着上次那个路径。
FLAGS_STAMP := $(OBJDIR)/.cflags
STAMP_VALUE := $(BASE) $(ASSETS_PAK)
# 整删 OBJDIR（不能按子目录枚举：APP_SRCS 带 ../ 上跳路径时对象会落在
# build/game、build/common 等 app/ 之外，枚举删不干净，旧档会混进新档——
# 2026-10-28 在 console 上踩实，见 target.mk 同处注释）。
$(shell if [ "$$(cat $(FLAGS_STAMP) 2>/dev/null)" != "$(STAMP_VALUE)" ]; then rm -rf $(OBJDIR); mkdir -p $(OBJDIR); printf '%s' "$(STAMP_VALUE)" > $(FLAGS_STAMP); fi)

APP_OBJS := $(addprefix $(OBJDIR)/app/,$(APP_SRCS:.c=.o))
LIB_SRCS := $(foreach l,$(LIBS),$(wildcard $(SW)lib/$(l)/*.c))
LIB_OBJS := $(patsubst $(SW)lib/%.c,$(OBJDIR)/lib/%.o,$(LIB_SRCS))
BSP_SRCS := $(wildcard $(SW)bsp/*.c) $(wildcard $(SW)bsp/host/*.c)
BSP_OBJS := $(patsubst $(SW)bsp/%.c,$(OBJDIR)/bsp/%.o,$(BSP_SRCS))

TARGET    := $(OBJDIR)/$(NAME)

# 默认目标写死：下面的 -include $(DEPS) 会把 .d 文件里的目标读进来，
# 如果不显式指定，第一个被读到的目标就成了默认目标，
# `make` 只重编那个目标文件而不链接（见 TechReports sw 第 01 章 4.8）。
.DEFAULT_GOAL := all

.PHONY: all run test gen-golden clean

# lib/test 覆盖 lib/core 与 lib/gfx 两个库。
# VEC 指向 `python3 tools/check/check_cmd_semantics.py --emit <file>` 的产物：
# 那一组向量是生成物、不入库，所以不给 VEC 时这一组跳过，其余照跑。
TEST_LIBS   := core gfx
TEST_SRCS   := $(foreach l,$(TEST_LIBS),$(wildcard $(SW)lib/$(l)/*.c)) \
               $(wildcard $(SW)lib/test/*.c)
GOLDEN      := $(SW)lib/test/golden.txt
REPLAY_FIX  := $(SW)lib/test/replay_fixture.g2dr
REPLAY_HDR  := $(OBJDIR)/test/replay_fixture.h
VEC         ?=
TEST_ARGS   := --golden $(GOLDEN) --replay $(REPLAY_FIX) $(if $(VEC),--vec $(VEC),)
TEST_INJECT := $(if $(INJECT),-DINJECT_$(INJECT),)

all: $(TARGET)

$(TARGET): $(APP_OBJS) $(LIB_OBJS) $(BSP_OBJS)
	@echo "LD $(TARGET)"
	@$(HOST_CC) $(BASE) -o $@ $^ $(SDL_LIBS)

# 录像 -> C 数组（SW6）。应用目标文件粗粒度地依赖全部生成的录像头：
# 头是编译期 include 的，必须在编 .o 之前就位；REPLAYS 为空时这几行是空操作。
$(OBJDIR)/replay/%.h: $(APP_SRCDIR)/$(REPLAY_DIR)/%.g2dr
	@mkdir -p $(dir $@)
	@echo "REPLAY $(patsubst $(SW)%,%,$<)"
	@python3 $(REPLAY2C) --in $< --out $@ --name $*

$(APP_OBJS): $(REPLAY_HDRS)

$(OBJDIR)/app/%.o: $(APP_SRCDIR)/%.c
	@mkdir -p $(dir $@)
	@echo "CC $(patsubst $(SW)%,%,$<)"
	@$(HOST_CC) $(BASE) -I$(SW)lib -I$(SW)bsp/include -I$(ASSETS_BUILD) -I$(OBJDIR)/replay -c $< -o $@

$(OBJDIR)/lib/%.o: $(SW)lib/%.c
	@mkdir -p $(dir $@)
	@echo "CC $(patsubst $(SW)%,%,$<)"
	@$(HOST_CC) $(BASE) -I$(SW)lib -c $< -o $@

$(OBJDIR)/bsp/%.o: $(SW)bsp/%.c
	@mkdir -p $(dir $@)
	@echo "CC $(patsubst $(SW)%,%,$<)"
	@$(HOST_CC) $(BASE) -I$(SW)bsp/include $(SDL_CFLAGS) \
	    -DGFX2D_ASSETS_PAK_DEFAULT='"$(ASSETS_PAK)"' -c $< -o $@

run: $(TARGET)
	@$(TARGET)

# 主机侧单元测试：64 位与 -m32 各编一份，两边输出必须逐字节相同。
# -m32 需要 gcc-multilib；16 位的 ASan 运行库不一定装，所以 -m32 那份默认不挂
# sanitizer（SAN32=1 可以强行打开）。
#
# 链 -lm：数值基础那一组（lib/test/test_fixmath.c）拿双精度的 sin、atan2 当参考，
# 双精度只出现在测试里（sw 第 06 章 3.4 节）。
#
# 录像那一组要先跑一次转换脚本：`lib/test/replay_fixture.g2dr` 是入库的输入，
# 生成的 C 数组落在 $(OBJDIR)/test/ 里、不入库。脚本进判据路径而不是一次性生成器，
# 这样「C 数组与 .g2dr 解析出同一串按键」这条判据才真的在测转换本身。
test:
	@set -e; \
	mkdir -p $(OBJDIR)/test; \
	command -v python3 >/dev/null || { echo "python3 不可用：录像那一组要它生成 C 数组"; exit 1; }; \
	python3 $(REPLAY2C) --in $(REPLAY_FIX) --out $(REPLAY_HDR) --name replay_fixture; \
	echo "== lib/test 64-bit =="; \
	$(HOST_CC) -std=gnu11 -O$(OPT) -g $(WARN) -fsanitize=address,undefined \
	    $(TEST_INJECT) -I$(SW)lib -I$(OBJDIR)/test -o $(OBJDIR)/test/libTest64 $(TEST_SRCS) -lm; \
	$(OBJDIR)/test/libTest64 $(TEST_ARGS) > $(OBJDIR)/test/out64.txt; \
	echo "== lib/test 32-bit =="; \
	$(HOST_CC) -std=gnu11 -O$(OPT) -g $(WARN) -m32 \
	    $(TEST_INJECT) -I$(SW)lib -I$(OBJDIR)/test -o $(OBJDIR)/test/libTest32 $(TEST_SRCS) -lm; \
	$(OBJDIR)/test/libTest32 $(TEST_ARGS) > $(OBJDIR)/test/out32.txt; \
	echo "== 两份输出比对 =="; \
	if diff -u $(OBJDIR)/test/out64.txt $(OBJDIR)/test/out32.txt; then \
	    echo "libTest 64/32 identical ok"; \
	else \
	    echo "libTest 64/32 DIFFERENT FAIL"; exit 1; \
	fi; \
	tail -2 $(OBJDIR)/test/out64.txt

# 重新生成整帧用例的期望 CRC。只在用例本身或规格有意改动之后跑，
# 跑完要把 golden.txt 的变化当成结论看，不是当成噪声顺手提交。
gen-golden:
	@set -e; \
	mkdir -p $(OBJDIR)/test; \
	python3 $(REPLAY2C) --in $(REPLAY_FIX) --out $(REPLAY_HDR) --name replay_fixture; \
	$(HOST_CC) -std=gnu11 -O$(OPT) -g $(WARN) -fsanitize=address,undefined \
	    -I$(SW)lib -I$(OBJDIR)/test -o $(OBJDIR)/test/libTestGen $(TEST_SRCS) -lm; \
	$(OBJDIR)/test/libTestGen --golden $(GOLDEN) --replay $(REPLAY_FIX) --gen-golden | tail -3

clean:
	@rm -rf $(OBJDIR)

# gcc 写出来的头文件依赖读到最后：-MMD 让每个目标文件带一份同名 .d，
# 读进来之后改头文件就会重编。放在文件末尾是为了不动默认目标（见上面 .DEFAULT_GOAL）。
DEPS := $(APP_OBJS:.o=.d) $(LIB_OBJS:.o=.d) $(BSP_OBJS:.o=.d)
-include $(DEPS)
