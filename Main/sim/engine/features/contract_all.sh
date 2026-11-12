#!/usr/bin/env bash
# 按构型的禁用契约判据：CAPS 位、CTRL 不支持位、保留寄存器读 0 写拒绝、
# 无实例哨兵。对七个代表档各编译运行一次 tb_feature_contract.v。
# 用法（仓根）：bash Main/sim/engine/features/contract_all.sh [构型源树根]
set -u
ROOT=$(cd "$(dirname "$0")/../../../.." && pwd)
WORK=${1:-/tmp/gfx_feature_elab}
TB="$(cd "$(dirname "$0")" && pwd)/tb_feature_contract.v"
fail=0
for tree in B0 B1 B2 B3 B4 B5 B6 B7; do
  dir="$WORK/$tree"
  [ -d "$dir/rtl/engine" ] || { echo "  INCOMPLETE $tree（无源树）"; exit 3; }
  cd "$dir"
  files=(
    rtl/engine/engine_top.v rtl/engine/core/engine_regs.v rtl/engine/core/engine_cmd.v
    rtl/engine/core/engine_geom.v rtl/engine/core/engine_pix.v rtl/engine/core/engine_blend.v
    rtl/engine/core/engine_mover.v
    rtl/vendor/verilog-axi/axi_dma_rd.v rtl/vendor/verilog-axi/axi_dma_wr.v
  )
  for extra in buffer/engine_tex buffer/engine_band \
               frontend/engine_inst frontend/engine_bounds frontend/engine_packet_check \
               frontend/engine_pb_dma frontend/engine_bin frontend/engine_pb \
               rast/engine_r3 rast/engine_rlst rast/engine_rast; do
    [ -f "rtl/engine/$extra.v" ] && files+=("rtl/engine/$extra.v")
  done
  if ! iverilog -g2012 -I rtl/engine -o build_contract.vvp -s tb_feature_contract \
        "$TB" "${files[@]}" > build_contract.compile.log 2>&1; then
    echo "  COMPILE-FAIL $tree"; tail -3 build_contract.compile.log | sed 's/^/      /'
    fail=1; continue
  fi
  out=$(vvp build_contract.vvp)
  echo "$out" | sed "s/^/  [$tree] /"
  case "$out" in *CONTRACT-PASS*) ;; *) fail=1 ;; esac
  cd "$ROOT"
done
exit $fail
