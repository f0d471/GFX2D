#!/usr/bin/env bash
# 入库 RTL rtl/engine/core/engine_pix.v 的正式判据
#
# 三组，期望值都不取自被测：
#   unit_*   tb_engine_pix.v，一行一行地跑：sweep 400 行（npx 1..25 × 源/目的相位 4×4）、
#            FILL 六方程、拆段四行、长行八行，逐槽对照 exec_sw.c 的像素公式；
#            写流下游恒 ready 与每 7 拍停 1 拍各一遍（停顿那一遍是反压、色键、掩码三类
#            缺陷的唯一发现者，变异体见 sim/mutation/mutants.yaml）。
#   pipe_*   tb_engine_pix_pipe.v，按边带契约允许的最快节奏发行（行与行之间不等，只等 row_free），
#            两半同时有货；命令 1~5 行、半号照几何层；写流下游恒 ready、周期停顿、成串停顿三种；
#            成串停顿那一遍负责发现「不随流水停」一类缺陷。
#   path_*   tb_engine_path.v，引擎全链真实通路（几何层、计算阵列、搬运层、两个 DMA 核、随机反压的
#            AXI RAM），命令与期望内存由 tools/check/pix_cases_dump.c 经软件执行器导出，
#            512 KiB 存储逐 128 位比对。另有跨命令写后读的一组：
#            等上一条写回完成再提交时对，几何层一空闲就提交（-DNOWAIT）时必须见红——
#            数据通路自己不挡这种冲突，控制层的命令下发要处理它。
#
# 故意改错的变异体不在 RTL 里，见 Main/sim/mutation/mutants.yaml 与 run_mutation.py。
# 需要 WSL 里的 iverilog 12 与 gcc；不需要 EFINITY_HOME（不经过 Axi_Mux）。
set -u
cd "$(dirname "$0")"
mkdir -p build
RTL=../../../rtl
bad=0
total=0

# 单例模式：--case <用例名> 只跑那一个用例，只打印 RESULT，由调用者判定。
CASE_ONLY=""
if [ "${1:-}" = "--case" ]; then CASE_ONLY="${2:-}"; shift 2; fi
matched=0

# run_case <名字> <顶层> <期望 PASS|FAIL> <源文件> [定义...]
run_case() {
    local name="$1" top="$2" want="$3" src="$4"
    shift 4
    if [ -n "$CASE_ONLY" ] && [ "$name" != "$CASE_ONLY" ]; then return 0; fi
    matched=1
    total=$((total+1))
    if ! iverilog -g2012 -I "$RTL/engine/include" -s "$top" "$@" \
        -o "build/$name.vvp" $src >"build/$name.compile.log" 2>&1; then
        echo "  $name: COMPILE FAIL (build/$name.compile.log)"; bad=$((bad+1)); return
    fi
    vvp -n "build/$name.vvp" >"build/$name.log" 2>&1
    local got stat
    got=$(grep '^RESULT:' "build/$name.log" | tail -1)
    stat=$(grep -E '^(pipe|PATH stats)' "build/$name.log" | tail -1)
    echo "  $name: ${got:-RESULT: MISSING}"
    [ -n "$stat" ] && [ "$want" = PASS ] && echo "      $stat"
    if [ -n "$CASE_ONLY" ]; then CASE_RESULT="$got"; return 0; fi
    case "$got" in
        "RESULT: $want"*) ;;
        *) bad=$((bad+1));;
    esac
}

UNIT="tb_engine_pix.v $RTL/engine/core/engine_pix.v $RTL/engine/core/engine_blend.v"
PIPE="tb_engine_pix_pipe.v $RTL/engine/core/engine_pix.v $RTL/engine/core/engine_blend.v"
PATH_SRC="tb_engine_path.v $RTL/engine/core/engine_geom.v $RTL/engine/core/engine_pix.v \
$RTL/engine/core/engine_blend.v $RTL/engine/core/engine_mover.v \
$RTL/vendor/verilog-axi/axi_dma_rd.v $RTL/vendor/verilog-axi/axi_dma_wr.v \
../../axi_probe/axi_ram_model.v"

echo 'B4 入库像素单元（一行一行）：'
run_case unit_ready     tb_engine_pix PASS "$UNIT"
run_case unit_stall     tb_engine_pix PASS "$UNIT" -DSTALL

echo 'B4 入库像素单元（乒乓与流水，行与行之间不等）：'
run_case pipe_ready     tb_engine_pix_pipe PASS "$PIPE" -DREADY_MODE=0
run_case pipe_stall     tb_engine_pix_pipe PASS "$PIPE" -DREADY_MODE=1
run_case pipe_burst     tb_engine_pix_pipe PASS "$PIPE" -DREADY_MODE=2

if ! gcc -O2 -std=c11 -Wall -Wextra -Werror -I../../../sw/lib \
    -o build/pix_cases_dump ../../../tools/check/pix_cases_dump.c \
    ../../../sw/lib/gfx/list.c ../../../sw/lib/gfx/strip.c ../../../sw/lib/gfx/strip_plan.c ../../../sw/lib/gfx/check.c \
    ../../../sw/lib/gfx/exec_sw.c ../../../sw/lib/gfx/cases.c \
    ../../../sw/lib/core/format.c >build/export.compile.log 2>&1 || \
   ! build/pix_cases_dump build/directed >build/export.log 2>&1; then
    echo '  directed_export: FAIL'; bad=$((bad+1))
else
    echo "  directed_export: $(cat build/export.log)"
    echo 'B1~B4 真实通路与随机反压 AXI RAM：'
    run_case path_golden  tb_engine_path PASS "$PATH_SRC"
fi
if ! build/pix_cases_dump --hazard build/hazard >build/hazard_export.log 2>&1; then
    echo '  hazard_export: FAIL'; bad=$((bad+1))
else
    echo "  hazard_export: $(cat build/hazard_export.log)"
    run_case path_hazard_wait   tb_engine_path PASS "$PATH_SRC" '-DCASE="build/hazard"'
    run_case path_hazard_nowait tb_engine_path FAIL "$PATH_SRC" '-DCASE="build/hazard"' -DNOWAIT
fi

if [ -n "$CASE_ONLY" ]; then
    [ "$matched" = 1 ] || { echo "RESULT: CASE-NOT-FOUND ($CASE_ONLY)"; exit 3; }
    case "${CASE_RESULT:-}" in
        "RESULT: PASS"*) exit 0 ;;
        *) exit 1 ;;
    esac
fi

if [ "$bad" -eq 0 ]; then
    echo "SIM OK: $total/$total RTL checks as expected"
else
    echo "SIM BAD: $bad of $total RTL checks unexpected"
    exit 1
fi
