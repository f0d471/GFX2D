#!/usr/bin/env bash
# 生成七个代表档的构型源树并逐个跑整机四步综合，汇总资源与时序。
# Windows + Efinity 命令行；用法（仓根）：
#   bash Main/sim/engine/features/synth_presets.sh [输出目录]
set -u
ROOT=$(cd "$(dirname "$0")/../../../.." && pwd)
WORK=${1:-$ROOT/Main/sim/build/feature-trees}
cd "$ROOT"

PY="C:/Efinity/2026.1/python311/bin/python.exe"
export PYTHONHOME="C:\\Efinity\\2026.1\\python311"
export EFINITY_HOME="C:\\Efinity\\2026.1"

for preset in B0 B1 B2 B3 B4 B5 B6; do
  rm -rf "$WORK/$preset"
  "$PY" Main/tools/check/feature_build.py --config "$preset" --out-dir "$WORK/$preset" \
    || { echo "BUILD-FAIL $preset"; continue; }
done

# 逐个跑；某个构型超器件上限时 pnr 会失败，脚本对该档记 FAIL 并继续。
# 本进程需要 PYTHONHOME 才能启动；厂商子进程的环境由 feature_synthesis.py 自行摘除。
"$PY" Main/tools/check/feature_synthesis.py --tree-root "$WORK" \
  --out "$WORK/synthesis.json"
