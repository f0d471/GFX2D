#!/usr/bin/env bash
# 引擎 P3 G1：sim/engine/tex 的正式判据（两个层级见 tb_engine_tex.v 文件头）。
# 金标准由 tools/check/pix_cases_dump.c --tex-sprites 现算（软件执行器经 TEX
# 影子窗口跑出期望内存），不照抄任何现成文件。注入构建只跑单元级，
# 对应判据见红才算过；正常构建要求单元级 + 整机级全绿输出 TEX SIM OK。
# 前期研究的四组实验仍在 tools/check/g1_research.py（B4 台现场补丁，证据在 logs/）。
set -u
cd "$(dirname "$0")"
mkdir -p build
RTL=../../../rtl
bad=0

gcc -O2 -std=c11 -Wall -Wextra -Werror -I../../../tools/check -I../../../sw/lib \
    -o build/dump ../../../tools/check/pix_cases_dump.c \
    ../../../sw/lib/gfx/list.c ../../../sw/lib/gfx/check.c \
    ../../../sw/lib/gfx/exec_sw.c ../../../sw/lib/gfx/cases.c \
    ../../../sw/lib/core/format.c || exit 1
./build/dump --tex-sprites build/tex_sprites > build/dump.log || exit 1

RTL_FILES="$RTL/engine/engine_tex.v $RTL/engine/engine_geom.v $RTL/engine/engine_cmd.v \
    $RTL/engine/engine_mover.v $RTL/engine/engine_regs.v $RTL/engine/engine_top.v \
    $RTL/engine/engine_pix.v $RTL/engine/engine_blend.v \
    $RTL/vendor/verilog-axi/axi_dma_rd.v $RTL/vendor/verilog-axi/axi_dma_wr.v \
    ../../../sim/axi_probe/axi_ram_model.v"

run_case() {  # <名字> <期望 PASS|FAIL> [注错值]
    local name="$1" want="$2" inj="${3:-}"
    local defs=""
    [[ "$inj" =~ ^[124]$ ]] && defs="-DINJECT_TEX_VAL=$inj"
    if ! iverilog -g2012 -s tb_engine_tex -DCASE='"build/tex_sprites"' $defs \
         ${G1_EXTRA_DEFS:-} -I $RTL/engine -o build/$name.vvp tb_engine_tex.v $RTL_FILES; then
        echo "$name: COMPILE-FAIL"
        bad=1
        return
    fi
    if ! vvp -n build/$name.vvp > build/$name.log 2>&1; then
        echo "$name: RUNTIME-FAIL (build/$name.log)"
        bad=1
        return
    fi
    if [ "$want" = PASS ]; then
        if grep -q "TEX SIM OK" build/$name.log; then
            echo "$name: RESULT: PASS"
        else
            echo "$name: RESULT: FAIL"
            grep -m 8 "FAIL\|MISMATCH\|TIMEOUT\|WATCHDOG\|NOT-EXERCISED" build/$name.log
            bad=1
        fi
    else
        local evidence=""
        case "$inj" in
            1) evidence='STR-DATA' ;;
            2) evidence='APB-UNALIGN-SIDE-EFFECT' ;;
            4) evidence='TEX INJECT 4 STATUS_MISSING' ;;
            stream) evidence='PIXEL_STREAM_COLLISION' ;;
        esac
        if ! grep -q "TEX INJECT NOT_RED" build/$name.log &&
           grep -q "$evidence" build/$name.log &&
           { [ "$inj" = 4 ] || { [ "$inj" = stream ] && grep -q 'TEX MIX INJECT red' build/$name.log; } ||
             grep -qE "TEX INJECT $inj red=[1-9][0-9]*" build/$name.log; }; then
            echo "$name: RESULT: FAIL (inject, as expected)"
        else
            echo "$name: RESULT: NOT_RED (missing targeted evidence)"
            bad=1
        fi
    fi
}

run_case normal PASS
G1_EXTRA_DEFS='-Ptb_engine_tex.TEX_LATENCY=2' run_case normal_latency2 PASS
run_case inj_valid FAIL 1
run_case inj_unalign FAIL 2
run_case inj_stat FAIL 4
G1_EXTRA_DEFS='-Ptb_engine_tex.MOVER_INJECT=64' run_case inj_stream FAIL stream

if [ $bad = 0 ]; then
    echo "SIM OK: 6 checks as expected"
else
    echo "SIM FAIL"
    exit 1
fi
