#!/usr/bin/env bash
# 单通道混合核（rtl/engine/core/engine_blend.v）仿真
#
# 用例：
#   spec      先用 check_round255.c 自己的断言过一次（六个方程、舍入与位宽的规格侧实现）
#   exhaustive 六个方程全定义域穷举，50462976 个向量，与测试台现算的精确商逐值比；
#             方程用不到的输入跟着循环变量变，输出必须与它们无关
#   golden    抽样 1024 条与 check_round255.c 导出的金标准对一次
#   stall     随机插入 en 拉低的停顿拍（步长 5 抽样），停顿期间输入是乱值
#   narrow_*  压到单个方程的快速用例，供变异测试快速判红
#
# 全量穷举一次约 6 分钟（WSL、iverilog 12）；开发期要快跑用 ONLY_EQ 或 STRIDE，
# 判据必须跑默认值。只有明文 RTL，不需要 EFINITY_HOME。
# 故意改错的变异体不在 RTL 里，见 Main/sim/mutation/mutants.yaml 与 run_mutation.py。
set -u
cd "$(dirname "$0")"

# 单例模式：--case <用例名> 只构建并运行这一个用例、只打印结果，判定交给
# sim/mutation 的跑器（它还要比对见红位置）。不给参数时行为与原来完全一致。
CASE_ONLY=""
if [ "${1:-}" = "--case" ]; then CASE_ONLY="${2:-}"; shift 2; fi
matched=0

RTL=../../../rtl
SPEC_C=../../../tools/check/check_round255.c
SRC="$RTL/engine/core/engine_blend.v tb_engine_blend.v"
BUILD=build
mkdir -p "$BUILD"
rm -f "$BUILD/golden.hex"      # 导出失败时不能拿上一次的金标准顶替

bad=0
total=0

run_case() {
    local name=$1
    shift
    if ! iverilog -g2012 -I "$RTL/engine/include" -o "$BUILD/$name.vvp" "$@" $SRC > "$BUILD/$name.compile.log" 2>&1; then
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
    if [ -n "$CASE_ONLY" ] && [ "$name" != "$CASE_ONLY" ]; then return 0; fi
    matched=1
    total=$((total + 1))
    local r
    r=$(run_case "$name" "$@")
    echo "  $name: $r"
    if [ -n "$CASE_ONLY" ]; then return 0; fi
    case "$r" in
        "RESULT: $want"*) ;;
        *) bad=$((bad + 1)) ;;
    esac
}

# ---- 0) 规格侧：check_round255.c 自己的断言，再导出抽样金标准 ----
echo "规格侧 check_round255.c:"
if ! gcc -O2 -o "$BUILD/cr255" "$SPEC_C" > "$BUILD/cr255.compile.log" 2>&1; then
    echo "RESULT: COMPILE-ERROR (see $BUILD/cr255.compile.log)"
    bad=$((bad + 1))
elif ! "$BUILD/cr255" > "$BUILD/cr255.log" 2>&1; then
    echo "  check_round255: FAIL"
    tail -5 "$BUILD/cr255.log"
    bad=$((bad + 1))
else
    echo "  check_round255: $(grep -E '总判定' "$BUILD/cr255.log")"
    "$BUILD/cr255" --golden "$BUILD/golden.hex" > /dev/null 2>&1 || {
        echo "  golden 导出失败"; bad=$((bad + 1)); }
fi

# ---- 正常用例 ----
echo "正常用例:"
check exhaustive PASS
check golden     PASS -DGOLDEN
check stall      PASS -DSTALL -DSTRIDE=5

# ---- 收窄范围的快速用例 ----
# 全量穷举一次约 6 分钟；下面几条把范围压到单个方程，供变异测试的跑器快速判红用。
# 正常构建下它们同样必须 PASS（只是覆盖范围小，判据不变）。
echo "收窄范围的快速用例:"
check narrow_copy  PASS -DONLY_EQ=0 -DFAILFAST
check narrow_trunc PASS -DONLY_EQ=2 -DFAILFAST
check narrow_unreg PASS -DONLY_EQ=1 -DFAILFAST
check narrow_const PASS -DONLY_EQ=3 -DFAILFAST

if [ -n "$CASE_ONLY" ]; then
    [ "$matched" = 1 ] || { echo "RESULT: CASE-NOT-FOUND ($CASE_ONLY)"; exit 3; }
    exit 0
fi

if [ "$bad" -eq 0 ]; then
    echo "SIM OK: $total/$total cases as expected"
else
    echo "SIM BROKEN: $bad case(s) not as expected"
    exit 1
fi
