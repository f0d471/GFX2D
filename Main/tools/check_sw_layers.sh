#!/usr/bin/env bash
# check_sw_layers.sh —— 提交前跑一次的结构判据
#
# 判据见 Engineering/Todo/sw/SW1-软件平台分层与双目标构建.md 2.5 节：
#   1  lib/ 在只有 -Isw/lib 的条件下编译通过；
#   2  注错：lib/ 里 include "board.h" 必须编不过；应用里 include <soc.h> 必须编不过；
#      另有一个对照组：合法的 board.h 与 lib 头文件必须编得过；
#   3  注错：两个不同目录放同名的 x.c，板上构建能正确链接两份符号；
#   4  nm 扫 lib/ 与应用的目标文件，没有软浮点符号；注错：临时放 float 运算必须被报出来；
#   5  git diff --quiet HEAD -- Main/sdk，非同步提交里 sdk 的生成物无改动
#      （放行 Main/sdk/README.md：它是登记改动清单的那个文件，随项目改）；
#   6  改了头文件必须触发重编**并重链**（-MMD 的 .d 依赖）；另有对照：没有改动时两者都不发生；
#   7  缓存行区间折算：bsp/include/dcache_line.h 是纯算术，在主机上跑算例。
#      它放在 bsp/include 而不是 board.c 里，就是为了让这一项能在主机上验证——
#      lib/ 不能看 bsp，bsp/ti60/board.c 又要 sdk 头文件、编不了主机。
#
# 第 2、3、4、6 项用的是**真实的板上构建**（xPack 交叉工具链 + sw/mk/target.mk，产物落临时目录），
# 不是另写一套 -I 去模拟。这一点是有代价换来：第一版第 2 项用主机 gcc 加手写的 -I 检查，
# 而真实构建里 sdk 的 include 路径当时是全局加的，应用照样能 include <soc.h>，
# 判据通过了但分层没有生效。见 TechReports sw 第 01 章 4.6。
#
# 用法（在 WSL 里，仓库根或任意位置）：
#     bash Main/tools/check_sw_layers.sh
#     RISCV_BIN=~/opt/xpack-riscv-none-embed-gcc-8.3.0-2.3/bin/riscv-none-embed- \
#         bash Main/tools/check_sw_layers.sh
#
# 本脚本只在仓库里临时放几个注错文件，退出时（含中途失败）一律删干净。
# 编译产物落在临时目录，不动 apps/game/gfx2dGame/build。
# 第 6 项会 touch 一次 bsp/include/memmap.h（只改时间戳，不改内容）。

set -u

MAIN="$(cd "$(dirname "$0")/.." && pwd)"
REPO="$(cd "$MAIN/.." && pwd)"
SW="$MAIN/sw"

HOST_CC="${HOST_CC:-gcc}"
RISCV_BIN="${RISCV_BIN:-riscv-none-embed-}"

WARN="-std=gnu11 -Wall -Wextra -Werror"

INJ_BOARD="$SW/lib/core/_inject_board_include.c"
INJ_SOC="$SW/apps/game/gfx2dGame/src/_inject_soc_include.c"
INJ_FLOAT="$SW/lib/core/_inject_float.c"
INJ_DUP_LIB="$SW/lib/core/x.c"
INJ_DUP_BSP="$SW/bsp/ti60/x.c"

TMP="$(mktemp -d)"
FAIL=0

cleanup() {
    rm -f "$INJ_BOARD" "$INJ_SOC" "$INJ_FLOAT" "$INJ_DUP_LIB" "$INJ_DUP_BSP"
    rm -rf "$TMP"
}
trap cleanup EXIT

ok()   { printf '%s ok\n'   "$1"; }
bad()  { printf '%s FAIL\n' "$1"; FAIL=$((FAIL + 1)); }
note() { printf '    %s\n'   "$1"; }

# 真实的板上构建：应用目录下的 makefile，临时 OBJDIR，日志进 $TMP/build.log
build_board() {
    make -C "$SW/apps/game/gfx2dGame" -s RISCV_BIN="$RISCV_BIN" OBJDIR="$1" >"$TMP/build.log" 2>&1
}

