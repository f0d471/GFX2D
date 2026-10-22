#!/usr/bin/env bash
# draw.io 预览/出图:vsdx 或 .drawio -> PNG(自动裁白边)。
# 原理:draw.io CLI 导 XML -> 把页面改成 4000x3000 绕开"按页导出裁掉页外内容" -> --size page 整页出图 -> PIL 裁白边。
# 已知偏差:vsdx 直转的文字可能溢出边框(axi_mux_guard、SPI Flash MB 等),报告成图前须先在
# draw.io GUI 里把图修好另存 .drawio,再喂给本脚本;直接喂 vsdx 只适合快速预览。
# 用法:bash vsdx_export.sh <文件.vsdx|文件.drawio> [输出目录]   输出目录缺省 ./_preview
# 依赖:Windows draw.io CLI(v32 测试过)、WSL python3 + PIL。可用 DRAWIO 指定安装位置。
set -euo pipefail
DRAWIO="${DRAWIO:-$(cygpath -u "${PROGRAMFILES:?}/draw.io/draw.io.exe")}"
SRC="$(cd "$(dirname "$0")" && pwd)"
to_mnt() { local w d; w="$(cygpath -w "$1")"; d="$(printf %s "${w:0:1}" | tr 'A-Z' 'a-z')"; echo "/mnt/$d/${w:3}" | tr '\\' '/'; }
in="$(ls "$1")"; name="$(basename "${in%.*}")"
out="${2:-$SRC/_preview}"; mkdir -p "$out"
tmp="$(mktemp -d)"
"$DRAWIO" -x -f xml -o "$tmp/$name.drawio" "$(cygpath -w "$in")" >/dev/null
sed -i 's/pageWidth="[0-9]*"/pageWidth="4000"/; s/pageHeight="[0-9]*"/pageHeight="3000"/' "$tmp/$name.drawio"
"$DRAWIO" -x -f png --width 4000 --size page -o "$tmp/big.png" "$(cygpath -w "$tmp/$name.drawio")" >/dev/null
bigmnt="$(to_mnt "$tmp/big.png")"; outmnt="$(to_mnt "$out/$name.png")"
wsl.exe -e bash -c "python3 -c \"
from PIL import Image, ImageChops
im = Image.open('$bigmnt').convert('RGB')
d = ImageChops.difference(im, Image.new('RGB', im.size, (255,255,255)))
b = d.getbbox(); p = 12
im.crop((max(0,b[0]-p), max(0,b[1]-p), min(im.size[0],b[2]+p), min(im.size[1],b[3]+p))).save('$outmnt')
\""
rm -rf "$tmp"
echo "ok $out/$name.png"
