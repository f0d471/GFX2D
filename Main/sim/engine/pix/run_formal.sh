#!/usr/bin/env bash
# 引擎 P1 B4：入库 RTL rtl/engine/engine_pix.v 的正式判据
#
# 三组，期望值都不取自被测：
#   unit_*   tb_engine_pix.v，一行一行地跑：sweep 400 行（npx 1..25 × 源/目的相位 4×4）、
#            FILL 六方程、拆段四行、长行八行，逐槽对照 exec_sw.c 的像素公式；
#            写流下游恒 ready 与每 7 拍停 1 拍各一遍；五个注错各自见红。
#   pipe_*   tb_engine_pix_pipe.v，按 I5 允许的最快节奏发行（行与行之间不等，只等 row_free），
#            两半同时有货；命令 1~5 行、半号照几何层；写流下游恒 ready、周期停顿、成串停顿三种；
#            不看 tready 与混合核不随流水停两个注错在成串停顿下见红。
#   path_*   tb_engine_path.v，B1~B4 真实通路（几何层、计算阵列、搬运层、两个 DMA 核、随机反压的
#            AXI RAM），命令与期望内存由 tools/check/pix_cases_dump.c 经软件执行器导出，
#            512 KiB 存储逐 128 位比对；色键注错见红。另有跨命令写后读的一组：
#            等上一条写回完成再提交时对，几何层一空闲就提交（-DNOWAIT）时必须见红——
#            B1~B4 自己不挡这种冲突，B5 的命令下发要处理它。
#
# 注错位与 rtl/engine/engine_pix.v 文件头一一对应。
# 需要 WSL 里的 iverilog 12 与 gcc；不需要 EFINITY_HOME（不经过 Axi_Mux）。
set -u
cd "$(dirname "$0")"
mkdir -p build
RTL=../../../rtl
bad=0
total=0

# run_case <名字> <顶层> <期望 PASS|FAIL> <源文件> [定义...]
run_case() {
    local name="$1" top="$2" want="$3" src="$4"
    shift 4
    total=$((total+1))
    if ! iverilog -g2012 -s "$top" "$@" \
        -o "build/$name.vvp" $src >"build/$name.compile.log" 2>&1; then
        echo "  $name: COMPILE FAIL (build/$name.compile.log)"; bad=$((bad+1)); return
    fi
    vvp -n "build/$name.vvp" >"build/$name.log" 2>&1
    local got stat
    got=$(grep '^RESULT:' "build/$name.log" | tail -1)
    stat=$(grep -E '^(pipe|PATH stats)' "build/$name.log" | tail -1)
    echo "  $name: ${got:-RESULT: MISSING}"
    [ -n "$stat" ] && [ "$want" = PASS ] && echo "      $stat"
    case "$got" in
        "RESULT: $want"*) ;;
        *) bad=$((bad+1));;
    esac
}

UNIT="tb_engine_pix.v $RTL/engine/engine_pix.v $RTL/engine/engine_blend.v"
PIPE="tb_engine_pix_pipe.v $RTL/engine/engine_pix.v $RTL/engine/engine_blend.v"
PATH_SRC="tb_engine_path.v $RTL/engine/engine_geom.v $RTL/engine/engine_pix.v \
$RTL/engine/engine_blend.v $RTL/engine/engine_mover.v \
$RTL/vendor/verilog-axi/axi_dma_rd.v $RTL/vendor/verilog-axi/axi_dma_wr.v \
../../axi_probe/axi_ram_model.v"

echo 'B4 入库像素单元（一行一行）：'
run_case unit_ready     tb_engine_pix PASS "$UNIT"
run_case unit_stall     tb_engine_pix PASS "$UNIT" -DSTALL
run_case unit_inj_rot   tb_engine_pix FAIL "$UNIT" -DINJ_ROT_OFF    -DSTALL
run_case unit_inj_key   tb_engine_pix FAIL "$UNIT" -DINJ_SKIP_KEY    -DSTALL
run_case unit_inj_mask  tb_engine_pix FAIL "$UNIT" -DINJ_DROP_MASK   -DSTALL
run_case unit_inj_ready tb_engine_pix FAIL "$UNIT" -DINJ_EN_NOTREADY -DSTALL
run_case unit_inj_core  tb_engine_pix FAIL "$UNIT" -DINJ_CORE_NOEN   -DSTALL

echo 'B4 入库像素单元（乒乓与流水，行与行之间不等）：'
run_case pipe_ready     tb_engine_pix_pipe PASS "$PIPE" -DREADY_MODE=0
run_case pipe_stall     tb_engine_pix_pipe PASS "$PIPE" -DREADY_MODE=1
run_case pipe_burst     tb_engine_pix_pipe PASS "$PIPE" -DREADY_MODE=2
run_case pipe_inj_ready tb_engine_pix_pipe FAIL "$PIPE" -DINJ_EN_NOTREADY -DREADY_MODE=2
run_case pipe_inj_core  tb_engine_pix_pipe FAIL "$PIPE" -DINJ_CORE_NOEN -DREADY_MODE=2

if ! gcc -O2 -std=c11 -Wall -Wextra -Werror -I../../../sw/lib \
    -o build/pix_cases_dump ../../../tools/check/pix_cases_dump.c \
    ../../../sw/lib/gfx/list.c ../../../sw/lib/gfx/check.c \
    ../../../sw/lib/gfx/exec_sw.c ../../../sw/lib/gfx/cases.c \
    ../../../sw/lib/core/format.c >build/export.compile.log 2>&1 || \
   ! build/pix_cases_dump build/directed >build/export.log 2>&1; then
    echo '  directed_export: FAIL'; bad=$((bad+1))
else
    echo "  directed_export: $(cat build/export.log)"
    echo 'B1~B4 真实通路与随机反压 AXI RAM：'
    run_case path_golden  tb_engine_path PASS "$PATH_SRC"
    run_case path_inj_key tb_engine_path FAIL "$PATH_SRC" -DINJ_SKIP_KEY
fi
if ! build/pix_cases_dump --hazard build/hazard >build/hazard_export.log 2>&1; then
    echo '  hazard_export: FAIL'; bad=$((bad+1))
else
    echo "  hazard_export: $(cat build/hazard_export.log)"
    run_case path_hazard_wait   tb_engine_path PASS "$PATH_SRC" '-DCASE="build/hazard"'
    run_case path_hazard_nowait tb_engine_path FAIL "$PATH_SRC" '-DCASE="build/hazard"' -DNOWAIT
fi

if [ "$bad" -eq 0 ]; then
    echo "SIM OK: $total/$total RTL checks as expected"
else
    echo "SIM BAD: $bad of $total RTL checks unexpected"
    exit 1
fi
