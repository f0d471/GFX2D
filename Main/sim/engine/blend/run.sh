#!/usr/bin/env bash
# 单通道混合核（rtl/engine/engine_blend.v）仿真
#
# 用例：
#   spec      先用 check_round255.c 自己的断言过一次（六个方程、舍入与位宽的规格侧实现）
#   exhaustive 六个方程全定义域穷举，50462976 个向量，与测试台现算的精确商逐值比；
#             方程用不到的输入跟着循环变量变，输出必须与它们无关
#   golden    抽样 1024 条与 check_round255.c 导出的金标准对一次
#   stall     随机插入 en 拉低的停顿拍（步长 5 抽样），停顿期间输入是乱值
#   五个注错   各自要红在它对应的判据上
#
# 全量穷举一次约 6 分钟（WSL、iverilog 12）；开发期要快跑用 ONLY_EQ 或 STRIDE，
# 判据必须跑默认值。只有明文 RTL，不需要 EFINITY_HOME。
set -u
cd "$(dirname "$0")"

RTL=../../../rtl
SPEC_C=../../../tools/check/check_round255.c
SRC="$RTL/engine/engine_blend.v tb_engine_blend.v"
BUILD=build
mkdir -p "$BUILD"
rm -f "$BUILD/golden.hex"      # 导出失败时不能拿上一次的金标准顶替

bad=0

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

check() {
    local name=$1 want=$2
    shift 2
    local r
    r=$(run_case "$name" "$@")
    echo "  $name: $r"
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

# ---- 注错：每一个都要红在它对应的判据上 ----
# ONLY_EQ 把范围压到被注错的那个方程，FAILFAST 让它在第一处不符就收尾，所以这几个都很快。
# inject_copy_dst 只有「用不到的输入会变」时才红，inject_x_no_en 只有插了停顿拍才红，
# 它们证明 exhaustive 与 stall 真的在检查这两件事。
# 注错开关以 -DINJ_xxx 直接传给被测 RTL（预处理器级宏），不经过 parameter。
echo "注错用例（期望 FAIL）:"
check inject_wrong_eq FAIL -DINJ_WRONG_EQ -DONLY_EQ=3 -DFAILFAST
check inject_trunc16  FAIL -DINJ_TRUNC16  -DONLY_EQ=2 -DFAILFAST
check inject_unreg_c  FAIL -DINJ_UNREG_C  -DONLY_EQ=1 -DFAILFAST
check inject_copy_dst FAIL -DINJ_COPY_DST -DONLY_EQ=0 -DFAILFAST
check inject_x_no_en  FAIL -DINJ_X_NO_EN  -DSTALL -DSTRIDE=5 -DFAILFAST

if [ "$bad" -eq 0 ]; then
    echo "SIM OK: 9/9 cases as expected"
else
    echo "SIM BROKEN: $bad case(s) not as expected"
    exit 1
fi
