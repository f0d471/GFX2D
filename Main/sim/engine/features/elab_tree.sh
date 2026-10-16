#!/usr/bin/env bash
# 构型 elaboration 冒烟：在构型自己的源树里编译 engine_top。
# 用法（仓根）：bash Main/sim/engine/features/elab_tree.sh <构型源树目录>
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
# 按构型启用的模块补进去；未启用的文件不存在于源树里也允许（源树里仍有副本）。
for extra in engine_tex engine_band engine_inst engine_bounds engine_packet_check \
             engine_pb_dma engine_bin engine_pb; do
  [ -f "rtl/engine/$extra.v" ] && files+=("rtl/engine/$extra.v")
done

iverilog -g2012 -Wportbind -I rtl/engine -o build_elab.vvp -s tb_feature_elab "$TB" "${files[@]}" \
  > build_elab.compile.log 2>&1
rc=$?
if [ $rc -ne 0 ]; then
  cat build_elab.compile.log
  echo "ELAB-FAIL $TREE"
  exit 1
fi
if grep -q 'dangling input port' build_elab.compile.log; then
  cat build_elab.compile.log
  echo "ELAB-FAIL $TREE: dangling input port"
  exit 1
fi
out=$(vvp build_elab.vvp)
case "$out" in
  *FEATURE-ELAB-PASS*) echo "ELAB-PASS $TREE" ;;
  *) echo "ELAB-BAD-OUTPUT $TREE: $out"; exit 1 ;;
esac
