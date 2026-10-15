#!/usr/bin/env bash
# 判据：三个像素开关的四组方程集各自 elaboration 通过。
# 像素位序 EQ_EXT/ALPHA/KEY；结构位取全开。
# 用法（仓根）：bash Main/sim/engine/features/elab_pixel.sh [工作目录]
set -u
ROOT=$(cd "$(dirname "$0")/../../../.." && pwd)
WORK=${1:-/tmp/gfx_feature_elab_pixel}
mkdir -p "$WORK"
cd "$ROOT"

# 交付构型要求 ALPHA 与 KEY 都为 1，所以只有 111 是合法的交付像素位；
# 其余组合以研究构型（不带 -d）验证，用来确认方程集收窄后仍能 elaborate。
fail=0
for pixel in 111 110 101 100 011 010 001 000; do
  suffix=""
  [ "$pixel" = "111" ] && suffix="-d"
  name="11111-$pixel$suffix"
  dir="$WORK/$name"
  rm -rf "$dir"
  if ! python3 Main/tools/check/feature_build.py --config "$name" --out-dir "$dir" \
        >"$WORK/$name.build.log" 2>&1; then
    echo "  BUILD-FAIL $name"
    fail=1
    continue
  fi
  if bash Main/sim/engine/features/elab_tree.sh "$dir" >"$WORK/$name.elab.log" 2>&1; then
    eq=$(python3 - "$dir" <<'PY'
import json, sys
f = json.load(open(sys.argv[1] + '/config.json'))['flags']
eqs = [0, 2] + ([1, 5] if f['ALPHA'] else []) + ([3, 4] if f['EQ_EXT'] else [])
print(','.join(map(str, eqs)))
PY
)
    echo "  ELAB-PASS $name  方程集 {$eq}"
  else
    echo "  ELAB-FAIL $name"
    tail -3 "$WORK/$name.elab.log" | sed 's/^/      /'
    fail=1
  fi
done
exit $fail
