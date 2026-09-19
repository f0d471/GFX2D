# SW1-板上构建：xPack riscv-none-elf-gcc 13.4.0 + sdk 的 BSP
#
# 用法（两种都可以）：
#   1) 应用目录下：make                 （应用 makefile include 本文件）
#   2) 从 sw/ 直接编 SW1 的最小应用：make -f mk/target.mk
#
# 为什么不直接用官方的 standalone.mk：
#   - 它用 `OBJS := $(notdir $(OBJS))` 把不同目录下的同名 .c 编成同一个 .o，
#     多目录之后必然撞上，所以编译规则自己写，目标文件按源文件的相对路径放；
#   - 它按 soc.mk 的 DEBUG?=yes / DEBUG_OG?=yes 默认给 -Og，优化级别要由项目
#     显式指定并记在产物名里；
#   - 它不支持按层给头文件路径，而分层正是靠这个保证的（见文件下半部分）。
#   bsp.mk 与 riscv64-unknown-elf.mk 仍然复用：-march/-mabi、工具链前缀、
#   sdk 头文件路径这些都从那里来，不重写一遍。
#
# 分层不是靠约定，是靠这里的 -I：
#   编 lib/   只有 -Isw/lib        —— 看不到 board.h、看不到 sdk
#   编 bsp/   只有 -Isw/bsp/include —— 看不到 lib
#   编应用    只有 lib 与 bsp/include —— 想直接调 sdk 驱动会编不过
#
# 变量的含义见 Engineering/Todo/sw/SW1-软件平台分层与双目标构建.md 2.3 节。

MK_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
SW     := $(dir $(patsubst %/,%,$(MK_DIR)))

# 直接以 `make -f mk/target.mk` 调用时（应用 makefile 没有先定义 APP），
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

OPT      ?= 2
OBJDIR   ?= build
INJECT   ?=
EXTRA_CFLAGS ?=

# --- sdk（只读上游）-------------------------------------------------------
STANDALONE ?= $(SW)../sdk/software/standalone
BSP        ?= efinix/EfxSapphireSoc

# DEBUG=no 要在 soc.mk 之前定下：soc.mk 里是 DEBUG?=yes，之后还会再来一层
# DEBUG_OG?=yes，两者合起来就是 -Og。
DEBUG := no

# 必须在 bsp.mk 之前定下：bsp.mk 里是 `LDSCRIPT ?= .../default.ld`，
# 它先赋值就把这里盖掉了（第一次编译时踩到：map 里 __stack_size 还是 4K）。
LDSCRIPT ?= $(SW)bsp/ti60/linker/gfx2d.ld

include $(STANDALONE)/common/bsp.mk
include $(STANDALONE)/common/riscv64-unknown-elf.mk

CFLAGS := $(filter-out -Os -O0 -Og -O3 -g3,$(CFLAGS))
CFLAGS += -O$(OPT) -g3 -std=gnu11 -Wall -Wextra -Werror

# sdk 是上游代码：它的头文件里定义了用不到的 static 函数（uart.h、clint.h 各有好几个），
# 按系统头引入（-isystem），项目的「零告警」判据只管项目自己的代码。
#
# 注意这两行只把 sdk 的路径收进 SDK_INC，**不加进 CFLAGS**：只有 bsp/ 那一层的规则用它们。
# 第一版把它们加在全局 CFLAGS 上，结果是应用与 lib 的编译行里也有 sdk 的路径，
# 「越界就编不过」在真实构建里并不成立——一个 #include <soc.h> 的应用文件照样编过，
# 而检查脚本用的是主机 gcc 加手写的 -I，发现不了。见 TechReports sw 第 01 章 4.6。
CFLAGS   := $(filter-out -I$(BSP_PATH)/include -I$(BSP_PATH)/app,$(CFLAGS))
SDK_INC  := $(addprefix -isystem ,$(BSP_PATH)/include $(BSP_PATH)/app $(STANDALONE)/include $(STANDALONE)/driver)

ifneq ($(INJECT),)
CFLAGS += -DINJECT_$(INJECT)
endif

# 头文件依赖：-MMD 让 gcc 为每个目标文件写一份同名的 .d，下面 include 进来。
# 没有它时目标文件只依赖源文件，改了头文件不会触发重编，
# 旧的目标文件被悄悄链进产物——改 memmap.h 里的常量、bin 却一模一样（见 TechReports sw 第 01 章 4.8）。
CFLAGS += -MMD -MP
CFLAGS += $(EXTRA_CFLAGS)

# 换过编译选项（打开 INJECT、换 OPT、改链接脚本……）之后必须重编，不能只重链。
# 用标记文件记住上一轮的选项，不一致就把这一轮的目标文件清掉。
# 没有这一步时 `make INJECT=XS32_SHIFT` 会静默复用上一次的对象，
# 注错版与正常版逐位一样（第一次跑判据时踩到，见开发日志）。
FLAGS_STAMP := $(OBJDIR)/.cflags
$(shell mkdir -p $(OBJDIR); if [ "$$(cat $(FLAGS_STAMP) 2>/dev/null)" != "$(CFLAGS) $(SDK_INC) $(LDSCRIPT)" ]; then rm -rf $(OBJDIR)/app $(OBJDIR)/lib $(OBJDIR)/bsp $(OBJDIR)/sdk; printf '%s' "$(CFLAGS) $(SDK_INC) $(LDSCRIPT)" > $(FLAGS_STAMP); fi)

