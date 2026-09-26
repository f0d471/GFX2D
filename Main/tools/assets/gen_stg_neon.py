#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""gen_stg_neon.py —— 生成游戏的霓虹几何素材 PNG（SW7 2.7 节）

主题照 G2 定案与决策页效果图：底色 #02040A，四种弹色青 #35F2FF、品红 #FF4FD8、琥珀 #FFB23E、绿 #8DFF6A，
自机 #A8FBFF，火花 #FFC45A，HUD 标签的青色 #4FD8F2。光晕是径向渐变、弹芯是白心加色环，线框是折线。
图案全部按坐标算出来：没有随机数，不读字体文件（「万弹」两个字按笔画画成线段，HUD 的青色字库照
Main/assets/font/ascii8x16.txt 渲染），同一份脚本在同一个 Python 上给出同样的像素。

产物（都写进 Main/assets/neon/，入库）：

    bul16.png      64x16   16x16 圆弹，4 色各一格                     预乘，ALPHA
    bul8.png       32x8    8x8 小圆弹，4 色                            预乘，ALPHA
    rice20.png     320x80  20x20 米弹，4 色为行 x 16 个方向为列         预乘，ALPHA
    pshot.png      6x24    自机弹，青色竖光束，上亮下暗                预乘，ADD
    spark.png      32x40   8x8 火花：4 列是闪烁的 4 帧，5 行是 5 档亮度 预乘，ADD
    ships.png      128x96  32x32 线框：自机 1 帧、DART 4 帧、CROSS 4 帧  XRGB，色键 0
    boss.png       192x288 96x96：品红六边形与青色六芒星，6 帧转过 60 度  预乘，ALPHA
    stars.png      8x2     2x2 星点 4 种                               XRGB，色键 0
    title.png      240x90  MYRIAD 加「万弹」                            预乘，ALPHA
    digits.png     256x32  16x32 大号数字 0~9 与 '-'，由字库放大一倍    XRGB，色键 0
    icons.png      64x16   16x16：自机图标、炸弹菱形、判定点            XRGB，色键 0
    font_cyan.png  128x96  青色 8x16 字库                              XRGB，色键 0

预乘的图写直通 RGBA（打包工具按 manifest 的 premult 做预乘）；带色键的 XRGB 图写 RGBA，
alpha 只有 0 与 255，alpha 为 0 的像素打包时换成色键 0x00000000。

用法：

    python3 Main/tools/assets/gen_stg_neon.py                   生成到 Main/assets/neon/（入库的那份）
    python3 Main/tools/assets/gen_stg_neon.py --out <目录>      生成到别处（判据用：连跑两次比逐字节）
    python3 Main/tools/assets/gen_stg_neon.py --check           重新生成，与入库的逐像素比（尺寸、模式、每个像素）
    python3 Main/tools/assets/gen_stg_neon.py --check --inject  注错：改一个像素再比，必须见红（退出码 1）