# 期望这次构建失败，并且失败原因就是那个注入文件（不能随便什么错都算数）
expect_build_fail() {   # $1 = OBJDIR，$2 = 注入文件名，$3 = 判据名
    if build_board "$1"; then
        bad "$3"
        note "构建居然成功了：越界没有被拦住"
    elif grep -q "$2" "$TMP/build.log"; then
        ok "$3"
    else
        bad "$3"
        note "构建失败了，但报错与注入文件 $2 无关，这一项不算通过："
        sed 's/^/    /' "$TMP/build.log" | head -5
    fi
}

# 工具链在不在，决定第 2、3、4 项能不能做
if ! command -v "${RISCV_BIN}gcc" >/dev/null 2>&1; then
    printf 'RISCV_BIN=%s 找不到交叉编译器，第 2、3、4 项要它。\n' "$RISCV_BIN"
    printf '用法见脚本头部，或 doc/开发环境.md 第 1、11 节。\n'
    exit 1
fi

# ---------------------------------------------------------------------------
# 1  lib/ 只带 -Isw/lib 编得过
#    （主机 gcc 就够：这一项要的是「lib 不依赖除自己以外的任何东西」）
# ---------------------------------------------------------------------------
n=0
item1=0
while IFS= read -r src; do
    n=$((n + 1))
    if ! "$HOST_CC" $WARN -I"$SW/lib" -c "$src" -o "$TMP/lib1.o" 2>"$TMP/err1.txt"; then
        item1=1
        note "编不过：${src#$SW/}"
        sed 's/^/    /' "$TMP/err1.txt" | head -5
    fi
done < <(find "$SW/lib" -name '*.c' | sort)

if [ "$item1" -eq 0 ]; then
    ok "1 lib/ 只有 -Ilib 编过（$n 个文件）"
else
    bad "1 lib/ 只有 -Ilib 编过"
fi

# ---------------------------------------------------------------------------
# 2  分层在真实构建里是编不过的，不是靠约定
# ---------------------------------------------------------------------------
printf '#include "board.h"\nint inject_board_include(void) { return 0; }\n' > "$INJ_BOARD"
expect_build_fail "$TMP/obj_2a" "_inject_board_include.c" "2a 注错 lib 里 include board.h 编不过"
rm -f "$INJ_BOARD"

printf '#include <soc.h>\nint inject_soc_include(void) { return (int)SYSTEM_CLINT_CTRL; }\n' > "$INJ_SOC"
expect_build_fail "$TMP/obj_2b" "_inject_soc_include.c" "2b 注错 应用里 include soc.h 编不过"
rm -f "$INJ_SOC"
# 同理的反面：包含目录里的 board.h 对应用是可见的，否则 2a/2b 的失败可能只是路径写错
printf '#include "board.h"\n#include "core/crc32.h"\nint inject_ok(void) { return (int)REGION_COUNT; }\n' > "$INJ_SOC"
if build_board "$TMP/obj_2c"; then
    ok "2c 对照 应用里 include board.h 与 lib 编得过"
else
    bad "2c 对照 应用里 include board.h 与 lib 编得过"
    sed 's/^/    /' "$TMP/build.log" | head -5
fi
rm -f "$INJ_SOC"

# ---------------------------------------------------------------------------
# 4  正常构建里没有软浮点符号；放了 float 必须被扫出来
# ---------------------------------------------------------------------------
SOFTFLOAT_RE='__(add|sub|mul|div|neg|eq|ne|lt|le|gt|ge)(sf|df)3|__(extendsfdf2|truncdfsf2|fixsfsi|fixdfsi|fixunssfsi|fixunsdfsi|floatsisf|floatsidf|floatunsisf|floatunsidf)'

