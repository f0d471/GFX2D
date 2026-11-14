#!/usr/bin/env bash
# sim/engine/tex 的正式判据（两个层级见 tb_engine_tex.v 文件头）。
# 金标准由 tools/check/pix_cases_dump.c --tex-sprites 现算（软件执行器经 TEX
# 影子窗口跑出期望内存），不照抄任何现成文件。注入构建只跑单元级，
# 对应判据见红才算过；正常构建要求单元级 + 整机级全绿输出 TEX SIM OK。
# 前期研究的四组实验仍在 tools/check/g1_research.py（现场补丁，证据在 logs/）。
set -u
cd "$(dirname "$0")"
mkdir -p build
RTL=../../../rtl
bad=0
python3 ../../../tools/check/feature_build.py --config B3 --out-dir build/include --headers-only || exit 1

gcc -O2 -std=c11 -Wall -Wextra -Werror -I../../../tools/check -I../../../sw/lib \
    -o build/dump ../../../tools/check/pix_cases_dump.c \
    ../../../sw/lib/gfx/list.c ../../../sw/lib/gfx/strip.c ../../../sw/lib/gfx/strip_plan.c ../../../sw/lib/gfx/check.c \
    ../../../sw/lib/gfx/exec_sw.c ../../../sw/lib/gfx/cases.c \
    ../../../sw/lib/core/format.c || exit 1
./build/dump --tex-sprites build/tex_sprites > build/dump.log || exit 1

RTL_FILES="$RTL/engine/buffer/engine_tex.v $RTL/engine/core/engine_geom.v $RTL/engine/core/engine_cmd.v $RTL/engine/frontend/engine_inst.v $RTL/engine/frontend/engine_bounds.v $RTL/engine/frontend/engine_packet_check.v $RTL/engine/frontend/engine_pb_dma.v $RTL/engine/frontend/engine_bin.v $RTL/engine/frontend/engine_pb.v \
    $RTL/engine/buffer/engine_band.v $RTL/engine/core/engine_mover.v $RTL/engine/core/engine_regs.v $RTL/engine/engine_top.v $RTL/engine/core/engine_config_guard.v \
    $RTL/engine/core/engine_pix.v $RTL/engine/core/engine_blend.v \
    $RTL/vendor/verilog-axi/axi_dma_rd.v $RTL/vendor/verilog-axi/axi_dma_wr.v \
    ../../../sim/axi_probe/axi_ram_model.v"

run_case() {  # <名字> <期望 PASS|FAIL> [注错开关]
    local name="$1" want="$2" inj="${3:-}"
    if ! iverilog -g2012 -I build/include -I "$RTL/engine/include" -s tb_engine_tex -DCASE='"build/tex_sprites"' $inj \
         ${G1_EXTRA_DEFS:-} -I $RTL/engine/include -o build/$name.vvp tb_engine_tex.v $RTL_FILES; then
        echo "$name: COMPILE-FAIL"
        bad=1
        return
    fi
    if ! vvp -n build/$name.vvp > build/$name.log 2>&1; then
        # 看门狗用 $fatal 收尾，vvp 退出码非零；那是这处变异见红的一种形态，
        # 不能在这里就判 RUNTIME-FAIL（下面按证据串核对）。
        if ! grep -q 'TEX WATCHDOG' build/$name.log; then
            echo "$name: RUNTIME-FAIL (build/$name.log)"
            bad=1
            return
        fi
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
        # 日志里的位号（TEX INJECT n）由测试台按开关名换算，这里也换算一次用于核对
        local injno evidence=""
        case "$inj" in
            -DINJ_VALID_EARLY) injno=1; evidence='STR-DATA' ;;
            -DINJ_WR_LOOSE)    injno=2; evidence='APB-UNALIGN-SIDE-EFFECT' ;;
            -DINJ_STAT_DROP)   injno=4; evidence='TEX INJECT 4 STATUS_MISSING' ;;
            # 这处注入打在搬运层：目的流不再等 TEX 收尾，两个流相撞。碰撞监视器抓到就打
            # PIXEL_STREAM_COLLISION 并收尾；撞到之前先卡住时由整机级看门狗兜底。
            # 两种形态都算它见红，证据串按实际出现的那一种取。
            -DINJ_TX_NOWAIT)   injno=64
                               if grep -q 'PIXEL_STREAM_COLLISION' build/$name.log; then
                                   evidence='PIXEL_STREAM_COLLISION'
                               else
                                   evidence='TEX WATCHDOG'
                               fi ;;
            *)                 injno=0 ;;
        esac
        if ! grep -q "TEX INJECT NOT_RED" build/$name.log &&
           grep -q "$evidence" build/$name.log &&
           { [ "$injno" = 4 ] || [ "$injno" = 64 ] ||
             grep -qE "TEX INJECT $injno red=[1-9][0-9]*" build/$name.log; }; then
            echo "$name: RESULT: FAIL (inject, as expected)"
        else
            echo "$name: RESULT: NOT_RED (missing targeted evidence)"
            bad=1
        fi
    fi
}

run_case normal PASS
G1_EXTRA_DEFS='-Ptb_engine_tex.TEX_LATENCY=2' run_case normal_latency2 PASS
# 变异（外部活动）：四个变异体在 Main/sim/mutation/mutants.yaml，
# 由 normal / normal_latency2 承担发现责任。

if [ $bad = 0 ]; then
    echo "SIM OK: 6 checks as expected"
else
    echo "SIM FAIL"
    exit 1
fi
