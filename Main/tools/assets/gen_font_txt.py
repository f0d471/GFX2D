#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""gen_font_txt.py —— 把上游 BDF 点阵字体转换成打包工具读的 8x16 字库文本

输入是原样收进仓库的上游字体 Main/assets/font/spleen-8x16.bdf（Spleen 8x16，BSD-2-Clause，
许可证全文在同目录的 LICENSE-spleen.txt），输出是 Main/assets/font/ascii8x16.txt：
打包工具（pack_assets.py）、霓虹素材脚本（gen_stg_neon.py 的青色字库与大号数字）和 SW6 的
HUD 独立重画（check_sw6_log.py）都只读这份文本，所以换字体不必改它们。

取字规则：只取 ASCII 0x20..0x7E 共 95 个字形，缺一个就报错；每个字形按 BDF 的 BBX 放进
8x16 的格子，格子顶边是字体的上伸线（第 r 行 = FONT_ASCENT - (高 + y 偏移) + 字形内行号），
越出格子的像素报错，不裁剪。文本格式见输出文件头的注释。

用法：

    python3 Main/tools/assets/gen_font_txt.py                   生成 Main/assets/font/ascii8x16.txt（入库的那份）
    python3 Main/tools/assets/gen_font_txt.py --out <文件>      生成到别处
    python3 Main/tools/assets/gen_font_txt.py --check           重新生成，与入库的逐字节比
    python3 Main/tools/assets/gen_font_txt.py --check --inject  注错：改一个像素再比，必须见红（退出码 1）

