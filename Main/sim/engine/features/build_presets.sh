#!/usr/bin/env bash
# 在 WSL 里生成七个代表档的构型源树（纯 Python，不需要 Efinity）。
# 综合另走 Windows 侧：python Main/tools/check/feature_synthesis.py ...
#
# 默认**不覆盖已存在的树**：综合产物（outflow 下的 .bit、map 网表、place 报告）
# 都在树里，重造会把它们删掉。要重建某档用 FORCE=1。
# 用法（仓根）：bash Main/sim/engine/features/build_presets.sh [输出目录]
set -u
ROOT=$(cd "$(dirname "$0")/../../../.." && pwd)
WORK=${1:-$ROOT/Main/sim/build/feature-trees}
FORCE=${FORCE:-0}
cd "$ROOT"
for preset in B0 B1 B2 B3 B4 B5 B6; do
  if [ -d "$WORK/$preset" ] && [ "$FORCE" != "1" ]; then
    echo "SKIP $preset（已存在；FORCE=1 覆盖）"
    continue
  fi
  rm -rf "$WORK/$preset"
  if python3 Main/tools/check/feature_build.py --config "$preset" --out-dir "$WORK/$preset"; then
    :
  else
    echo "BUILD-FAIL $preset"
  fi
done
