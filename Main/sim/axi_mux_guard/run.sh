#!/usr/bin/env bash
# Axi_Mux（X3）+ axi_mux_guard + 三个主机的仿真（实验 E5 2.2 节）
# 正常用例必须 PASS；四个注错/对照用例按各自的期望判定，否则说明判据失效
#
# Axi_Mux 用到厂商原语 EFX_SRL8，需要 Efinity 自带的仿真模型：
#   EFINITY_HOME 指向 Efinity 安装目录（WSL 里的路径），见 doc/开发环境.md 第 1、11 节
set -u
cd "$(dirname "$0")"

if [ -z "${EFINITY_HOME:-}" ] || [ ! -f "$EFINITY_HOME/sim_models/verilog/efx_srl8.v" ]; then
    echo "RESULT: SETUP-ERROR (EFINITY_HOME 未设置或其中没有 sim_models/verilog/efx_srl8.v)"
    exit 1
fi

SRC="../../rtl/bus/axi_mux_guard.v ../../rtl/vendor/axi_mux/Axi_Mux.v \
     $EFINITY_HOME/sim_models/verilog/efx_srl8.v ../axi_probe/axi_ram_model.v tb_axi_mux_guard.v"
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

# 固定授权序列（直接例化地址仲裁器，复现 E5 1 节的两组数字）
check grant_sequence  PASS -DCASE_GRANT_SEQUENCE
# 全链路：保护开启
check normal          PASS -DCASE_CHAIN -DADDR_STALL_MAX_VAL=4 -DRD_QUEUE_VAL=4
# 全链路：保护关闭但引擎每方向一笔在途（措施一，实测 301 <= 414）
check inflight1       PASS -DCASE_CHAIN -DGUARD_OFF -DINFLIGHT1 -DADDR_STALL_MAX_VAL=4 -DRD_QUEUE_VAL=4
# guard 单元：AR、AW 同时超限且分时完成
check dual_direction  PASS -DCASE_DUAL_DIRECTION
# 全链路：保护关闭 + 激进引擎，同一阈值判据必须不通过（实测 889 > 700）
check inject_noguard  FAIL -DCASE_CHAIN -DGUARD_OFF -DADDR_STALL_MAX_VAL=4 -DRD_QUEUE_VAL=4
# 全链路：掩码把 S0 位一起清掉，扫描必须拿不到授权（判据报 FAIL 才对）
check inject_mask_s0  FAIL -DCASE_CHAIN -DINJECT_MASK_S0 -DADDR_STALL_MAX_VAL=4 -DRD_QUEUE_VAL=4
# 全链路：已授权后下游持续不 ready，guard 不得伪造抢占
check inject_midgrant PASS -DCASE_CHAIN -DHOLD_MIDGRANT -DADDR_STALL_MAX_VAL=4 -DRD_QUEUE_VAL=4
# 写方向：读、写由两个独立仲裁器处理，必须各验一轮（判据表「引擎替身读、写各一轮」）
# 2026-09-19 复核发现：激进写引擎会把 CPU 的写饿住，而且 guard 挡不住
# （20000 与 60000 周期都只完成 7 笔写，保护开/关的数字完全相同——掩码撤不掉已授权的排队 AW）。
# 所以写方向这样安排：两例记录激进写引擎的停顿（期望 FAIL），一例用措施一证明修复（期望 PASS）。
check wr_aggr_guard   FAIL -DCASE_CHAIN -DS1_WRITE -DADDR_STALL_MAX_VAL=4 -DRD_QUEUE_VAL=4
check wr_aggr_noguard FAIL -DCASE_CHAIN -DGUARD_OFF -DS1_WRITE -DADDR_STALL_MAX_VAL=4 -DRD_QUEUE_VAL=4
check wr_contract     PASS -DCASE_CHAIN -DGUARD_OFF -DS1_WRITE -DINFLIGHT1 -DADDR_STALL_MAX_VAL=4 -DRD_QUEUE_VAL=4

if [ "$bad" -eq 0 ]; then
    echo "SIM OK: 9/9 cases as expected"
else
    echo "SIM BROKEN: $bad case(s) not as expected"
    exit 1
fi
