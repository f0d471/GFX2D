#!/usr/bin/env bash
# check_sw7_game.sh —— 游戏本体（SW7）A 批的提交前判据
#
# 判据见 Engineering/Todo/sw/SW7-游戏本体.md 第 3 节，口径见 2.1 节与 4.5 节。本脚本串起来跑：
#
#   G1 正弦表可重生成   gen_sin_tab.py 重打一份，与入库的 lib/core/sin_tab.c 逐字节相同；
#                       注错 --inject 改一项，比对必须见红
#   G2 主机测试         mk/host.mk 的 test 目标：64 位（ASan/UBSan）与 -m32 输出逐字节相同、fail 0，
#                       stderr 里没有 sanitizer 报告；数值基础（F 组）21 行全 ok
#   G3 数值基础的注错   FX_IDX_TRUNC / FX_VEL_Q4 / FX_ATAN_NOSCALE / FX_ATAN_UNIT 各编一次，
#                       指定的那一行必须 FAIL
#   G4 实体池           apps/game/gfx2dGame/test/ 与 src/game/stg_*.c 按 test 目标的选项直接编：
#                       64 位挂 ASan/UBSan、-m32 各一份，两份输出逐字节相同、fail 0、无 sanitizer 报告；
#                       只链 lib/ 与 stg_*.c，不链 bsp
#   G5 实体池的注错     STG_POOL_STALL / STG_POOL_SWAP 各编一次，指定的那一行必须 FAIL
#
# 用法（WSL 或 Linux，仓库根或任意位置）：
#     bash Main/tools/check/check_sw7_game.sh
#
# 需要 gcc、gcc-multilib（-m32）、libsdl2-dev（mk/host.mk 解析时就要 sdl2-config）与 python3。
# 不需要板子与交叉工具链；板上构建的零告警由 check_sw_layers.sh 与 gfx2dGame 的板上构建覆盖。
# 全部通过时末尾是 fail 0 与 done，退出码 0。

set -u

MAIN="$(cd "$(dirname "$0")/../.." && pwd)"
SW="$MAIN/sw"
GEN="$MAIN/tools/check/gen_sin_tab.py"
SIN_TAB="$SW/lib/core/sin_tab.c"
APP="$SW/apps/game/gfx2dGame"
HOST_CC="${HOST_CC:-gcc}"

TMP="$(mktemp -d)"
FAIL=0

cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT

ok()   { printf '%s ok\n'   "$1"; }
bad()  { printf '%s FAIL\n' "$1"; FAIL=$((FAIL + 1)); }
note() { printf '    %s\n'   "$1"; }

# 跑一次 mk/host.mk 的 test 目标。$1 = 产物目录名，$2 = INJECT（可空）。
# stdout 与 stderr 一起进日志：sanitizer 的报告打在 stderr，test 目标本身不看它。
run_libtest() {
    local dir="$TMP/$1"
    if [ -n "$2" ]; then
        make -C "$SW" -f mk/host.mk test OBJDIR="$dir" INJECT="$2" >"$dir.log" 2>&1
    else
        make -C "$SW" -f mk/host.mk test OBJDIR="$dir" >"$dir.log" 2>&1
    fi
}

# ---------------------------------------------------------------- G1 正弦表
echo "== G1 正弦表可重生成 =="
if python3 "$GEN" --out "$TMP/sin_tab.c" >"$TMP/gen.log" 2>&1; then
    if cmp -s "$TMP/sin_tab.c" "$SIN_TAB"; then
        ok "G1 重新生成的 sin_tab.c 与入库的逐字节相同（$(stat -c %s "$SIN_TAB") 字节）"
    else
        bad "G1 重新生成的 sin_tab.c 与入库的不同"
        diff "$TMP/sin_tab.c" "$SIN_TAB" | head -6 | while read -r l; do note "$l"; done
    fi
else
    bad "G1 gen_sin_tab.py 跑不动"
    head -5 "$TMP/gen.log" | while read -r l; do note "$l"; done
fi
if python3 "$GEN" --inject --out "$TMP/sin_tab_inj.c" >>"$TMP/gen.log" 2>&1 &&
   ! cmp -s "$TMP/sin_tab_inj.c" "$SIN_TAB"; then
    ok "G1i 注错 改一项之后比对见红"
else
    bad "G1i 注错 改一项之后比对见红（比对没发现改动，这条判据是空转）"
fi

# ---------------------------------------------------------------- G2 主机测试
echo "== G2 主机测试（64 位 ASan/UBSan 与 -m32） =="
if run_libtest t_norm ""; then rc=0; else rc=$?; fi
out="$TMP/t_norm/test/out64.txt"
if [ "$rc" -eq 0 ] && grep -q '^libTest 64/32 identical ok' "$TMP/t_norm.log" &&
   grep -q '^fail 0$' "$out" && grep -q '^done$' "$out"; then
    ok "G2 test 目标退出码 0、64/32 逐字节相同、fail 0、done（$(wc -l <"$out") 行）"
else
    bad "G2 test 目标退出码 $rc，或 64/32 不同，或没有 fail 0"
    tail -6 "$TMP/t_norm.log" | while read -r l; do note "$l"; done
fi
if grep -qE 'runtime error|AddressSanitizer|LeakSanitizer' "$TMP/t_norm.log"; then
    bad "G2s stderr 里有 sanitizer 报告"
    grep -m3 -E 'runtime error|AddressSanitizer|LeakSanitizer' "$TMP/t_norm.log" | while read -r l; do note "$l"; done
else
    ok "G2s stderr 里没有 sanitizer 报告"
