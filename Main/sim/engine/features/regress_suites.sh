#!/usr/bin/env bash
# 构型改造后的套件回归：逐个跑指定仿真目录的最后一行结果。
# 机制专项套件（tex、band）在主工程构型关闭对应机制时记 NOT_APPLICABLE：
# 它们的测试台绑定被裁机制才有例化点（如 u_eng.tx_busy），关闭档跑不了属预期。
# 用法（仓根）：bash Main/sim/engine/features/regress_suites.sh [套件相对 sim/ 的路径...]
set -u
ROOT=$(cd "$(dirname "$0")/../../../.." && pwd)
cd "$ROOT/Main/sim"
FEATURE_H=$ROOT/Main/rtl/engine/engine_feature.vh
suites=("$@")
if [ ${#suites[@]} -eq 0 ]; then
  suites=(engine/blend engine/pix engine/tex engine/frame engine/geom engine/ctrl \
          engine/mover engine/band display apb_probe hdmi)
fi
export EFINITY_HOME=${EFINITY_HOME:-/mnt/c/Efinity/2026.1}
fail=0
for s in "${suites[@]}"; do
  printf '%-16s ' "$s"
  case "$s" in
    engine/tex)
      if ! grep -q 'define GFX_HAS_TEX' "$FEATURE_H"; then
        echo 'NOT_APPLICABLE（主工程构型无 GFX_HAS_TEX，专项测试台不适用）'; continue
      fi ;;
    engine/band)
      if ! grep -q 'define GFX_HAS_BAND_HW' "$FEATURE_H"; then
        echo 'NOT_APPLICABLE（主工程构型无 GFX_HAS_BAND_HW，专项测试台不适用）'; continue
      fi ;;
  esac
  last=$( cd "$s" && timeout 1800 bash run.sh 2>&1 | tail -1 )
  echo "$last"
  case "$last" in
    *OK*|*PASS*) : ;;
    *) fail=1 ;;
  esac
done
exit $fail
