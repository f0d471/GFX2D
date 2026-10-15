#!/usr/bin/env bash
# 在七个代表档上验证：生成构型源树 -> elaboration。
# 用法（仓根）：bash Main/sim/engine/features/elab_all.sh [工作目录]
set -u
ROOT=$(cd "$(dirname "$0")/../../../.." && pwd)
WORK=${1:-/tmp/gfx_feature_elab}
mkdir -p "$WORK"
cd "$ROOT"

fail=0
for preset in B0 B1 B2 B3 B4 B5 B6; do
  tree="$WORK/$preset"
  rm -rf "$tree"
  if ! python3 Main/tools/check/feature_build.py --config "$preset" --out-dir "$tree" >"$WORK/$preset.build.log" 2>&1; then
    echo "BUILD-FAIL $preset"; fail=1; continue
  fi
  if ! bash Main/sim/engine/features/elab_tree.sh "$tree" 2>&1 | tail -2; then
    fail=1
  fi
done
exit $fail
