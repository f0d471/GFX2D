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
    if ! iverilog -g2012 -I "$RTL/engine" -I "$RTL/vendor/axi_mux" -o "$BUILD/$name.vvp" "$@" $SRC \
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

echo "注错用例（期望 FAIL）:"
# 去掉读门控：读核一 ready 就放下一个，读流上下文错位、总线侧违例
check inject_nogate_rd FAIL -DCASE_NORMAL -DINJ_NO_GATE_RD
# 带内掩码旁路：wstrb 与 X 字节直通，被标记的像素照写
check inject_mask      FAIL -DCASE_NORMAL -DINJ_MASK_BYPASS
# 排空旁路：排空期不帮忙收/补，两个方向都停在忙态
check inject_nodrain   FAIL -DCASE_DRAIN  -DINJ_DRAIN_BYPASS
# 去掉写门控：写核一 ready 就放下一个
check inject_nogate_wr FAIL -DCASE_NORMAL -DINJ_NO_GATE_WR
# 排空按电平重进：请求举着时 drain_done 反复打，退出那一拍收了新提交
check inject_drain_level FAIL -DCASE_DRAIN -DINJ_DRAIN_LEVEL
# 正常写流不打 tlast（应由搬运层按描述符拍数自己打）：写核进 DROP_DATA，吞掉下一笔的写流
check inject_no_tlast  FAIL -DCASE_NORMAL -DINJ_NO_TLAST

# 注错的 FAIL 必须红在它对应的判据上，不是随便什么错
if grep -qE 'RD 契约|在途契约违例|读流|像素读流|目标缓冲号' "$BUILD/inject_nogate_rd.log"; then
    echo "  inject_nogate_rd: FAIL 由读流上下文错位或契约违例引起"
else
    echo "  inject_nogate_rd: FAIL 不是由读侧判据引起"; bad=$((bad + 1))
fi
total=$((total + 1))

if grep -qE '写目的字' "$BUILD/inject_mask.log"; then
    echo "  inject_mask: FAIL 由写目的区比对引起"
else
    echo "  inject_mask: FAIL 不是由写掩码判据引起"; bad=$((bad + 1))
fi
total=$((total + 1))

if grep -qE '排空没有完成' "$BUILD/inject_nodrain.log"; then
    echo "  inject_nodrain: FAIL 由排空没有完成引起"
else
    echo "  inject_nodrain: FAIL 不是由排空判据引起"; bad=$((bad + 1))
fi
total=$((total + 1))

if grep -qE '上一笔写还没结束|在途契约违例' "$BUILD/inject_nogate_wr.log"; then
    echo "  inject_nogate_wr: FAIL 由写方向在途契约违例引起"
else
    echo "  inject_nogate_wr: FAIL 不是由写在途契约引起"; bad=$((bad + 1))
fi
total=$((total + 1))

if grep -qE '请求一直举着时 drain_done|排空请求还举着就接受' "$BUILD/inject_drain_level.log"; then
    echo "  inject_drain_level: FAIL 由「一次请求只完成一次」引起"
else
    echo "  inject_drain_level: FAIL 不是由排空请求判据引起"; bad=$((bad + 1))
fi
total=$((total + 1))

# 不打 tlast：第一笔写的拍数用完时流上没有 tlast，写核进 DROP_DATA 一直等，
# 后面的写描述符一个也收不进去。正常用例发 4 笔写，这里只有第一笔上了总线
if grep -qE '看门狗' "$BUILD/inject_no_tlast.log" && grep -qE '^STAT .* aw=1 ' "$BUILD/inject_no_tlast.log"; then
    echo "  inject_no_tlast: FAIL 由写核卡在 DROP_DATA（只有第一笔写上了总线）引起"
else
    echo "  inject_no_tlast: FAIL 不是由写流 tlast 引起"; bad=$((bad + 1))
fi
total=$((total + 1))

# 从机定时模式：不经过被测，单独编译
echo "从机定时模式:"
if iverilog -g2012 -o "$BUILD/slave_timed.vvp" ../../axi_probe/axi_ram_model.v tb_slave_timed.v \
        > "$BUILD/slave_timed.compile.log" 2>&1; then
    vvp -n "$BUILD/slave_timed.vvp" > "$BUILD/slave_timed.log" 2>&1
    r=$(grep -E '^RESULT:' "$BUILD/slave_timed.log")
else
    r="RESULT: COMPILE-ERROR (see $BUILD/slave_timed.compile.log)"
fi
echo "  slave_timed: ${r:-RESULT: MISSING}"
total=$((total + 1))
case "$r" in "RESULT: PASS"*) ;; *) bad=$((bad + 1)) ;; esac

# 从机自己报的 WLAST 对账
for c in normal drain err timeout; do
    if [ -f "$BUILD/$c.log" ] && grep -q 'wlast' "$BUILD/$c.log"; then
        echo "  $c: 从机报了 WLAST 不匹配"; bad=$((bad + 1))
    fi
done

if [ "$bad" -eq 0 ]; then
    echo "SIM OK: $total/$total checks as expected"
else
    echo "SIM BROKEN: $bad of $total check(s) not as expected"
    exit 1
fi
