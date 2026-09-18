#!/usr/bin/env bash
# Axi_Mux（X3）+ axi_mux_guard + 三个主机的仿真（实验 E5 2.2 节）
# 每个用例按各自的期望（PASS / FAIL）判定，另有两组「保护开关结果逐项相同」的比对；
# 任何一项不符合都说明判据或机理变了，脚本报 SIM BROKEN 并以非零退出
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

# 全链路用例共用的从机条件：地址通道接近「随时可收」（E5 2.2 节）
CHAIN_Q="-DADDR_STALL_MAX_VAL=4 -DRD_QUEUE_VAL=4"

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
total=0
check() {
    local name=$1 want=$2
    shift 2
    local r
    r=$(run_case "$name" "$@")
    echo "$name: $r"
    total=$((total + 1))
    case "$r" in
        "RESULT: $want"*) ;;
        *) bad=$((bad + 1)) ;;
    esac
}

# 两个用例的扫描、引擎与 CPU 统计必须逐项相同（写方向保护开关的记录判据）
same() {
    local a=$1 b=$2 la lb
    la=$(grep -E '^CHAIN (扫描|引擎)' "$BUILD/$a.log")
    lb=$(grep -E '^CHAIN (扫描|引擎)' "$BUILD/$b.log")
    total=$((total + 1))
    if [ -n "$la" ] && [ "$la" = "$lb" ]; then
        echo "same($a, $b): 逐项相同"
    else
        echo "same($a, $b): 出现差异，保护在写方向的作用机理变了，先核对源码与测试台"
        bad=$((bad + 1))
    fi
}

# 固定授权序列（直接例化地址仲裁器，复现 E5 1 节的两组数字）
check grant_sequence  PASS -DCASE_GRANT_SEQUENCE
# guard 单元：AR、AW 同时超限且分时完成；同拍 HITS 加 2；未握手时 MAX_WAIT 已在涨
check dual_direction  PASS -DCASE_DUAL_DIRECTION

# 读方向
# 保护开启 + 激进引擎（实测 590 <= 700）
check normal          PASS -DCASE_CHAIN $CHAIN_Q
# 保护关闭但引擎每方向一笔在途（措施一，实测 301 <= 414）
check inflight1       PASS -DCASE_CHAIN -DGUARD_OFF -DINFLIGHT1 $CHAIN_Q
# 保护关闭 + 激进引擎，同一阈值判据必须不通过（实测 889 > 700）：保护在读方向起作用的对照
check inject_noguard  FAIL -DCASE_CHAIN -DGUARD_OFF $CHAIN_Q
# 掩码把 S0 位一起清掉，扫描必须拿不到授权（判据报 FAIL 才对）
check inject_mask_s0  FAIL -DCASE_CHAIN -DINJECT_MASK_S0 $CHAIN_Q
# 已授权后下游持续不 ready，guard 不得伪造抢占
check inject_midgrant PASS -DCASE_CHAIN -DHOLD_MIDGRANT $CHAIN_Q

# 写方向：读、写由两个独立仲裁器处理，必须各验一轮（判据表「引擎替身读、写各一轮」）。
# 2026-09-19 订正：之前记录的「激进写引擎把 CPU 的写停住、guard 挡不住」是测试台自己的错，
# 引擎替身的 WLAST 打了一拍，W 错位后写通路死锁（从机报了几十次 wlast mismatch 却没有判据检查）。
# 修正后写方向没有饿死；保护开关的结果逐项相同（保护触发时 S1 的授权几乎总是已经锁存），
# 所以写方向的 guard 只验接线与触发，写方向的等待由措施一约束。
# WR_QUEUE=1 是原从机（一次一笔写），=4 与读方向的 RD_QUEUE=4 条件对等。
check wr_aggr_guard      PASS -DCASE_CHAIN -DS1_WRITE $CHAIN_Q
check wr_aggr_noguard    PASS -DCASE_CHAIN -DGUARD_OFF -DS1_WRITE $CHAIN_Q
same  wr_aggr_guard wr_aggr_noguard
check wr_contract        PASS -DCASE_CHAIN -DGUARD_OFF -DS1_WRITE -DINFLIGHT1 $CHAIN_Q
check wr_q4_aggr_guard   PASS -DCASE_CHAIN -DS1_WRITE $CHAIN_Q -DWR_QUEUE_VAL=4
check wr_q4_aggr_noguard PASS -DCASE_CHAIN -DGUARD_OFF -DS1_WRITE $CHAIN_Q -DWR_QUEUE_VAL=4
same  wr_q4_aggr_guard wr_q4_aggr_noguard
check wr_q4_contract     PASS -DCASE_CHAIN -DGUARD_OFF -DS1_WRITE -DINFLIGHT1 $CHAIN_Q -DWR_QUEUE_VAL=4
# 注错：复原打一拍的 WLAST，从机的协议检查必须报出来（FAIL 且 wlast_err 非零）
check inject_wlast       FAIL -DCASE_CHAIN -DS1_WRITE -DINJECT_WLAST $CHAIN_Q
total=$((total + 1))
if grep -qE 'wlast_err=[1-9]' "$BUILD/inject_wlast.log"; then
    echo "inject_wlast: FAIL 由 wlast_err 引起，协议检查生效"
else
    echo "inject_wlast: FAIL 不是由 wlast_err 引起，协议检查没有生效"
    bad=$((bad + 1))
fi

if [ "$bad" -eq 0 ]; then
    echo "SIM OK: $total/$total checks as expected"
else
    echo "SIM BROKEN: $bad of $total check(s) not as expected"
    exit 1
fi
