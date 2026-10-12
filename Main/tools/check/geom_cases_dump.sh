#!/usr/bin/env bash
# 把十一个整帧用例导出成几何层模型能吃的记录。
#
# 命令表由 sw/lib/gfx/cases.c + list.c 在运行时构造，这里直接链接它们，
# 而不是在 Python 里照抄一遍——照抄会与真源分叉。
#
# 用法：
#   bash tools/check/geom_cases_dump.sh [输出文件]       默认 Main/build/geom_cases.txt
# 输出文件写相对路径时按调用者的当前目录算（下面先转成绝对路径再 cd），
# 不按 Main/ 算：sim/engine/geom/run.sh 传的 build/cases.txt 指的是它自己的 build/。
# 之后：
#   python3 tools/check/geom_model.py --cases <输出文件>
set -eu
OUT="${1:-}"
case "$OUT" in
    "") ;;
    /*) ;;
    *)  OUT="$PWD/$OUT" ;;
esac
cd "$(dirname "$0")/../.."          # 到 Main/
OUT="${OUT:-$PWD/build/geom_cases.txt}"
BIN="${TMPDIR:-/tmp}/geom_cases_dump.$$"

mkdir -p "$(dirname "$OUT")"
gcc -O2 -Wall -Isw/lib -o "$BIN" \
    tools/check/geom_cases_dump.c \
    sw/lib/gfx/cases.c sw/lib/gfx/list.c sw/lib/gfx/strip.c sw/lib/gfx/strip_plan.c sw/lib/gfx/check.c sw/lib/core/format.c

"$BIN" > "$OUT"
rm -f "$BIN"
echo "写出 $OUT"