OBJ_NORM="$TMP/obj_norm"
if build_board "$OBJ_NORM"; then
    if "${RISCV_BIN}nm" "$OBJ_NORM"/app/*/*.o "$OBJ_NORM"/lib/*/*.o "$OBJ_NORM"/bsp/*.o \
            2>/dev/null | grep -Eq "$SOFTFLOAT_RE"; then
        bad "4 正常构建里没有软浮点符号"
        note "扫到的："
        "${RISCV_BIN}nm" "$OBJ_NORM"/app/*/*.o "$OBJ_NORM"/lib/*/*.o "$OBJ_NORM"/bsp/*.o \
            2>/dev/null | grep -E "$SOFTFLOAT_RE" | sed 's/^/    /' | head -5
    else
        ok "4 正常构建里没有软浮点符号"
    fi
else
    bad "4 正常构建里没有软浮点符号（构建失败）"
    sed 's/^/    /' "$TMP/build.log" | tail -8
fi

cat > "$INJ_FLOAT" <<'EOF'
/* 注错用：板上没有 FPU，出现 float/double 就必须被报出来。 */
float _inject_float_mul(float a, float b) { return a * b; }
double _inject_double_add(double a, double b) { return a + b; }
EOF

OBJ_FPINJ="$TMP/obj_fpinj"
if build_board "$OBJ_FPINJ"; then
    hits="$("${RISCV_BIN}nm" "$OBJ_FPINJ"/lib/*/*.o 2>/dev/null | grep -E "$SOFTFLOAT_RE" | awk '{print $NF}' | sort -u | tr '\n' ' ')"
    if [ -n "$hits" ]; then
        ok "4i 注错 放了 float 之后被扫出来（$hits）"
    else
        bad "4i 注错 放了 float 之后被扫出来"
        note "没扫到软浮点符号，说明这一项是空转"
    fi
else
    bad "4i 注错 放了 float 之后被扫出来（构建失败）"
    sed 's/^/    /' "$TMP/build.log" | tail -8
fi
rm -f "$INJ_FLOAT"

# ---------------------------------------------------------------------------
# 3  两个目录下的同名 x.c 必须编成两个不同的 .o，两份符号都在
#    （官方的 standalone.mk 用 notdir，这一步会撞车，所以编译规则自己写）
# ---------------------------------------------------------------------------
cat > "$INJ_DUP_LIB" <<'EOF'
int inject_dup_from_lib(void) { return 1; }
EOF
cat > "$INJ_DUP_BSP" <<'EOF'
int inject_dup_from_bsp(void) { return 2; }
EOF

OBJ_DUP="$TMP/obj_dup"
if build_board "$OBJ_DUP"; then
    syms="$("${RISCV_BIN}nm" "$OBJ_DUP"/gfx2dGame-*.elf 2>/dev/null | grep -cE 'inject_dup_from_(lib|bsp)')"
    files="$(ls "$OBJ_DUP"/lib/core/x.o "$OBJ_DUP"/bsp/ti60/x.o 2>/dev/null | wc -l)"
    if [ "$syms" -ge 2 ] && [ "$files" -eq 2 ]; then
        ok "3 同名 x.c 两份符号都在（两份 .o、$syms 个符号）"
    else
        bad "3 同名 x.c 两份符号都在"
        note "两份 .o 找到 $files 个，ELF 里找到 $syms 个 inject_dup_from_* 符号"
    fi
else
    bad "3 同名 x.c 两份符号都在（构建失败）"
    sed 's/^/    /' "$TMP/build.log" | tail -8
fi
rm -f "$INJ_DUP_LIB" "$INJ_DUP_BSP"

# ---------------------------------------------------------------------------
# 6  改了头文件必须触发重编**并重链**
#    目标文件只依赖源文件时，改头文件不会重编，旧的目标文件被悄悄链进产物：
#    把 memmap.h 的 FB_HEIGHT 从 480 改成 240，bin 与改之前一模一样（见 TechReports sw 第 01 章 4.8）。
#    只查「重编了」不够：第一版修法把 -include $(DEPS) 放错位置，默认目标被 .d 抢走，
#    目标文件重编了却不链接，产物照样不更新。两项都要查。
# ---------------------------------------------------------------------------
OBJ_HDR="$TMP/obj_hdr"
if build_board "$OBJ_HDR"; then
    touch "$SW/bsp/include/memmap.h"        # 只改时间戳，不改内容
    if build_board "$OBJ_HDR" \
       && grep -qE 'CC .*main\.c' "$TMP/build.log" \
       && grep -qE '^LD ' "$TMP/build.log"; then
        ok "6 改头文件触发重编并重链（touch bsp/include/memmap.h 后 main.c 重编、elf 重链）"
    else
        bad "6 改头文件触发重编并重链"
        note "第二次构建的输出（要有 CC main.c 与 LD 两行）："
        sed 's/^/    /' "$TMP/build.log" | head -8
    fi
    # 对照：没有改动时既不该重编也不该重链，否则上面那一条可能是「每次全编」蒙对的
    if build_board "$OBJ_HDR" \
       && ! grep -qE 'CC .*main\.c' "$TMP/build.log" \
       && ! grep -qE '^LD ' "$TMP/build.log"; then
        ok "6b 对照 没有改动时不重编也不重链"
    else
        bad "6b 对照 没有改动时不重编也不重链"
        sed 's/^/    /' "$TMP/build.log" | head -8
    fi
