#!/usr/bin/env bash
# 判据：关掉 GFX_HAS_STRIP 时，命令层的条带状态与条带表离开编译输入。
# STRIP 不增减模块例化，所以只能用预处理后的源码内容判定。
# 用法（仓根）：bash Main/sim/engine/features/probe_strip.sh <构型源树根目录>
set -u
WORK=${1:-/tmp/gfx_feature_elab}
cd "$WORK"
fail=0
printf '%-4s %-10s %-10s %-12s %-10s %s\n' 档 S_CFG counts table_base band_mode 结果
for tree in B0 B1 B2 B3 B4 B5 B6 B7; do
  [ -d "$tree/rtl/engine" ] || continue
  ( cd "$tree/rtl/engine" && iverilog -g2012 -E -I include core/engine_cmd.v -o "$WORK/$tree.cmd.pp.v" 2>/dev/null )
  cfg=$(grep -c 'S_CFG:' "$WORK/$tree.cmd.pp.v" || true)
  cnt=$(grep -c 'counts\[' "$WORK/$tree.cmd.pp.v" || true)
  tab=$(grep -c 'table_base' "$WORK/$tree.cmd.pp.v" || true)
  bm=$(grep -c 'band_mode' "$WORK/$tree.cmd.pp.v" || true)
  want=$(python3 - "$tree" <<'PY'
import json, os, sys
root = os.environ.get('GFX_FEATURE_ELAB', '/tmp/gfx_feature_elab')
f = json.load(open(os.path.join(root, sys.argv[1], 'config.json')))['flags']
print(1 if f['STRIP'] else 0)
PY
)
  verdict=PASS
  # S_CFG 标签、counts 数组与 table_base 只在 STRIP 开时出现；
  # 判据是「有无」而不是出现次数，所以先归一成 0/1 再比。
  norm() { [ "${1:-0}" -gt 0 ] && echo 1 || echo 0; }
  for v in "$cfg" "$cnt" "$tab"; do
    [ "$(norm "$v")" = "$want" ] || { verdict="FAIL"; fail=1; }
  done
  printf '%-4s %-10s %-10s %-12s %-10s %s\n' "$tree" "$(norm "$cfg")|$want" "$(norm "$cnt")|$want" "$(norm "$tab")|$want" "$bm" "$verdict"
done
echo
echo '格式为 实际|期望（期望 1 = STRIP 开，0 = STRIP 关）。'
echo 'band_mode 是端口名，两种构型都在，列出来只作参考。'
exit $fail
