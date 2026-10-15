#!/usr/bin/env bash
# 判据：全部合法结构组合逐个 elaboration 通过。
# 像素位与交付位取全开（对结构切割无影响），结构位穷举 feature_config 判定的 16 种。
# 用法（仓根）：bash Main/sim/engine/features/elab_legal.sh [工作目录]
set -u
ROOT=$(cd "$(dirname "$0")/../../../.." && pwd)
WORK=${1:-/tmp/gfx_feature_elab_legal}
mkdir -p "$WORK"
cd "$ROOT"

mapfile -t configs < <(python3 - "$WORK" <<'PY'
import sys
sys.path.insert(0, 'Main/tools/check')
import feature_config as fc
for config in fc.legal_configurations(with_pixel=False):
    bits = ''.join('1' if config[n] else '0' for n in fc.STRUCT_NAMES)
    print(bits + '-111-d')
PY
)
echo "合法结构组合 ${#configs[@]} 种"

fail=0
for name in "${configs[@]}"; do
  dir="$WORK/$name"
  rm -rf "$dir"
  if ! python3 Main/tools/check/feature_build.py --config "$name" --out-dir "$dir" \
        >"$WORK/$name.build.log" 2>&1; then
    echo "  BUILD-FAIL $name"; fail=1; continue
  fi
  if out=$(bash Main/sim/engine/features/elab_tree.sh "$dir" 2>&1); then
    echo "  ELAB-PASS $name"
  else
    echo "  ELAB-FAIL $name"; fail=1
  fi
done

# 反例：非法结构组合必须被构建入口拒绝。
python3 - "$WORK" <<'PY' > "$WORK/invalid.txt"
import sys
sys.path.insert(0, 'Main/tools/check')
import itertools, feature_config as fc
rows = []
for bits in itertools.product((0, 1), repeat=len(fc.STRUCT_NAMES)):
    config = dict(zip(fc.STRUCT_NAMES, (b == 1 for b in bits)))
    full = {**config, **{n: True for n in fc.PIXEL_NAMES}, 'DELIVERY': True}
    if fc.check(full):
        rows.append(''.join(str(b) for b in bits) + '-111-d')
print('\n'.join(rows))
PY
illegal=$(wc -l < "$WORK/invalid.txt")
echo "非法结构组合 $illegal 种，逐个验证被拒"
while read -r name; do
  [ -n "$name" ] || continue
  if python3 Main/tools/check/feature_build.py --config "$name" --out-dir "$WORK/rej-$name" \
       >"$WORK/rej-$name.log" 2>&1; then
    echo "  ACCEPTED-ILLEGAL $name"; fail=1
  fi
done < "$WORK/invalid.txt"
echo "非法组合拒绝验证完成"

exit $fail
