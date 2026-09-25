#!/usr/bin/env bash
# check_sw8_stats.sh —— 演示模式与统计埋点（SW8）A 批的提交前判据
#
# 判据见 Engineering/Todo/sw/SW8-演示模式与统计埋点.md 第 3 节的「A 批」几行，口径见 2.2、2.4 节与 4.4 节。
# 本脚本串起来跑：
#
#   M1 主机测试         mk/host.mk 的 test 目标：64 位（ASan/UBSan）与 -m32 输出逐字节相同、fail 0，
#                       stderr 里没有 sanitizer 报告；普查（C 组）19 行全 ok
#   M2 普查的注错       CENSUS_NO_SETCLIP / CENSUS_NO_SURF 各编一次，指定的那一行必须 FAIL
#
# 用法（WSL 或 Linux，仓库根或任意位置）：
#     bash Main/tools/check/check_sw8_stats.sh
#
# 需要 gcc、gcc-multilib（-m32）、libsdl2-dev（mk/host.mk 解析时就要 sdl2-config）与 python3。
# 全部通过时末尾是 fail 0 与 done，退出码 0。

set -u

MAIN="$(cd "$(dirname "$0")/../.." && pwd)"
SW="$MAIN/sw"

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

# 某一组的 ok 行数恰好是 $2、没有 FAIL 行。$1 = 组的前缀字母，$3 = 名字
group_all_ok() {
    local n_ok n_bad
    n_ok=$(grep -cE "^$1[0-9] .* ok\$" "$OUT" 2>/dev/null)
    n_bad=$(grep -cE "^$1[0-9] .* FAIL\$" "$OUT" 2>/dev/null)
    if [ "$n_ok" -eq "$2" ] && [ "$n_bad" -eq 0 ]; then
        ok "M1$1 $3 $2 行全 ok"
    else
        bad "M1$1 $3 应有 $2 行 ok，实际 ok $n_ok 行、FAIL $n_bad 行"
    fi
}

# 注错：编一次，指定的那一行必须 FAIL。$1 = 判据名，$2 = INJECT，$3 = 行首（grep -E）
expect_red() {
    run_libtest "i_$2" "$2"
    if grep -qE "$3.* FAIL$" "$TMP/i_$2/test/out64.txt" 2>/dev/null; then
        ok "$1 INJECT=$2 见红：$(grep -m1 -E "$3.* FAIL$" "$TMP/i_$2/test/out64.txt" | cut -c1-80)"
    else
        bad "$1 INJECT=$2 没有让「$3」见红"
    fi
}

# ---------------------------------------------------------------- M1 主机测试
echo "== M1 主机测试（64 位 ASan/UBSan 与 -m32） =="
if run_libtest t_norm ""; then rc=0; else rc=$?; fi
OUT="$TMP/t_norm/test/out64.txt"
if [ "$rc" -eq 0 ] && grep -q '^libTest 64/32 identical ok' "$TMP/t_norm.log" &&
   grep -q '^fail 0$' "$OUT" && grep -q '^done$' "$OUT"; then
    ok "M1 test 目标退出码 0、64/32 逐字节相同、fail 0、done（$(wc -l <"$OUT") 行）"
else
    bad "M1 test 目标退出码 $rc，或 64/32 不同，或没有 fail 0"
    tail -6 "$TMP/t_norm.log" | while read -r l; do note "$l"; done
fi
if grep -qE 'runtime error|AddressSanitizer|LeakSanitizer' "$TMP/t_norm.log"; then
    bad "M1s stderr 里有 sanitizer 报告"
    grep -m3 -E 'runtime error|AddressSanitizer|LeakSanitizer' "$TMP/t_norm.log" | while read -r l; do note "$l"; done
else
    ok "M1s stderr 里没有 sanitizer 报告"
fi
group_all_ok C 19 "普查"
grep -E '^C1 set-clip-state|^C2 ' "$OUT" | while read -r l; do note "$l"; done

# ---------------------------------------------------------------- M2 普查的注错
echo "== M2 普查的注错（每个都要让指定的行 FAIL） =="
expect_red M2 CENSUS_NO_SETCLIP '^C1 set-clip-state'
expect_red M2 CENSUS_NO_SURF    '^C2 300 sequences'

printf 'fail %d\ndone\n' "$FAIL"
[ "$FAIL" -eq 0 ]
