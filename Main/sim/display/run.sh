#!/usr/bin/env bash
# 扫描输出（display）仿真
#   1) tb_fifo_semantics：display_fifo（厂商异步 FIFO 的封装）的水位与复位语义
#   2) tb_display：display_top 的取数、跨域、像素对齐、双缓冲切换与欠载自愈
# 正常用例必须 PASS；注错用例必须 FAIL，否则说明判据失效。
#
# 全部是明文 RTL，iverilog 直接能编，不需要 EFINITY_HOME。
# 整帧用例（image_full）按 640x480 跑三场，仿真时间长，用 FULL=1 打开：
#   FULL=1 bash run.sh
set -u
cd "$(dirname "$0")"

RTL=../../rtl
FIFO_RTL="$RTL/vendor/ddr3_controller/common/efx_fifo_v2.3/efx_fifo_wrapper.v"
DISPLAY_SRC="$RTL/display/display_top.v $RTL/display/display_fetch.v \
             $RTL/display/display_out.v $RTL/display/display_fifo.v \
             $RTL/hdmi/color_bar_rgb.v $FIFO_RTL \
             ../axi_probe/axi_ram_model.v tb_display.v"
FIFO_SRC="$RTL/display/display_fifo.v $FIFO_RTL tb_fifo_semantics.v"

BUILD=build
mkdir -p "$BUILD"
bad=0

run_one() {
    local name=$1 src=$2
    shift 2
    if ! iverilog -g2012 -o "$BUILD/$name.vvp" "$@" $src > "$BUILD/$name.compile.log" 2>&1; then
        echo "RESULT: COMPILE-ERROR (see $BUILD/$name.compile.log)"
        return
    fi
    vvp -n "$BUILD/$name.vvp" > "$BUILD/$name.log" 2>&1
    local r
    r=$(grep -E '^RESULT:' "$BUILD/$name.log")
    echo "${r:-RESULT: MISSING (see $BUILD/$name.log)}"
}

check_fifo() {
    local name=$1 want=$2
    shift 2
    local r
    r=$(run_one "$name" "$FIFO_SRC" "$@")
    echo "fifo/$name: $r"
    case "$r" in "RESULT: $want"*) ;; *) bad=$((bad + 1)) ;; esac
}

check_disp() {
    local name=$1 want=$2
    shift 2
    local r
    r=$(run_one "$name" "$DISPLAY_SRC" "$@")
    echo "display/$name: $r"
    case "$r" in "RESULT: $want"*) ;; *) bad=$((bad + 1)) ;; esac
}

# ---- FIFO 语义（D4 的开工前提）----
check_fifo semantics        PASS
check_fifo inject_no_reset  FAIL -DINJECT_NO_RESET

# ---- 正常用例 ----
check_disp image            PASS -DCASE_IMAGE
check_disp image_clk_plus   PASS -DCASE_IMAGE -DCLK_PLUS
check_disp image_clk_minus  PASS -DCASE_IMAGE -DCLK_MINUS
check_disp swap             PASS -DCASE_SWAP
check_disp inflight         PASS -DCASE_INFLIGHT
check_disp level            PASS -DCASE_LEVEL
check_disp starve           PASS -DCASE_STARVE
check_disp snapshot         PASS -DCASE_SNAPSHOT
check_disp corerst          PASS -DCASE_CORERST

# ---- 注错：每一个都要红在它对应的判据上 ----
check_disp inj_lag0         FAIL -DCASE_IMAGE -DINJECT_VAL=16
check_disp inj_order        FAIL -DCASE_IMAGE -DINJECT_VAL=64
check_disp inj_phase        FAIL -DCASE_IMAGE -DINJECT_VAL=32
# inj_no_drain 的靶子是「场起始排空在途那一笔」，它跟 starve 配对：
# 正常场的最后一行早就取完，场起始时取数侧是闲着的，跟 image 配对测不出任何东西。
check_disp inj_no_drain     FAIL -DCASE_STARVE -DINJECT_VAL=1
check_disp inj_no_clear     FAIL -DCASE_IMAGE -DINJECT_VAL=2
check_disp inj_inflight2    FAIL -DCASE_INFLIGHT -DINJECT_VAL=8
check_disp inj_swap_now     FAIL -DCASE_SWAP -DINJECT_VAL=4

# ---- 整帧用例：跑得慢，单独开关 ----
if [ "${FULL:-0}" = "1" ]; then
    check_disp image_full   PASS -DCASE_IMAGE -DFULL_FRAME
else
    echo "display/image_full: SKIPPED（要整帧跑就 FULL=1 bash run.sh）"
fi

if [ "$bad" -eq 0 ]; then
    echo "SIM OK"
else
    echo "SIM BROKEN: $bad case(s) not as expected"
    exit 1
fi
