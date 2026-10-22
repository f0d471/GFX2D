#!/usr/bin/env bash
# 批量:vsdx -> raw XML -> fix.py 打补丁 -> 页面固定 5000x3500 -> 整页 3 倍渲染 -> PIL 裁白边。
# 不信任导入 XML 里的坐标边界(实测会低估导致裁切),用超大页面+整页出图兜底;
# autocrop 若报 WARN 说明 5000x3500 仍不够,需单独放大页面重跑。
# 用法:bash render_all.sh   (工作/输出在当前用户 LocalAppData/Temp/drawio_work/out)
set -euo pipefail
DRAWIO="${DRAWIO:-$(cygpath -u "${PROGRAMFILES:?}/draw.io/draw.io.exe")}"
VISIO="$(cd "$(dirname "$0")" && pwd)"
W="$(cygpath -u "${LOCALAPPDATA:?}/Temp/drawio_work")"
WM="$(cygpath -m "$W")"
WM="/mnt/$(printf %s "${WM:0:1}" | tr 'A-Z' 'a-z')/${WM:3}"
mkdir -p "$W/out"
cd "$VISIO"
wsl.exe -e bash -c "python3 $WM/fix.py $WM $WM"
for f in *.vsdx; do
  n="${f%.vsdx}"
  sed -i 's/pageWidth="[0-9]*"/pageWidth="5000"/; s/pageHeight="[0-9]*"/pageHeight="3500"/' "$W/$n.fixed.drawio"
  "$DRAWIO" -x -f png --size page --width 15000 -o "$W/out/$n.big.png" "$(cygpath -w "$W/$n.fixed.drawio")" >/dev/null
  final="$(wsl.exe -e bash -c "python3 $WM/autocrop.py $WM/out/$n.big.png $WM/out/$n.png 48")"
  echo "$n: $final"
done
