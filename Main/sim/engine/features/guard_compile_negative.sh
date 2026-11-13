#!/usr/bin/env bash
# 判据：绕过生成器造出的非法构型，必须在编译阶段失败，且诊断里出现违规边的名字。
#
# 背景：守卫曾经用「9 个空模块声明」让例化合法，于是非法组合能一路编过（2026-11-13 实测
# B5 删掉 GFX_HAS_BAND_HW 后 iverilog 退出码 0）。现在不再声明那 9 个模块，缺模块就是
# iverilog / hierarchy -check / 厂商综合（blackbox-error=1）的硬失败。这条判据守住这个性质：
#   1) 合法 B5 必须编得过（对照，防止判据因为别的原因假通过）；
#   2) 删掉 BAND_HW 后必须编不过，且诊断里出现 PB_REQUIRES_BAND_HW。
set -u
ROOT=$(cd "$(dirname "$0")/../../../.." && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cd "$ROOT" || exit 3

if ! python3 Main/tools/check/feature_build.py --config B5 --out-dir "$WORK/legal" \
     >"$WORK/build.log" 2>&1; then
  echo "SETUP-FAIL 生成 B5 源树失败"; tail -3 "$WORK/build.log"; exit 3
fi
cp -r "$WORK/legal" "$WORK/illegal"

HDR="$WORK/illegal/rtl/engine/include/engine_feature.vh"
[ -f "$HDR" ] || { echo "SETUP-FAIL 缺少 $HDR"; exit 3; }
# 删掉 BAND_HW、保留 PB：违反「PB 需要 BAND_HW」
sed -i '/`define GFX_HAS_BAND_HW/d' "$HDR"

compile() {  # <树目录> <输出名>
  local tree="$1" name="$2"
  local files=(
    rtl/engine/engine_top.v rtl/engine/core/engine_config_guard.v
    rtl/engine/core/engine_regs.v rtl/engine/core/engine_cmd.v
    rtl/engine/core/engine_geom.v rtl/engine/core/engine_pix.v
    rtl/engine/core/engine_blend.v rtl/engine/core/engine_mover.v
    rtl/vendor/verilog-axi/axi_dma_rd.v rtl/vendor/verilog-axi/axi_dma_wr.v
  )
  local extra
  for extra in buffer/engine_tex buffer/engine_band \
               frontend/engine_inst frontend/engine_bounds frontend/engine_packet_check \
               frontend/engine_pb_dma frontend/engine_bin frontend/engine_pb; do
    [ -f "$tree/rtl/engine/$extra.v" ] && files+=("rtl/engine/$extra.v")
  done
  ( cd "$tree" && iverilog -g2012 -I rtl/engine/include -s engine_top \
      -o "$WORK/$name.vvp" "${files[@]}" ) >"$WORK/$name.log" 2>&1
  return $?
}

bad=0
if compile "$WORK/legal" baseline; then
  echo "BASELINE-PASS 合法 B5 编得过"
else
  echo "BASELINE-FAIL 合法 B5 编不过，判据失效："
  head -3 "$WORK/baseline.log"
  bad=1
fi

if compile "$WORK/illegal" illegal; then
  echo "GUARD-FAIL 非法构型（PB 缺 BAND_HW）编过了：守卫没有拦住"
  bad=1
elif grep -q 'GFX_CONFIG_ERROR_PB_REQUIRES_BAND_HW' "$WORK/illegal.log"; then
  echo "GUARD-PASS 非法构型被拒绝，诊断点名 PB_REQUIRES_BAND_HW"
else
  echo "GUARD-WRONG-REASON 编不过，但不是守卫引起的："
  head -3 "$WORK/illegal.log"
  bad=1
fi

exit $bad