--check 比像素而不比 PNG 文件的字节：PNG 的压缩流随 Pillow 与 zlib 的版本变，像素不变；
打包只看像素，素材包因此逐字节不变。退出码 0 表示成功（--check 时表示一致）。
"""

import argparse
import math
import os
import sys

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
MAIN = os.path.dirname(os.path.dirname(HERE))       # Main/
OUT_DIR = os.path.join(MAIN, "assets", "neon")
FONT_TXT = os.path.join(MAIN, "assets", "font", "ascii8x16.txt")

CYAN = (0x35, 0xF2, 0xFF)
MAGENTA = (0xFF, 0x4F, 0xD8)
AMBER = (0xFF, 0xB2, 0x3E)
GREEN = (0x8D, 0xFF, 0x6A)
BULLET_COLORS = (CYAN, MAGENTA, AMBER, GREEN)       # 颜色号 0~3（SW7 2.3 节）
PLAYER = (0xA8, 0xFB, 0xFF)
SPARK = (0xFF, 0xC4, 0x5A)
LABEL = (0x4F, 0xD8, 0xF2)
WHITE = (0xFF, 0xFF, 0xFF)


def clamp8(v):
    return 0 if v < 0 else 255 if v > 255 else int(v)


def mix(a, b, t):
    """a 与 b 按 t 混合（t = 0 为 a），逐通道四舍五入。"""
    return tuple(clamp8(math.floor(a[i] + (b[i] - a[i]) * t + 0.5)) for i in range(3))


def glow_pixel(e, color, core, ring, glow, glow_alpha):
    """径向剖面：e 是到中心的（归一化）距离。白心 -> 色环（不透明） -> 光晕（alpha 按平方衰减） -> 透明。"""
    if e <= core:
        return WHITE + (255,)
    if e <= ring:
        return mix(WHITE, color, (e - core) / (ring - core)) + (255,)
    if e < glow:
        t = (glow - e) / (glow - ring)
        return color + (clamp8(math.floor(glow_alpha * t * t + 0.5)),)
    return (0, 0, 0, 0)


# --- 弹 -----------------------------------------------------------------------

def round_atlas(cell, core, ring, glow, glow_alpha):
    """圆弹图集：4 色各一格。像素中心 (x + 0.5, y + 0.5) 到格子中心的距离。"""
    im = Image.new("RGBA", (cell * 4, cell))
    px = im.load()
    c = cell / 2.0
    for k, color in enumerate(BULLET_COLORS):
        for y in range(cell):
            for x in range(cell):
                d = math.sqrt((x + 0.5 - c) ** 2 + (y + 0.5 - c) ** 2)
                px[k * cell + x, y] = glow_pixel(d, color, core, ring, glow, glow_alpha)
    return im


def rice_atlas(cell=20, dirs=16):
    """米弹：4 色为行、16 个方向为列。第 k 列朝向 k x 22.5 度（从 +x 转向 +y，与 BAM 同向）。
    椭圆：沿朝向半轴 7.5 像素、横向 3.2 像素，按归一化距离取径向剖面。"""
    im = Image.new("RGBA", (cell * dirs, cell * 4))
    px = im.load()
    c = cell / 2.0
    for row, color in enumerate(BULLET_COLORS):
        for k in range(dirs):
            th = 2.0 * math.pi * k / dirs
            cs, sn = math.cos(th), math.sin(th)
            for y in range(cell):
                for x in range(cell):
                    dx, dy = x + 0.5 - c, y + 0.5 - c
                    u = dx * cs + dy * sn
                    v = -dx * sn + dy * cs
                    e = math.sqrt((u / 7.5) ** 2 + (v / 3.2) ** 2)
                    px[k * cell + x, row * cell + y] = glow_pixel(e, color, 0.35, 0.8, 1.25, 150)
    return im


def pshot():
    """自机弹：6x24 竖光束。中间两列白青、两侧青、最外两列淡；上端（弹头）最亮，往下渐暗到四分之一。"""
    w, h = 6, 24
    im = Image.new("RGBA", (w, h))
    px = im.load()
    cols = ((CYAN, 90), (CYAN, 220), ((0xD8, 0xFF, 0xFF), 255), ((0xD8, 0xFF, 0xFF), 255), (CYAN, 220), (CYAN, 90))
    for y in range(h):
        m = 1.0 if y < 6 else 1.0 - 0.75 * (y - 6) / (h - 1 - 6)
        for x in range(w):
            color, a = cols[x]
            px[x, y] = color + (clamp8(math.floor(a * m + 0.5)),)
    return im


def spark():
    """火花：8x8 一格，4 列是闪烁的 4 帧（十字、斜十字、点、短十字），5 行是亮度 1/5~5/5。
    画图时帧号取粒子的剩余寿命：列 = 寿命 & 3，行 = 寿命 >> 2，所以寿命递减时闪烁并变暗。"""
    cell = 8
    im = Image.new("RGBA", (cell * 4, cell * 5))
    px = im.load()
    shapes = []
    plus = [(0, 0), (1, 0), (-1, 0), (0, 1), (0, -1), (2, 0), (-2, 0), (0, 2), (0, -2), (3, 0), (-3, 0), (0, 3), (0, -3)]
    cross = [(0, 0), (1, 1), (-1, -1), (1, -1), (-1, 1), (2, 2), (-2, -2), (2, -2), (-2, 2)]
    dot = [(0, 0), (-1, 0), (0, -1), (-1, -1), (1, 0), (0, 1)]
    short = [(0, 0), (1, 0), (-1, 0), (0, 1), (0, -1), (2, 0), (-2, 0), (0, 2), (0, -2)]
    shapes = (plus, cross, dot, short)
    for row in range(5):
        level = (row + 1) / 5.0
        for f, shape in enumerate(shapes):
            for (ox, oy) in shape:
                r = max(abs(ox), abs(oy))
                a = level * (255 if r == 0 else 200 if r == 1 else 130 if r == 2 else 70)
                color = WHITE if r == 0 else SPARK
                px[f * cell + 4 + ox, row * cell + 4 + oy] = color + (clamp8(math.floor(a + 0.5)),)
    return im


# --- 线框 ---------------------------------------------------------------------

def poly(draw, pts, color, width=1, alpha=255, close=False):
    seq = list(pts) + ([pts[0]] if close else [])
    draw.line(seq, fill=color + (alpha,), width=width)


def ships():
    """32x32 线框，4 列 x 3 行：第 0 格自机；第 4~7 格 DART 4 帧；第 8~11 格 CROSS 4 帧；其余空。
    alpha 只有 0 与 255（色键图）。"""
    cell = 32
    im = Image.new("RGBA", (cell * 4, cell * 3), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    # 自机：箭头形，鼻尖朝上
    poly(d, [(16, 2), (4, 28), (16, 21), (28, 28)], PLAYER, close=True)
    poly(d, [(16, 8), (16, 18)], (0xE8, 0xFF, 0xFF))
    poly(d, [(10, 24), (7, 29)], PLAYER)
    poly(d, [(22, 24), (25, 29)], PLAYER)
    # DART：朝下的三角，里面一道折线逐帧下移
    for k in range(4):
        ox, oy = k * cell, cell
        poly(d, [(ox + 3, oy + 4), (ox + 28, oy + 4), (ox + 16, oy + 29)], MAGENTA, close=True)
        poly(d, [(ox + 9, oy + 8 + 2 * k), (ox + 16, oy + 15 + 2 * k), (ox + 23, oy + 8 + 2 * k)],
             (0xFF, 0x9F, 0xE8))
    # CROSS：十字外框，里面一个方块逐帧转 22.5 度
    for k in range(4):
        ox, oy = k * cell, 2 * cell
        outline = [(11, 2), (21, 2), (21, 11), (30, 11), (30, 21), (21, 21), (21, 30), (11, 30),
                   (11, 21), (2, 21), (2, 11), (11, 11)]
        poly(d, [(ox + x, oy + y) for (x, y) in outline], AMBER, close=True)
        th = math.pi / 8.0 * k
        sq = []
        for j in range(4):
            a = th + math.pi / 2.0 * j
            sq.append((ox + int(math.floor(16 + 6 * math.cos(a) + 0.5)),
                       oy + int(math.floor(16 + 6 * math.sin(a) + 0.5))))
        poly(d, sq, CYAN, close=True)
    return im


def boss():
    """96x96 一格，2 列 x 3 行共 6 帧：品红六边形（半径 44）顺转、青色六芒星（半径 30）逆转，每帧 10 度；
    六重对称，6 帧转过 60 度首尾相接。先画宽的淡线当光晕，再画细的亮线；中心一个白心品红光点。
    帧数是按素材包的总长定的（SW7 4.20 节）：12 帧时包总长超过 SW5 坏包判据 S5f 的构造所能覆盖的上限。"""
    cell = 96
    im = Image.new("RGBA", (cell * 2, cell * 3), (0, 0, 0, 0))
    px = im.load()
    for i in range(6):
        ox, oy = (i % 2) * cell, (i // 2) * cell
        # 中心光点
        for y in range(cell):
            for x in range(cell):
                dd = math.sqrt((x + 0.5 - 48) ** 2 + (y + 0.5 - 48) ** 2)
                if dd < 11:
                    px[ox + x, oy + y] = glow_pixel(dd, MAGENTA, 3.0, 6.0, 11.0, 120)
        d = ImageDraw.Draw(im)
        phi = math.radians(10.0 * i)

        def vert(r, a):
            return (ox + math.floor(48 + r * math.cos(a) + 0.5), oy + math.floor(48 + r * math.sin(a) + 0.5))

        hexa = [vert(44, phi + math.pi / 3.0 * k) for k in range(6)]
        tri1 = [vert(30, -phi + math.pi / 6.0 + 2.0 * math.pi / 3.0 * k) for k in range(3)]
        tri2 = [vert(30, -phi + math.pi / 2.0 + 2.0 * math.pi / 3.0 * k) for k in range(3)]
        poly(d, hexa, MAGENTA, width=5, alpha=70, close=True)
        poly(d, tri1, CYAN, width=4, alpha=60, close=True)
        poly(d, tri2, CYAN, width=4, alpha=60, close=True)
        poly(d, hexa, MAGENTA, width=2, alpha=255, close=True)
        poly(d, tri1, CYAN, width=1, alpha=255, close=True)
        poly(d, tri2, CYAN, width=1, alpha=255, close=True)
    return im


def stars():
    """2x2 星点 4 种：2x2 亮、2x2 暗、1x1 亮、1x1 暗（1x1 的只点左上角，其余是色键）。"""
    im = Image.new("RGBA", (8, 2), (0, 0, 0, 0))
    px = im.load()
    bright, dim = (0xB8, 0xF4, 0xFF), (0x3C, 0x70, 0x86)
    for (x, y) in ((0, 0), (1, 0), (0, 1), (1, 1)):
        px[x, y] = bright + (255,)
        px[2 + x, y] = dim + (255,)
    px[4, 0] = bright + (255,)
    px[6, 0] = dim + (255,)
    return im


# --- 标题 ---------------------------------------------------------------------

# 字母的笔画：5 x 7 的格子（x 0~4，y 0~6），每笔一条折线
LETTERS = {
    "M": [[(0, 6), (0, 0), (2, 3), (4, 0), (4, 6)]],
    "Y": [[(0, 0), (2, 3), (4, 0)], [(2, 3), (2, 6)]],
    "R": [[(0, 6), (0, 0), (3, 0), (4, 1), (4, 2), (3, 3), (0, 3)], [(2, 3), (4, 6)]],
    "I": [[(1, 0), (3, 0)], [(2, 0), (2, 6)], [(1, 6), (3, 6)]],
    "A": [[(0, 6), (2, 0), (4, 6)], [(1, 4), (3, 4)]],
    "D": [[(0, 0), (0, 6), (3, 6), (4, 5), (4, 1), (3, 0), (0, 0)]],
}

# 「万弹」的笔画：28 x 28 的格子，照楷书的笔顺取直线段
HANZI = [
    # 万：横、撇（先竖后撇）、横折钩
    [[(1, 3), (27, 3)], [(10, 3), (10, 14), (2, 27)], [(10, 13), (25, 13), (25, 24), (20, 27)]],
    # 弹：弓（横折、横、竖折、竖折钩），单（两点、日字框、中竖、底横）
    [[(1, 3), (9, 3), (9, 9), (2, 9), (2, 15), (10, 15), (10, 26), (6, 27)],
     [(13, 1), (15, 4)], [(25, 1), (23, 4)],
     [(13, 6), (27, 6), (27, 16), (13, 16), (13, 6)], [(13, 11), (27, 11)],
     [(20, 6), (20, 27)], [(12, 21), (28, 21)]],
]


def neon_strokes(d, strokes, color, core):
    """霓虹线：宽 5 的淡光晕、宽 3 的色线、宽 1 的白心（核心色）。"""
    for s in strokes:
        d.line(s, fill=color + (70,), width=5, joint="curve")
    for s in strokes:
        d.line(s, fill=color + (255,), width=3, joint="curve")
    for s in strokes:
        d.line(s, fill=core + (255,), width=1)


def title():
    """240x90：MYRIAD 六个字母（每个 28x36，间距 8）在上，「万弹」两个字（各 28x28）在下居中。"""
    im = Image.new("RGBA", (240, 90), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    x0, y0 = 16, 8
    strokes = []
    for n, ch in enumerate("MYRIAD"):
        lx = x0 + n * 36
        for s in LETTERS[ch]:
            strokes.append([(lx + 7 * gx, y0 + 6 * gy) for (gx, gy) in s])
    neon_strokes(d, strokes, CYAN, (0xE0, 0xFF, 0xFF))
    strokes = []
    for n, ch in enumerate(HANZI):
        hx, hy = 120 - 34 + n * 40, 54
        for s in ch:
            strokes.append([(hx + gx, hy + gy) for (gx, gy) in s])
    neon_strokes(d, strokes, MAGENTA, (0xFF, 0xD8, 0xF4))
    return im


# --- 字库派生 -----------------------------------------------------------------

def font_rows():
    """照打包工具的规则读 8x16 点阵字库：# 后跟空白是注释，空行忽略；95 个字形 x 16 行。"""
    with open(FONT_TXT, "r", encoding="utf-8") as f:
        lines = f.read().splitlines()
    rows = []
    for line in lines:
        s = line.strip()
        if s == "" or s == "#" or (s.startswith("#") and len(s) > 1 and s[1].isspace()):
            continue
        rows.append(s)
    if len(rows) != 95 * 16:
        raise SystemExit("字库 %s 有 %d 行数据，应为 1520" % (FONT_TXT, len(rows)))
    return rows


