#!/usr/bin/env bash
# 搬运层 engine_mover 仿真
#
# 被测是 rtl/engine/core/engine_mover.v 整块，经仓内 Axi_Mux（S1）接补齐四样的从机。
# 用例分四组：协议与上下文、复位与排空、错误、超时；另有六个注错，每一个都要见红
# （第六个是后补的：正常写流不打 tlast）。
# 最后单独跑一次从机定时模式（TIMED=1）的回归，它不经过被测，只测 axi_ram_model。
#
# 正常用例全程不得置超时位（复核补的判据），测试台自己检查。
# Axi_Mux 用到厂商原语 EFX_SRL8，需要 Efinity 自带的仿真模型：
#   EFINITY_HOME 指向 Efinity 安装目录（WSL 里的路径），见 doc/开发环境.md 第 1、11 节
set -u
cd "$(dirname "$0")"

if [ -z "${EFINITY_HOME:-}" ] || [ ! -f "$EFINITY_HOME/sim_models/verilog/efx_srl8.v" ]; then
    echo "RESULT: SETUP-ERROR (EFINITY_HOME 未设置或其中没有 sim_models/verilog/efx_srl8.v)"
    exit 1
fi

RTL=../../../rtl
SRC="$RTL/engine/core/engine_mover.v $RTL/engine/buffer/engine_band.v \
     $RTL/vendor/verilog-axi/axi_dma_rd.v $RTL/vendor/verilog-axi/axi_dma_wr.v \
     $RTL/vendor/axi_mux/Axi_Mux.v $EFINITY_HOME/sim_models/verilog/efx_srl8.v \
     ../../axi_probe/axi_ram_model.v tb_engine_mover.v"
BUILD=build
mkdir -p "$BUILD"

bad=0
total=0

run_case() {
    local name=$1
    shift
    if ! iverilog -g2012 -I "$RTL/engine/include" -I "$RTL/vendor/axi_mux" -o "$BUILD/$name.vvp" "$@" $SRC \
            > "$BUILD/$name.compile.log" 2>&1; then
        echo "RESULT: COMPILE-ERROR (see $BUILD/$name.compile.log)"
        return
    fi
    vvp -n "$BUILD/$name.vvp" > "$BUILD/$name.log" 2>&1
    local r
    r=$(grep -E '^RESULT:' "$BUILD/$name.log")
    echo "${r:-RESULT: MISSING (see $BUILD/$name.log)}"
}

check() {
    local name=$1 want=$2
    shift 2
    local r
    r=$(run_case "$name" "$@")
    echo "  $name: $r"
    total=$((total + 1))
    case "$r" in
        "RESULT: $want"*) ;;
        *) bad=$((bad + 1)) ;;
    esac
    if [ -n "${STAT:-}" ]; then grep -E '^STAT' "$BUILD/$name.log" | sed 's/^/      /'; fi
}

echo "正常用例:"
check normal  PASS -DCASE_NORMAL
check drain   PASS -DCASE_DRAIN
check err     PASS -DCASE_ERR -DERR_REGION
check timeout PASS -DCASE_TIMEOUT -DTMO_LIMIT_VAL=64

# 变异（外部活动）
# 原先的 inject_* 用例已删：同一组激励由 normal / drain 承担，
# 变异体在 Main/sim/mutation/mutants.yaml。
if [ "$bad" -eq 0 ]; then
    echo "SIM OK: $total/$total checks as expected"
else
    echo "SIM BROKEN: $bad of $total check(s) not as expected"
    exit 1
fi
