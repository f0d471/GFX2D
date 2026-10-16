#!/usr/bin/env bash
# 生成七个代表档的构型源树并逐个跑整机四步综合，汇总资源与时序。
# Windows + Efinity 命令行；用法（仓根）：
#   bash Main/sim/engine/features/synth_presets.sh [输出目录]
# 调用前按 doc/开发环境.md 设置 EFINITY_HOME；WSL 可另设 EFINITY_PYTHON。
set -u
ROOT=$(cd "$(dirname "$0")/../../../.." && pwd)
WORK=$(realpath -m -- "${1:-$ROOT/Main/sim/build/feature-trees}")
cd "$ROOT"

if [ -z "${EFINITY_HOME:-}" ]; then
  echo 'INCOMPLETE EFINITY_HOME 未设置，见 Main/doc/开发环境.md'
  exit 3
fi
PY=${EFINITY_PYTHON:-$EFINITY_HOME/python311/bin/python.exe}
if [ ! -f "$PY" ]; then
  echo 'INCOMPLETE 未找到 Efinity Python；可用 EFINITY_PYTHON 指定可执行文件'
  exit 3
fi
export PYTHONHOME=${PYTHONHOME:-$EFINITY_HOME/python311}
case "$WORK/" in
  "$ROOT/Main/sim/build/"*) ;;
  *) echo 'BUILD-FAIL 输出目录必须在 Main/sim/build 内'; exit 1 ;;
esac

for preset in B0 B1 B2 B3 B4 B5 B6; do
  if [ -L "$WORK/$preset" ] || [ "$(realpath -m -- "$WORK/$preset")" != "$WORK/$preset" ]; then
    echo "BUILD-FAIL 构型目录不能是链接：$preset"; exit 1
  fi
  "$PY" Main/tools/check/feature_build.py --config "$preset" --out-dir "$WORK/$preset" \
    || { echo "BUILD-FAIL $preset"; continue; }
done

# 逐个跑；某个构型超器件上限时 pnr 会失败，脚本对该档记 FAIL 并继续。
# 本进程需要 PYTHONHOME 才能启动；厂商子进程的环境由 feature_synthesis.py 自行摘除。
"$PY" Main/tools/check/feature_synthesis.py --tree-root "$WORK" \
  --out "$WORK/synthesis.json"