# 产物名带上优化级别与注错标记：build/gfx2dGame-O2.bin、gfx2dGame-Os.bin、
# gfx2dGame-O2-inject.bin。哪份是什么，从文件名就能看出来。
NAME := $(APP)-O$(OPT)
ifneq ($(INJECT),)
NAME := $(NAME)-inject
endif

# 链接参数照抄 sdk 的 standalone.mk，只把链接脚本换成项目自己那份。
LDFLAGS += -L$(STANDALONE)/common
LDFLAGS += -specs=nosys.specs -lgcc -nostartfiles -ffreestanding -lc -lm
LDFLAGS += -Wl,-Bstatic,-T,$(LDSCRIPT),-Map,$(OBJDIR)/$(NAME).map,--print-memory-usage,--no-warn-rwx-segment

APP_OBJS  := $(addprefix $(OBJDIR)/app/,$(APP_SRCS:.c=.o))
LIB_SRCS  := $(foreach l,$(LIBS),$(wildcard $(SW)lib/$(l)/*.c))
LIB_OBJS  := $(patsubst $(SW)lib/%.c,$(OBJDIR)/lib/%.o,$(LIB_SRCS))
BSP_SRCS  := $(wildcard $(SW)bsp/*.c) $(wildcard $(SW)bsp/ti60/*.c)
BSP_OBJS  := $(patsubst $(SW)bsp/%.c,$(OBJDIR)/bsp/%.o,$(BSP_SRCS))
START_OBJ := $(OBJDIR)/sdk/start.o

ELF := $(OBJDIR)/$(NAME).elf
BIN := $(OBJDIR)/$(NAME).bin

# 默认目标写死：下面的 -include $(DEPS) 会把 .d 文件里的目标读进来，
# 如果不显式指定，第一个被读到的目标就成了默认目标，
# `make` 只重编那个目标文件而不链接，产物更新不出来（见 TechReports sw 第 01 章 4.8）。
.DEFAULT_GOAL := all

.PHONY: all size clean

all: $(ELF) $(OBJDIR)/$(NAME).hex $(OBJDIR)/$(NAME).asm $(BIN)

$(ELF): $(APP_OBJS) $(LIB_OBJS) $(BSP_OBJS) $(START_OBJ)
	@echo "LD $@"
	@$(RISCV_CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# 目标文件保留源文件的相对路径：两个目录下的同名 x.c 是两份不同的 .o。
$(OBJDIR)/app/%.o: $(APP_SRCDIR)/%.c
	@mkdir -p $(dir $@)
	@echo "CC $(patsubst $(SW)%,%,$<)"
	@$(RISCV_CC) -c $(CFLAGS) -I$(SW)lib -I$(SW)bsp/include -o $@ $<

$(OBJDIR)/lib/%.o: $(SW)lib/%.c
	@mkdir -p $(dir $@)
	@echo "CC $(patsubst $(SW)%,%,$<)"
	@$(RISCV_CC) -c $(CFLAGS) -I$(SW)lib -o $@ $<

$(OBJDIR)/bsp/%.o: $(SW)bsp/%.c
	@mkdir -p $(dir $@)
	@echo "CC $(patsubst $(SW)%,%,$<)"
	@$(RISCV_CC) -c $(CFLAGS) $(SDK_INC) -I$(SW)bsp/include -o $@ $<

$(START_OBJ): $(STANDALONE)/common/start.S
	@mkdir -p $(dir $@)
	@echo "CC $(patsubst $(SW)%,%,$<)"
	@$(RISCV_CC) -c $(CFLAGS) -o $@ $<

$(OBJDIR)/$(NAME).hex: $(ELF)
	@$(RISCV_OBJCOPY) -O ihex $< $@

$(BIN): $(ELF)
	@$(RISCV_OBJCOPY) -O binary $< $@

$(OBJDIR)/$(NAME).asm: $(ELF)
	@$(RISCV_OBJDUMP) -S -d $< > $@

# 登记用：bin 字节数与 SHA256，写进 sw/README.md 与开发日志
size: $(BIN)
	@wc -c < $(BIN) | tr -d ' ' | sed 's/^/bin bytes /'
	@sha256sum $(BIN) | cut -c1-16 | sed 's/^/sha256-16 /'

clean:
	@rm -rf $(OBJDIR)

# gcc 写出来的头文件依赖读到最后：-MMD 让每个目标文件带一份同名 .d，
# 读进来之后改头文件就会重编。放在文件末尾是为了不动默认目标（见上面 .DEFAULT_GOAL）。
DEPS := $(APP_OBJS:.o=.d) $(LIB_OBJS:.o=.d) $(BSP_OBJS:.o=.d) $(START_OBJ:.o=.d)
-include $(DEPS)
