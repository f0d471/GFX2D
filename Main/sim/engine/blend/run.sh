#!/usr/bin/env bash
# blend 正常回归：两项数学检查，完整穷举、金标准与停顿。
# --case 只运行指定用例；narrow_* 是外部变异的普通发现者，不加入默认回归。
set -uo pipefail
cd "$(dirname "$0")"

CASE_ONLY=""
if [ "$#" -ne 0 ]; then
    if [ "$#" -ne 2 ] || [ "$1" != --case ]; then
        echo 'usage: bash run.sh [--case spec|exhaustive|golden|stall|narrow_copy|narrow_trunc|narrow_unreg|narrow_const]'
        exit 2
    fi
    CASE_ONLY=$2
fi
case "$CASE_ONLY" in
    ''|spec|exhaustive|golden|stall|narrow_copy|narrow_trunc|narrow_unreg|narrow_const) ;;
    *) echo "RESULT: CASE-NOT-FOUND ($CASE_ONLY)"; exit 3 ;;
esac

RTL=../../../rtl
CHK=../../../tools/check
SRC=("$RTL/engine/core/engine_blend.v" tb_engine_blend.v)
BUILD=build
mkdir -p "$BUILD"

spec_check() {
    local name=$1
    if ! gcc -O2 -Wall -Wextra -o "$BUILD/$name" "$CHK/$name.c" >"$BUILD/$name.compile.log" 2>&1; then
        cat "$BUILD/$name.compile.log"
        echo "RESULT: COMPILE-ERROR ($name)"
        return 1
    fi
    if ! "$BUILD/$name" >"$BUILD/$name.log" 2>&1; then
        cat "$BUILD/$name.log"
        echo "RESULT: FAIL (spec $name)"
        return 1
    fi
    if ! grep -q '^总判定：全部通过$' "$BUILD/$name.log"; then
        cat "$BUILD/$name.log"
        echo "RESULT: MISSING (spec $name)"
        return 1
    fi
    echo "  $name: PASS"
}

# 只有默认回归/spec/golden 消费数学检查；变异的单例不重复准备无关 C 产物。
case "$CASE_ONLY" in
    ''|spec|golden)
        echo '规格侧：'
        spec_check check_round255 || exit 1
        if [ "$CASE_ONLY" != golden ]; then
            spec_check check_subst || exit 1
        fi
        ;;
esac
if [ "$CASE_ONLY" = spec ]; then exit 0; fi
if [ -z "$CASE_ONLY" ] || [ "$CASE_ONLY" = golden ]; then
    rm -f "$BUILD/golden.hex"
    if ! "$BUILD/check_round255" --golden "$BUILD/golden.hex" >"$BUILD/golden.export.log" 2>&1 || [ ! -s "$BUILD/golden.hex" ]; then
        cat "$BUILD/golden.export.log"
        echo 'RESULT: SETUP-ERROR (golden export)'
        exit 1
    fi
fi

bad=0
total=0
run_case() {
    local name=$1
    shift
    local status=0 result
    if ! iverilog -g2012 -I "$RTL/engine/include" -o "$BUILD/$name.vvp" "$@" "${SRC[@]}" >"$BUILD/$name.compile.log" 2>&1; then
        cat "$BUILD/$name.compile.log"
        echo "RESULT: COMPILE-ERROR ($name)"
        return 1
    fi
    vvp -n "$BUILD/$name.vvp" >"$BUILD/$name.log" 2>&1 || status=$?
    result=$(grep '^RESULT:' "$BUILD/$name.log") || result=''
    if [ "$status" -ne 0 ] || [ "$result" != 'RESULT: PASS' ]; then
        cat "$BUILD/$name.log"
        echo "  $name: FAIL (vvp=$status; exactly one RESULT: PASS required)"
        return 1
    fi
    echo "  $name: RESULT: PASS"
}

check() {
    local name=$1
    shift
    if [ -n "$CASE_ONLY" ] && [ "$name" != "$CASE_ONLY" ]; then return; fi
    total=$((total + 1))
    if ! run_case "$name" "$@"; then bad=$((bad + 1)); fi
}

echo 'RTL 用例：'
check exhaustive
check golden -DGOLDEN
check stall -DSTALL -DSTRIDE=5
if [ -n "$CASE_ONLY" ]; then
    check narrow_copy -DONLY_EQ=0 -DFAILFAST
    check narrow_trunc -DONLY_EQ=2 -DFAILFAST
    # 步长 17 包含 0 和 255，三个有效操作数共 16^3=4096 组；完整域仍由 exhaustive 覆盖。
    check narrow_unreg -DONLY_EQ=1 -DSTRIDE=17 -DFAILFAST
    check narrow_const -DONLY_EQ=3 -DSTRIDE=17 -DFAILFAST
fi

if [ "$bad" -eq 0 ]; then
    echo "SIM OK: $total/$total cases as expected"
else
    echo "SIM BROKEN: $bad case(s) not as expected"
    exit 1
fi