#!/usr/bin/env bash
# TEX 普通单元/整机判据；外部变异使用同一激励与检查器。
set -eu
cd "$(dirname "$0")"
selected="${2:-}"
if [ "${1:-}" != "" ] && [ "${1:-}" != "--case" ]; then exit 2; fi
case "$selected" in ""|normal|normal_latency2) ;; *) echo "unknown case: $selected"; exit 2;; esac
mkdir -p build
RTL=../../../rtl
python3 ../../../tools/check/feature_build.py --config B3 --out-dir build/include --headers-only
# 所有输入现场生成，不消费历史 build。
gcc -O2 -std=c11 -Wall -Wextra -Werror -I../../../tools/check -I../../../sw/lib \
    -o build/dump ../../../tools/check/pix_cases_dump.c \
    ../../../sw/lib/gfx/list.c ../../../sw/lib/gfx/strip.c ../../../sw/lib/gfx/strip_plan.c ../../../sw/lib/gfx/check.c \
    ../../../sw/lib/gfx/exec_sw.c ../../../sw/lib/gfx/cases.c ../../../sw/lib/core/format.c
./build/dump --tex-sprites build/tex_sprites > build/dump.log
RTL_FILES="$RTL/engine/buffer/engine_tex.v $RTL/engine/core/engine_geom.v $RTL/engine/core/engine_cmd.v $RTL/engine/frontend/engine_inst.v $RTL/engine/frontend/engine_bounds.v $RTL/engine/frontend/engine_packet_check.v $RTL/engine/frontend/engine_pb_dma.v $RTL/engine/frontend/engine_bin.v $RTL/engine/frontend/engine_pb.v
$RTL/engine/buffer/engine_band.v $RTL/engine/core/engine_mover.v $RTL/engine/core/engine_regs.v $RTL/engine/engine_top.v $RTL/engine/core/engine_config_guard.v
$RTL/engine/core/engine_pix.v $RTL/engine/core/engine_blend.v $RTL/vendor/verilog-axi/axi_dma_rd.v $RTL/vendor/verilog-axi/axi_dma_wr.v ../../../sim/axi_probe/axi_ram_model.v"
for name in normal normal_latency2; do
    if [ -n "$selected" ] && [ "$selected" != "$name" ]; then continue; fi
    defs=""
    if [ "$name" = normal_latency2 ]; then defs='-Ptb_engine_tex.TEX_LATENCY=2'; fi
    if ! iverilog -g2012 -I build/include -I "$RTL/engine/include" -s tb_engine_tex -DCASE='"build/tex_sprites"' $defs ${G1_EXTRA_DEFS:-} \
         -o build/$name.vvp tb_engine_tex.v $RTL_FILES > build/$name.compile.log 2>&1; then
        echo "$name: COMPILE-FAIL"; exit 1
    fi
    if ! vvp -n build/$name.vvp > build/$name.log 2>&1; then cat build/$name.log; exit 1; fi
    if ! grep -q 'TEX SIM OK' build/$name.log; then cat build/$name.log; echo "$name: RESULT: FAIL"; exit 1; fi
    echo "$name: RESULT: PASS"
done
echo 'TEX suite PASS'