else
    bad "6 改头文件触发重编并重链（构建失败）"
    sed 's/^/    /' "$TMP/build.log" | tail -8
fi

# ---------------------------------------------------------------------------
# 7  缓存行区间折算：bsp/include/dcache_line.h 是纯算术，在主机上跑算例。
#    报的缺陷是起始地址未对齐时漏掉末尾那一行（0x3F 起 2 字节要覆盖 0x00 与 0x40 两行）。
# ---------------------------------------------------------------------------
cat > "$TMP/span_probe.c" <<'EOF'
#include <stdio.h>
#include "dcache_line.h"

static int fails;

static void chk(unsigned addr, unsigned bytes, unsigned first, unsigned last)
{
    dcache_span s  = dcache_line_span((uint32_t)addr, (uint32_t)bytes, 64u);
    int         ok = (s.first == (uint32_t)first) && (s.last == (uint32_t)last);

    printf("span 0x%03x +%u -> 0x%03x..0x%03x %s\n", addr, bytes, s.first, s.last,
           ok ? "ok" : "FAIL");
    if (!ok)
        fails++;
}

int main(void)
{
    chk(0x3F, 2, 0x00, 0x40);       /* 报的算例：末尾那一行不能漏 */
    chk(0x3F, 1, 0x00, 0x00);
    chk(0x40, 1, 0x40, 0x40);
    chk(0x00, 1, 0x00, 0x00);
    chk(0x3F, 66, 0x00, 0x80);
    chk(0x1000, 64, 0x1000, 0x1000);
    chk(0x1001, 64, 0x1000, 0x1040);
    chk(0x1FFF, 2, 0x1FC0, 0x2000);
    printf("fail %d\n", fails);
    return fails ? 1 : 0;
}
EOF

if "$HOST_CC" -std=gnu11 -Wall -Wextra -Werror -I"$SW/bsp/include" \
        -o "$TMP/span_probe" "$TMP/span_probe.c" 2>"$TMP/span_build.txt" \
   && "$TMP/span_probe" > "$TMP/span_out.txt"; then
    ok "7 缓存行区间折算（8 个算例，含 0x3F +2 -> 0x00..0x40）"
else
    bad "7 缓存行区间折算"
    sed 's/^/    /' "$TMP/span_build.txt" "$TMP/span_out.txt" 2>/dev/null | head -14
fi

# ---------------------------------------------------------------------------
# 5  sdk/ 只读：本条及以后的非同步提交里 sdk 的生成物无改动
#    唯一放行的是 Main/sdk/README.md：它不是生成物，而是登记「重新生成后要恢复
#    哪些改动」的那个文件，本来就要随项目改。判据要拦的是生成物被动过。
# ---------------------------------------------------------------------------
if git -C "$REPO" diff --quiet HEAD -- Main/sdk ':(exclude)Main/sdk/README.md'; then
    ok "5 sdk/ 无改动（README 除外）"
else
    bad "5 sdk/ 无改动（README 除外）"
    git -C "$REPO" diff --stat HEAD -- Main/sdk ':(exclude)Main/sdk/README.md' | sed 's/^/    /' | head -5
fi

echo "fail $FAIL"
echo "done"
[ "$FAIL" -eq 0 ]
