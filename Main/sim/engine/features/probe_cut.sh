#!/usr/bin/env bash
# 判据：被关闭的机制必须离开编译输入，被开启的必须还在。
# 对预处理后的源文件数实例化点；期望值由 feature_config 的开关语义决定。
# 用法（仓根）：bash Main/sim/engine/features/probe_cut.sh <构型源树根目录>
set -u
WORK=${1:-/tmp/gfx_feature_elab}
cd "$WORK"
fail=0
printf '%-4s %-8s %-8s %-8s %-8s %s\n' 档 TEX PB BAND INST 结果
for tree in B0 B1 B2 B3 B4 B5 B6; do
  [ -d "$tree/rtl/engine" ] || continue
  ( cd "$tree/rtl/engine" && iverilog -g2012 -E engine_top.v  -o "$WORK/$tree.top.v"   2>/dev/null
                            iverilog -g2012 -E engine_mover.v -o "$WORK/$tree.mover.v" 2>/dev/null
                            iverilog -g2012 -E engine_cmd.v   -o "$WORK/$tree.cmd.v"   2>/dev/null )
  tex=$(grep -c 'engine_tex #' "$WORK/$tree.top.v" || true)
  pb=$(grep -c 'engine_pb #' "$WORK/$tree.top.v" || true)
  band=$(grep -c 'engine_band #' "$WORK/$tree.mover.v" || true)
  inst=$(grep -c 'engine_inst u_inst' "$WORK/$tree.cmd.v" || true)
  # 期望值：结构位序 TEX/BAND_HW/STRIP/INST/PB
  expect=$(python3 - "$tree" <<'PY'
import json, sys, os
root = os.environ.get('GFX_FEATURE_ELAB', '/tmp/gfx_feature_elab')
record = json.load(open(os.path.join(root, sys.argv[1], 'config.json')))
f = record['flags']
print(f"{1 if f['TEX'] else 0} {1 if f['PB'] else 0} {1 if f['BAND_HW'] else 0} {1 if f['INST'] else 0}")
PY
)
  want_tex=$(echo "$expect" | cut -d' ' -f1); want_pb=$(echo "$expect" | cut -d' ' -f2)
  want_band=$(echo "$expect" | cut -d' ' -f3); want_inst=$(echo "$expect" | cut -d' ' -f4)
  verdict=PASS
  [ "$tex" != "$want_tex" ] && { verdict="FAIL TEX"; fail=1; }
  [ "$pb" != "$want_pb" ] && { verdict="FAIL PB"; fail=1; }
  [ "$band" != "$want_band" ] && { verdict="FAIL BAND_HW"; fail=1; }
  [ "$inst" != "$want_inst" ] && { verdict="FAIL INST"; fail=1; }
  printf '%-4s %-8s %-8s %-8s %-8s %s\n' "$tree" "$tex|$want_tex" "$pb|$want_pb" "$band|$want_band" "$inst|$want_inst" "$verdict"
done
echo
echo 'TEX=engine_tex 例化数  PB=engine_pb 例化数  BAND=engine_band 例化数  INST=engine_cmd 的 engine_inst 例化数'
echo '格式为 实际|期望。BAND 与 INST 的切割尚未实现，那两列会 FAIL，属预期。'
exit $fail
