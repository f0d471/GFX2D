#!/usr/bin/env bash
# 构型改造后的套件回归：逐个跑指定仿真目录的最后一行结果。
# 用法（仓根）：bash Main/sim/engine/features/regress_suites.sh [套件相对 sim/ 的路径...]
set -u
ROOT=$(cd "$(dirname "$0")/../../../.." && pwd)
cd "$ROOT/Main/sim"
suites=("$@")
if [ ${#suites[@]} -eq 0 ]; then
  suites=(engine/blend engine/pix engine/tex engine/frame engine/geom engine/ctrl \
          engine/mover engine/band display apb_probe hdmi)
fi
export EFINITY_HOME=${EFINITY_HOME:-/mnt/c/Efinity/2026.1}
fail=0
for s in "${suites[@]}"; do
  printf '%-16s ' "$s"
  last=$( cd "$s" && timeout 1800 bash run.sh 2>&1 | tail -1 )
  echo "$last"
  case "$last" in
    *OK*|*PASS*) : ;;
    *) fail=1 ;;
  esac
done
exit $fail
