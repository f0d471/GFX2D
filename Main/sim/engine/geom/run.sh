#!/usr/bin/env bash
# 几何层的仿真
#
# 两步：
#   1 研究探针 probe_geom.v（产出速率与单独综合，保留为回归）
#   2 入库实现 rtl/engine/core/engine_geom.v 的单元仿真
#
# 期望值一律由独立模型 tools/check/geom_model.py 现算，测试台只做「驱动 + 比对」：
#   三组输入：定向用例 34 条、159 条向量里几何层见到的命令、十一整帧用例的全部命令。
#   每条记录只有在握手真正发生的那一拍之后才前进到下一条（+STALL 会压出反压）。
#   测试台另有握手监视器（PROTO / DONEERR）与行缓冲替身（BUFERR），见 tb_engine_geom.v 文件头。
#
# 需要 WSL 里的 iverilog 12、python3 与主机 gcc（整帧用例的导出用）。
# 不需要 EFINITY_HOME：本目录不例化任何厂商原语（单独综合的工程在 syn/）。
set -u
cd "$(dirname "$0")"

RTL=../../../rtl
BUILD=build
mkdir -p "$BUILD"

bad=0
total=0

# ---------------------------------------------------------------- 0 生成期望值
echo "生成期望值（独立模型）："
if ! python3 ../../../tools/check/check_cmd_semantics.py \
        --emit "$BUILD/vectors.txt" --no-selftest > "$BUILD/emit.log" 2>&1; then
    echo "RESULT: SETUP-ERROR (check_cmd_semantics.py --emit 失败，见 $BUILD/emit.log)"
    exit 1
fi
python3 ../../../tools/check/geom_model.py --dump-directed "$BUILD/directed.hex" || exit 1
python3 ../../../tools/check/geom_model.py --dump-records "$BUILD/vectors.txt" \
        "$BUILD/vectors.hex" || exit 1
if ! bash ../../../tools/check/geom_cases_dump.sh "$BUILD/cases.txt" > "$BUILD/cases.log" 2>&1; then
    echo "RESULT: SETUP-ERROR (geom_cases_dump.sh 失败，见 $BUILD/cases.log)"
    exit 1
fi
python3 ../../../tools/check/geom_model.py --dump-records "$BUILD/cases.txt" \
        "$BUILD/cases.hex" || exit 1
echo ""

# ---------------------------------------------------------------- 1 研究探针
echo "研究探针 probe_geom（B3 前期研究）："
if ! iverilog -g2012 -o "$BUILD/probe.vvp" probe_geom.v tb_probe_geom.v \
        > "$BUILD/probe.compile.log" 2>&1; then
    echo "  RESULT: COMPILE-ERROR (see $BUILD/probe.compile.log)"
    bad=$((bad + 1))
else
    vvp -n "$BUILD/probe.vvp" > "$BUILD/probe.log" 2>&1
    r=$(grep -E '^RESULT:' "$BUILD/probe.log")
    echo "  probe: ${r:-RESULT: MISSING}"
    total=$((total + 1))
    case "$r" in "RESULT: PASS"*) ;; *) bad=$((bad + 1)) ;; esac
fi
echo ""

# ---------------------------------------------------------------- 2 单元仿真
# build_one <名字> [注错开关...]（开关是预处理器级宏，直接作用于被测 RTL）
build_one() {
    local name=$1
    shift
    iverilog -g2012 -I "$RTL/engine/include" -o "$BUILD/$name.vvp" "$@" \
        "$RTL/engine/core/engine_geom.v" tb_engine_geom.v > "$BUILD/$name.compile.log" 2>&1
}

# run_case <日志名> <vvp 名> <记录文件> <期望 RESULT> [plusargs]
run_case() {
    local name=$1 vvp=$2 rec=$3 want=$4 plus=${5:-}
    local r
    vvp -n "$BUILD/$vvp.vvp" +REC="$rec" $plus > "$BUILD/$name.log" 2>&1
    r=$(grep -E '^RESULT:' "$BUILD/$name.log")
    echo "  $name: ${r:-RESULT: MISSING (see $BUILD/$name.log)}"
    if [ -n "$plus" ]; then echo "      ($plus)"; fi
    total=$((total + 1))
    case "$r" in
        "RESULT: $want"*) ;;
        *) bad=$((bad + 1)) ;;
    esac
}

echo "单元仿真 engine_geom（第 03 章 3.7 节）："
build_one geom || { echo "  RESULT: COMPILE-ERROR (see $BUILD/geom.compile.log)"; exit 1; }
run_case geom_directed geom "$BUILD/directed.hex" PASS
run_case geom_vectors  geom "$BUILD/vectors.hex"  PASS
run_case geom_cases    geom "$BUILD/cases.hex"    PASS
echo ""

echo "握手用例（判据：序列与恒 ready 时逐条相同）："
run_case geom_stall     geom "$BUILD/directed.hex" PASS "+STALL"
run_case geom_stall_c   geom "$BUILD/cases.hex"    PASS "+STALL"
# +RFSTALL：行缓冲替身的排空拉长到 23 拍，下一行与下一条命令的第 0 行都要真的等。
# 159 条向量里单行、奇数行的命令最多，命令边界上同一半被连着用的情形都在这一组里。
run_case geom_rfstall   geom "$BUILD/directed.hex" PASS "+RFSTALL"
run_case geom_rfstall_v geom "$BUILD/vectors.hex"  PASS "+RFSTALL"
run_case geom_rfstall_c geom "$BUILD/cases.hex"    PASS "+RFSTALL"
echo ""

# 3 变异（外部活动）
# 原先的 inj_* 用例已删：同一组激励由 geom_directed / geom_rfstall 承担，
# 变异体在 Main/sim/mutation/mutants.yaml，由 run_mutation.py 在隔离树里施加。

# 反面对照：正常构建里不许出现任何违例标记（八份正常日志全查）
for c in geom_directed geom_vectors geom_cases geom_stall geom_stall_c \
         geom_rfstall geom_rfstall_v geom_rfstall_c; do
    if grep -qaE 'RULE1|MISMATCH|DONEERR|PROTO|BUFERR' "$BUILD/$c.log"; then
        echo "  $c: 正常构建里出现了违例"; bad=$((bad + 1))
    fi
    total=$((total + 1))
done
echo ""

if [ "$bad" -eq 0 ]; then
    echo "SIM OK: $total/$total checks as expected"
else
    echo "SIM BROKEN: $bad of $total check(s) not as expected"
    exit 1
fi
