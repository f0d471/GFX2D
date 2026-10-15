#!/usr/bin/env bash
# 在构型源树里编 engine_top 并原样打印编译器输出（诊断用，不判通过）。
# 用法：bash Main/sim/engine/features/elab_verbose.sh <构型源树目录>
set -u
TREE=$(cd "${1:?需要构型源树目录}" && pwd)
TB="$(cd "$(dirname "$0")" && pwd)/tb_feature_elab.v"
cd "$TREE"
files=(
  rtl/engine/engine_top.v rtl/engine/engine_regs.v rtl/engine/engine_cmd.v
  rtl/engine/engine_geom.v rtl/engine/engine_pix.v rtl/engine/engine_blend.v
  rtl/engine/engine_mover.v
  rtl/vendor/verilog-axi/axi_dma_rd.v rtl/vendor/verilog-axi/axi_dma_wr.v
)
for extra in engine_tex engine_band engine_inst engine_bounds engine_packet_check \
             engine_pb_dma engine_bin engine_pb; do
  [ -f "rtl/engine/$extra.v" ] && files+=("rtl/engine/$extra.v")
done
iverilog -g2012 -I rtl/engine -o /dev/null -s tb_feature_elab "$TB" "${files[@]}" 2>&1 | head -40
