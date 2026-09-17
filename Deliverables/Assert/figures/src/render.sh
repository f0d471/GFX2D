#!/usr/bin/env bash
# 把 figures/src/<名字>.html 渲染成 figures/<名字>.png（2 倍像素）。
# 用法：bash render.sh [名字 ...]，不带参数时渲染 src 下全部非下划线开头的 html。
# 图的尺寸写在 html 的 <meta name="fig-size" content="宽x高">。
# 渲染后检查 png 的修改时间是否晚于开始时间：Edge 参数出错时不会报错，只会留下旧图。
set -euo pipefail
EDGE="/c/Program Files (x86)/Microsoft/Edge/Application/msedge.exe"
SRC="$(cd "$(dirname "$0")" && pwd)"
OUT="$(dirname "$SRC")"
names=("$@")
if [ ${#names[@]} -eq 0 ]; then
  for f in "$SRC"/*.html; do b="$(basename "$f" .html)"; [[ "$b" == _* ]] || names+=("$b"); done
fi
fail=0
for n in "${names[@]}"; do
  html="$SRC/$n.html"; png="$OUT/$n.png"
  size="$(grep -o 'name="fig-size" content="[0-9]*x[0-9]*"' "$html" | grep -o '[0-9]*x[0-9]*')"
  w="${size%x*}"; h="${size#*x}"
  stamp="$(date +%s)"
  "$EDGE" --headless=new --disable-gpu --hide-scrollbars --force-device-scale-factor=2 \
    --virtual-time-budget=4000 --window-size="$w,$h" \
    --screenshot="$(cygpath -w "$png")" "file:///$(cygpath -m "$html")" >/dev/null 2>&1 || true
  if [ -f "$png" ] && [ "$(stat -c %Y "$png")" -ge "$stamp" ]; then echo "ok   $n (${w}x${h})"
  else echo "FAIL $n"; fail=1; fi
done
exit $fail