输出只由 BDF 决定，没有随机数与时间戳。退出码 0 表示成功（--check 时表示一致）。
"""

import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
MAIN = os.path.dirname(os.path.dirname(HERE))       # Main/
FONT_DIR = os.path.join(MAIN, "assets", "font")
BDF = os.path.join(FONT_DIR, "spleen-8x16.bdf")
OUT = os.path.join(FONT_DIR, "ascii8x16.txt")

CW, CH = 8, 16
FIRST, LAST = 0x20, 0x7E


def die(msg):
    sys.stderr.write("gen_font_txt: " + msg + "\n")
    sys.exit(2)


def parse_bdf(path):
    """返回 (属性表, {码点: (w, h, xo, yo, [每行的整数], 每行位数)})。"""
    props, glyphs = {}, {}
    enc = bbx = rows = None
    comments = []
    with open(path, "r", encoding="latin-1") as f:
        for line in f:
            t = line.split()
            if not t:
                continue
            key = t[0]
            if rows is not None and key != "ENDCHAR":
                rows.append(t[0])
            elif key == "COMMENT":
                comments.append(line[len("COMMENT"):].strip())
            elif key in ("FONTBOUNDINGBOX", "FONT_ASCENT", "FONT_DESCENT", "COPYRIGHT", "FONT"):
                props[key] = line[len(key):].strip()
            elif key == "ENCODING":
                enc = int(t[1])
            elif key == "BBX":
                bbx = tuple(int(v) for v in t[1:5])
            elif key == "BITMAP":
                rows = []
            elif key == "ENDCHAR":
                if enc is not None and FIRST <= enc <= LAST:
                    w, h, xo, yo = bbx
                    if len(rows) != h:
                        die("码点 0x%02X 的位图有 %d 行，BBX 写的是 %d" % (enc, len(rows), h))
                    glyphs[enc] = (w, h, xo, yo, [int(r, 16) for r in rows], [len(r) * 4 for r in rows])
                enc = bbx = rows = None
    props["COMMENTS"] = comments
    return props, glyphs


def render(props, glyphs):
    """95 个字形各自的 16 行，每行 8 个字符 '.'/'#'。"""
    fb = [int(v) for v in props["FONTBOUNDINGBOX"].split()]
    ascent = int(props["FONT_ASCENT"])
    descent = int(props["FONT_DESCENT"])
    if fb[0] != CW or fb[1] != CH or ascent + descent != CH:
        die("字体框 %s、上伸 %d、下伸 %d，不是 8x16" % (props["FONTBOUNDINGBOX"], ascent, descent))
    out = []
    for c in range(FIRST, LAST + 1):
        if c not in glyphs:
            die("缺码点 0x%02X" % c)
        w, h, xo, yo, rows, nbits = glyphs[c]
        cell = [["."] * CW for _ in range(CH)]
        top = ascent - (h + yo)
        for r in range(h):
            for x in range(w):
                if (rows[r] >> (nbits[r] - 1 - x)) & 1:
                    y, xx = top + r, xo + x
                    if not (0 <= y < CH and 0 <= xx < CW):
                        die("码点 0x%02X 的像素 (%d, %d) 越出 8x16 格子" % (c, xx, y))
                    cell[y][xx] = "#"
        out.append(["".join(row) for row in cell])
    return out


def version_of(props):
    for c in props["COMMENTS"]:
        s = c.strip("/* ").strip()
        if s.startswith("Spleen "):
            return s
    return props.get("FONT", "?")


def to_text(props, cells):
    copyright_ = props.get("COPYRIGHT", "").strip('"')
    head = [
        "# ascii8x16.txt - 8x16 点阵 ASCII 字库（生成物，不要手改）",
        "# 由 Main/tools/assets/gen_font_txt.py 从同目录的 spleen-8x16.bdf 转换：",
        "# %s，%s，BSD-2-Clause，许可证全文见同目录的 LICENSE-spleen.txt。" % (version_of(props), copyright_),
        "# 编码：'.' = 0（背景），'#' = 1（前景）；每行 8 个字符，最左字符为最高位 bit7。",
        "# 每字 16 行，行序自上而下；上伸 %s 行、下伸 %s 行，基线在第 %s 行与第 %d 行之间（行号从 1 数起）。"
        % (props["FONT_ASCENT"], props["FONT_DESCENT"], props["FONT_ASCENT"], int(props["FONT_ASCENT"]) + 1),
        "# 字形顺序：ASCII 0x20（空格）.. 0x7E（'~'），共 95 个字形，数据行 95*16 = 1520 行。",
        "# 只有 '# ' 开头的注释行与空行解析时忽略；'########' 是合法像素行。",
        "",
    ]
    body = []
    for i, cell in enumerate(cells):
        c = FIRST + i
        body.append("# 0x%02X %s" % (c, "空格" if c == 0x20 else chr(c)))
        body.extend(cell)
    return "\n".join(head + body) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", default=OUT, help="输出文件（缺省 Main/assets/font/ascii8x16.txt）")
    ap.add_argument("--check", action="store_true", help="重新生成，与入库的逐字节比")
    ap.add_argument("--inject", action="store_true", help="注错：'0' 的一个像素取反（与 --check 一起用）")
    args = ap.parse_args()

    props, glyphs = parse_bdf(BDF)
    cells = render(props, glyphs)
    if args.inject:
        row = list(cells[0x30 - FIRST][8])
        row[3] = "." if row[3] == "#" else "#"
        cells[0x30 - FIRST][8] = "".join(row)
    text = to_text(props, cells)

    if args.check:
        with open(OUT, "r", encoding="utf-8", newline="") as f:
            old = f.read()
        if old == text:
            print("gen_font_txt: check 与入库的 ascii8x16.txt 逐字节相同（95 个字形）")
            return 0
        a, b = old.split("\n"), text.split("\n")
        k = next((i for i in range(min(len(a), len(b))) if a[i] != b[i]), min(len(a), len(b)))
        print("gen_font_txt: check 不同，第一处在第 %d 行" % (k + 1))
        return 1
    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    print("gen_font_txt: 写出 %s（95 个字形，%s）" % (os.path.relpath(args.out, MAIN), version_of(props)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