fi
nf_ok=$(grep -cE '^F[0-9] .* ok$' "$out" 2>/dev/null)
nf_bad=$(grep -cE '^F[0-9] .* FAIL$' "$out" 2>/dev/null)
if [ "$nf_ok" -eq 21 ] && [ "$nf_bad" -eq 0 ]; then
    ok "G2f 数值基础 21 行全 ok"
    grep -E '^F(2 sin_tab|2 sin\^2|4 atan2 \[|5 atan2 Q|6 ring)' "$out" | while read -r l; do note "$l"; done
else
    bad "G2f 数值基础应有 21 行 ok，实际 ok $nf_ok 行、FAIL $nf_bad 行"
fi

# ---------------------------------------------------------------- G3 注错
echo "== G3 数值基础的注错（每个都要让指定的行 FAIL） =="
expect_red() {   # $1 = INJECT，$2 = 必须 FAIL 的行（grep -E，行首锚定）
    run_libtest "i_$1" "$1"
    if grep -qE "$2.* FAIL$" "$TMP/i_$1/test/out64.txt" 2>/dev/null; then
        ok "G3 INJECT=$1 见红：$(grep -m1 -E "$2.* FAIL$" "$TMP/i_$1/test/out64.txt")"
    else
        bad "G3 INJECT=$1 没有让「$2」见红"
    fi
}
expect_red FX_IDX_TRUNC    '^F3 index rounding'
expect_red FX_VEL_Q4       '^F6 ring 32 shots'
expect_red FX_ATAN_NOSCALE '^F5 atan2 Q\.8 random'
expect_red FX_ATAN_UNIT    '^F4 atan2 \[-512'

# ---------------------------------------------------------------- G4 实体池
echo "== G4 实体池（64 位 ASan/UBSan 与 -m32，不链 bsp） =="
# 选项从 mk/host.mk 里取，不在这里另写一份：判据与被测构建必须用同一组参数
# （TechReports sw 第 01 章 4.6 节）。OBJDIR 指到临时目录，解析 host.mk 时的标记文件不落进仓库。
FLAGS="$(make -C "$SW" -s --no-print-directory -f mk/host.mk OBJDIR="$TMP/flags" \
         --eval 'sw7_print_flags: ; @echo -std=gnu11 -O$(OPT) -g $(WARN)' sw7_print_flags 2>/dev/null)"
if [ -z "$FLAGS" ]; then
    bad "G4 取不到 mk/host.mk 的编译选项"
fi
note "选项：$FLAGS"

# $1 = 产物名，$2 = 64|32，$3 = 追加的 -D（可空）
build_stg() {
    local extra=""
    [ "$2" = "64" ] && extra="-fsanitize=address,undefined"
    [ "$2" = "32" ] && extra="-m32"
    # shellcheck disable=SC2086
    "$HOST_CC" $FLAGS $extra $3 -I"$SW/lib" -I"$APP/src" -o "$TMP/$1" \
        "$APP"/test/*.c "$APP"/src/game/stg_*.c "$SW"/lib/core/*.c "$SW"/lib/gfx/*.c -lm \
        >"$TMP/$1.build.log" 2>&1
}

if build_stg stg64 64 "" && build_stg stg32 32 ""; then
    "$TMP/stg64" >"$TMP/stg64.txt" 2>"$TMP/stg64.err"; rc64=$?
    "$TMP/stg32" >"$TMP/stg32.txt" 2>"$TMP/stg32.err"; rc32=$?
    if [ "$rc64" -eq 0 ] && [ "$rc32" -eq 0 ] && grep -q '^fail 0$' "$TMP/stg64.txt" &&
       grep -q '^done$' "$TMP/stg64.txt" && cmp -s "$TMP/stg64.txt" "$TMP/stg32.txt"; then
        ok "G4 实体池 64/32 逐字节相同、fail 0、done（$(grep -cE ' ok$' "$TMP/stg64.txt") 行 ok）"
        grep -E '^P2 totals' "$TMP/stg64.txt" | while read -r l; do note "$l"; done
    else
        bad "G4 实体池退出码 64:$rc64 32:$rc32，或没有 fail 0，或 64/32 输出不同"
        grep -E 'FAIL|^fail' "$TMP/stg64.txt" | head -5 | while read -r l; do note "$l"; done
    fi
    if grep -qE 'runtime error|AddressSanitizer|LeakSanitizer' "$TMP/stg64.err"; then
        bad "G4s 实体池有 sanitizer 报告"
        head -3 "$TMP/stg64.err" | while read -r l; do note "$l"; done
    else
        ok "G4s 实体池没有 sanitizer 报告"
    fi
else
    bad "G4 实体池编不过"
    tail -5 "$TMP"/stg*.build.log | while read -r l; do note "$l"; done
fi

# ---------------------------------------------------------------- G5 实体池的注错
echo "== G5 实体池的注错（每个都要让指定的行 FAIL） =="
expect_stg_red() {   # $1 = INJECT，$2 = 必须 FAIL 的行
    if build_stg "stg_$1" 64 "-DINJECT_$1"; then
        "$TMP/stg_$1" >"$TMP/stg_$1.txt" 2>&1
        if grep -qE "$2.* FAIL$" "$TMP/stg_$1.txt"; then
            ok "G5 INJECT=$1 见红：$(grep -m1 -E "$2.* FAIL$" "$TMP/stg_$1.txt")"
        else
            bad "G5 INJECT=$1 没有让「$2」见红"
        fi
    else
        bad "G5 INJECT=$1 编不过"
    fi
}
expect_stg_red STG_POOL_STALL '^P2 300 rounds: removed == oob'
expect_stg_red STG_POOL_SWAP  '^P3 300 rounds: survivors keep'

printf 'fail %d\ndone\n' "$FAIL"
[ "$FAIL" -eq 0 ]