def glyph(rows, ch):
    g = ord(ch) - 0x20
    return rows[g * 16:(g + 1) * 16]


def font_cyan(rows):
    """青色字库：与 SW5 的白色字库同一个网格（16 列 x 6 行，字形 0x20~0x7E），字形像素为 #4FD8F2，其余是色键。"""
    im = Image.new("RGBA", (128, 96), (0, 0, 0, 0))
    px = im.load()
    for g in range(95):
        gx, gy = (g % 16) * 8, (g // 16) * 16
        for r in range(16):
            line = rows[g * 16 + r]
            for c in range(8):
                if line[c] == "#":
                    px[gx + c, gy + r] = LABEL + (255,)
    return im


def digits(rows):
    """大号数字：'0'~'9' 与 '-' 各一格 16x32，字库的每个像素放大成 2x2，白色；其余格与底是色键。"""
    im = Image.new("RGBA", (256, 32), (0, 0, 0, 0))
    px = im.load()
    for n, ch in enumerate("0123456789-"):
        g = glyph(rows, ch)
        for r in range(16):
            for c in range(8):
                if g[r][c] == "#":
                    for (ox, oy) in ((0, 0), (1, 0), (0, 1), (1, 1)):
                        px[n * 16 + 2 * c + ox, 2 * r + oy] = WHITE + (255,)
    return im


def icons():
    """16x16 图标 4 格：自机图标、炸弹菱形（琥珀）、判定点（白心半径 2.6、品红环 3.6~5.0）、空。"""
    im = Image.new("RGBA", (64, 16), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    poly(d, [(8, 1), (2, 14), (8, 10), (14, 14)], PLAYER, close=True)
    poly(d, [(24, 1), (31, 8), (24, 15), (17, 8)], AMBER, close=True)
    poly(d, [(24, 5), (27, 8), (24, 11), (21, 8)], AMBER, close=True)
    px = im.load()
    for y in range(16):
        for x in range(16):
            dd = math.sqrt((x + 0.5 - 8) ** 2 + (y + 0.5 - 8) ** 2)
            if dd <= 2.6:
                px[32 + x, y] = WHITE + (255,)
            elif 3.6 <= dd <= 5.0:
                px[32 + x, y] = MAGENTA + (255,)
    return im


def build():
    """按文件名给出全部图（有序）。"""
    rows = font_rows()
    return [
        ("bul16.png", round_atlas(16, 2.5, 4.5, 7.5, 150)),
        ("bul8.png", round_atlas(8, 1.0, 2.2, 3.9, 140)),
        ("rice20.png", rice_atlas()),
        ("pshot.png", pshot()),
        ("spark.png", spark()),
        ("ships.png", ships()),
        ("boss.png", boss()),
        ("stars.png", stars()),
        ("title.png", title()),
        ("digits.png", digits(rows)),
        ("icons.png", icons()),
        ("font_cyan.png", font_cyan(rows)),
    ]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", default=OUT_DIR, help="输出目录（缺省 Main/assets/neon/）")
    ap.add_argument("--check", action="store_true", help="重新生成，与 Main/assets/neon/ 里的逐像素比")
    ap.add_argument("--inject", action="store_true", help="注错：第一张图改一个像素（与 --check 一起用）")
    args = ap.parse_args()

    imgs = build()
    if args.inject:
        name, im = imgs[0]
        p = im.getpixel((8, 8))
        im.putpixel((8, 8), (p[0] ^ 1,) + tuple(p[1:]))

    if args.check:
        bad = 0
        for name, im in imgs:
            path = os.path.join(OUT_DIR, name)
            if not os.path.exists(path):
                print("缺 %s" % name)
                bad += 1
                continue
            have = Image.open(path)
            if have.size != im.size or have.mode != im.mode or have.tobytes() != im.tobytes():
                print("%s 与重新生成的不同（尺寸 %s/%s，模式 %s/%s）"
                      % (name, have.size, im.size, have.mode, im.mode))
                bad += 1
        print("check %d 张，不同 %d 张" % (len(imgs), bad))
        return 1 if bad else 0

    os.makedirs(args.out, exist_ok=True)
    for name, im in imgs:
        im.save(os.path.join(args.out, name), format="PNG")
        print("%-14s %4dx%-4d %s" % (name, im.size[0], im.size[1], im.mode))
    return 0


if __name__ == "__main__":
    sys.exit(main())
