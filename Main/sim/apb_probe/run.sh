#!/usr/bin/env bash
# apb_probe 仿真
# 正常配置必须 PASS；两个注错配置必须 FAIL，否则说明判据失效
set -u
cd "$(dirname "$0")"

RTL=../../rtl
SRC="$RTL/bus/apb_probe.v tb_apb_probe.v"
BUILD=build
mkdir -p "$BUILD"

run_case() {
    local name=$1
    shift
    if ! iverilog -g2012 -I "$RTL/engine" -o "$BUILD/$name.vvp" "$@" $SRC > "$BUILD/$name.compile.log" 2>&1; then
        echo "RESULT: COMPILE-ERROR (see $BUILD/$name.compile.log)"
        return
    fi
    vvp -n "$BUILD/$name.vvp" > "$BUILD/$name.log" 2>&1
    local r
    r=$(grep -E '^RESULT:' "$BUILD/$name.log")
    echo "${r:-RESULT: MISSING (see $BUILD/$name.log)}"
}

bad=0
check() {
    local name=$1 want=$2
    shift 2
    local r
    r=$(run_case "$name" "$@")
    echo "$name: $r"
    case "$r" in
        "RESULT: $want"*) ;;
        *) bad=$((bad + 1)) ;;
    esac
}

check normal            PASS
check inject_addr_trunc FAIL -DINJECT_ADDR_TRUNC
check inject_data_trunc FAIL -DINJECT_DATA_TRUNC

if [ "$bad" -eq 0 ]; then
    echo "SIM OK: 3/3 cases as expected"
else
    echo "SIM BROKEN: $bad case(s) not as expected"
    exit 1
fi
