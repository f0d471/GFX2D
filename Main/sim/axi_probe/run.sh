#!/usr/bin/env bash
# axi_probe_master 与 Axi_Mux 仿真
# 正常配置必须 PASS；四个注错配置必须 FAIL，否则说明判据失效
#
# Axi_Mux 用到厂商原语 EFX_SRL8，需要 Efinity 自带的仿真模型：
#   EFINITY_HOME 指向 Efinity 安装目录（WSL 里的路径），见 doc/开发环境.md 第 1、11 节
set -u
cd "$(dirname "$0")"

if [ -z "${EFINITY_HOME:-}" ] || [ ! -f "$EFINITY_HOME/sim_models/verilog/efx_srl8.v" ]; then
    echo "RESULT: SETUP-ERROR (EFINITY_HOME 未设置或其中没有 sim_models/verilog/efx_srl8.v)"
    exit 1
fi

SRC="../../rtl/bus/axi_probe_master.v ../../rtl/vendor/axi_mux/Axi_Mux.v \
     $EFINITY_HOME/sim_models/verilog/efx_srl8.v axi_ram_model.v tb_axi_probe.v"
BUILD=build
mkdir -p "$BUILD"

run_case() {
    local name=$1
    shift
    if ! iverilog -g2012 -I ../../rtl/vendor/axi_mux -o "$BUILD/$name.vvp" "$@" $SRC > "$BUILD/$name.compile.log" 2>&1; then
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

check normal         PASS
check inject_flip    FAIL -DINJECT_FLIP
check inject_hang    FAIL -DINJECT_HANG
check inject_maxlen  FAIL -DINJECT_MAXLEN
check inject_timeout FAIL -DINJECT_TIMEOUT

if [ "$bad" -eq 0 ]; then
    echo "SIM OK: 5/5 cases as expected"
else
    echo "SIM BROKEN: $bad case(s) not as expected"
    exit 1
fi
