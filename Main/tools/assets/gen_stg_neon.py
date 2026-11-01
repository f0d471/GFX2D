#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""gen_stg_neon.py —— 生成游戏的霓虹几何素材 PNG（sw 第 06 章 3.12 节）

主题照 G2 定案与决策页效果图：底色 #02040A，四种弹色青 #35F2FF、品红 #FF4FD8、琥珀 #FFB23E、绿 #8DFF6A，
自机 #A8FBFF，火花 #FFC45A，HUD 标签的青色 #4FD8F2。光晕是径向渐变、弹芯是白心加色环，线框是折线。
图案全部按坐标算出来：没有随机数，不读字体文件（标题的 MYRIAD 按笔画画成线段，HUD 的青色字库与
大号数字照 Main/assets/font/ascii8x16.txt 渲染），同一份脚本在同一个 Python 上给出同样的像素。

产物（都写进 Main/assets/neon/，入库）：

    bul16.png      64x16   16x16 圆弹，4 色各一格                     预乘，ALPHA
    bul8.png       32x8    8x8 小圆弹，4 色                            预乘，ALPHA
    rice20.png     320x80  20x20 米弹，4 色为行 x 16 个方向为列         预乘，ALPHA
    pshot.png      6x24    自机弹，青色竖光束，上亮下暗                预乘，ADD
    spark.png      32x40   8x8 火花：4 列是闪烁的 4 帧，5 行是 5 档亮度 预乘，ADD
    ships.png      128x96  32x32 线框：自机 1 帧、DART 4 帧、CROSS 4 帧  XRGB，色键 0
    boss.png       192x288 96x96：品红六边形与青色六芒星，6 帧转过 60 度  预乘，ALPHA
    stars.png      8x2     2x2 星点 4 种                               XRGB，色键 0
    title.png      240x90  MYRIAD，垂直居中                             预乘，ALPHA
    digits.png     256x32  16x32 大号数字 0~9 与 '-'，由字库放大一倍    XRGB，色键 0
    icons.png      64x16   16x16：自机图标、炸弹菱形、判定点            XRGB，色键 0
    font_cyan.png  128x96  青色 8x16 字库                              XRGB，色键 0
    font_amber.png 128x96  琥珀 8x16 字库（R4 HUD 数值）                XRGB，色键 0
    bg_layers.png  512x608 背景合图：天空 400x480 + 远景带 512x56 + 城市带 512x72   XRGB，色键 0

其中 KENNEY_OWNED 的 8 张，入库的是 apply_kenney_art.py 在本脚本基线上叠过 Kenney 贴图的版本
（2026-10-31 定案），Kenney 原图不入库，本脚本生成不出来。所以写回入库目录时默认跳过这 8 张，
--check 也不比；要重做基线必须加 --base，随后马上重跑 apply_kenney_art.py。
（e05fd33 为加 font_amber.png 全量重跑过一次，Kenney 层被静默打回基线，到 11-01 才发现。）

预乘的图写直通 RGBA（打包工具按 manifest 的 premult 做预乘）；带色键的 XRGB 图写 RGBA，
alpha 只有 0 与 255，alpha 为 0 的像素打包时换成色键 0x00000000。

用法：

    python3 Main/tools/assets/gen_stg_neon.py                   生成到 Main/assets/neon/（入库的那份；跳过 Kenney 叠加的 8 张）
    python3 Main/tools/assets/gen_stg_neon.py --base            同上，但 8 张也写回基线（之后必须重跑 apply_kenney_art.py）
    python3 Main/tools/assets/gen_stg_neon.py --out <目录>      全部 14 张生成到别处（判据用：连跑两次比逐字节）
    python3 Main/tools/assets/gen_stg_neon.py --check           重新生成，与入库的逐像素比（尺寸、模式、每个像素；不比那 8 张）
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

# 入库的是 apply_kenney_art.py 叠加版的 8 张：写回入库目录默认跳过，--check 不比（见文件头）
KENNEY_OWNED = frozenset((
    "bul16.png", "bul8.png", "rice20.png", "pshot.png",
    "ships.png", "boss.png", "icons.png", "bg_layers.png",
))

CYAN = (0x35, 0xF2, 0xFF)
MAGENTA = (0xFF, 0x4F, 0xD8)
AMBER = (0xFF, 0xB2, 0x3E)
GREEN = (0x8D, 0xFF, 0x6A)
BULLET_COLORS = (CYAN, MAGENTA, AMBER, GREEN)       # 颜色号 0~3（sw 第 06 章 3.7 节）
PLAYER = (0xA8, 0xFB, 0xFF)
SPARK = (0xFF, 0xC4, 0x5A)
LABEL = (0x4F, 0xD8, 0xF2)
WHITE = (0xFF, 0xFF, 0xFF)
# 填充底色与背景过渡带（升级调色板）：填充不为纯黑（纯黑是色键）
NAVY = (0x08, 0x14, 0x30)
NAVY_M = (0x2A, 0x0A, 0x24)         # DART 的暗品红填充
NAVY_A = (0x2E, 0x1E, 0x08)         # CROSS 的暗琥珀填充
SKY_TOP = (0x02, 0x04, 0x0A)
SKY_MID = (0x05, 0x0F, 0x22)
SKY_BOT = (0x0B, 0x22, 0x3E)
FAR_FILL = (0x0A, 0x1C, 0x33)
FAR_EDGE = (0x16, 0x44, 0x66)
CITY_FILL = (0x06, 0x10, 0x24)


def clamp8(v):
    return 0 if v < 0 else 255 if v > 255 else int(v)


def mix(a, b, t):
    """a 与 b 按 t 混合（t = 0 为 a），逐通道四舍五入。"""
    return tuple(clamp8(math.floor(a[i] + (b[i] - a[i]) * t + 0.5)) for i in range(3))


def glow_pixel(e, color, core, ring, glow, glow_alpha, glow2=0.0, alpha2=0):
    """径向剖面：e 是到中心的（归一化）距离。
    白心 -> 色环（不透明） -> 内光晕（亮，平方衰减） -> 外光晕（淡，平方衰减） -> 透明。
    glow2 为 0 时只有一层光晕（与旧口径一致）。"""
    if e <= core:
        return WHITE + (255,)
    if e <= ring:
        return mix(WHITE, color, (e - core) / (ring - core)) + (255,)
    if glow2 > 0.0 and e >= glow2:
        return (0, 0, 0, 0)
    if e < glow or (glow2 > 0.0 and e < glow2):
        outer = glow2 if glow2 > 0.0 else glow
        t = (outer - e) / (outer - ring)
        if e < glow:
            t = (glow - e) / (glow - ring)
            return color + (clamp8(math.floor(glow_alpha * t * t + 0.5)),)
        return color + (clamp8(math.floor(alpha2 * t * t + 0.5)),)
    return (0, 0, 0, 0)


# --- 弹 -----------------------------------------------------------------------

def round_atlas(cell, core, ring, glow, glow_alpha, glow2=0.0, alpha2=0):
    """圆弹图集：4 色各一格。像素中心 (x + 0.5, y + 0.5) 到格子中心的距离。"""
    im = Image.new("RGBA", (cell * 4, cell))
    px = im.load()
    c = cell / 2.0
    for k, color in enumerate(BULLET_COLORS):
        for y in range(cell):
            for x in range(cell):
                d = math.sqrt((x + 0.5 - c) ** 2 + (y + 0.5 - c) ** 2)
                px[k * cell + x, y] = glow_pixel(d, color, core, ring, glow, glow_alpha, glow2, alpha2)
    return im


def rice_atlas(cell=20, dirs=16):
    """米弹：4 色为行、16 个方向为列。第 k 列朝向 k x 22.5 度（从 +x 转向 +y，与 BAM 同向）。
    椭圆：沿朝向半轴 7.0 像素、横向 3.0 像素；尾部（u < 0）拉伸到 10.5 像素形成彗尾，
    按归一化距离取径向剖面。"""
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
                    uu = u / 10.5 if u < 0 else u / 7.0
                    e = math.sqrt((uu) ** 2 + (v / 3.0) ** 2)
                    px[k * cell + x, row * cell + y] = glow_pixel(e, color, 0.32, 0.75, 1.05, 170, 1.35, 80)
    return im


def pshot():
    """自机弹：6x24 竖光束。中间两列白青、两侧青、最外两列淡；上端（弹头）最亮，
    往下渐暗到三成；弹头两行整列提亮做冲击帽。"""
    w, h = 6, 24
    im = Image.new("RGBA", (w, h))
    px = im.load()
    cols = ((CYAN, 90), (CYAN, 230), ((0xE4, 0xFF, 0xFF), 255), ((0xE4, 0xFF, 0xFF), 255), (CYAN, 230), (CYAN, 90))
    for y in range(h):
        m = 1.0 if y < 5 else 1.0 - 0.70 * (y - 5) / (h - 1 - 5)
        if y < 2:
            m = 1.0
        for x in range(w):
            color, a = cols[x]
            px[x, y] = color + (clamp8(math.floor(a * m + 0.5)),)
    return im


def spark():
    """火花：8x8 一格，4 列是 4 帧（爆：大十字加斜、散：外圈点、淡：近点、灭：中心残点），
    5 行是亮度 1/5~5/5。画图时帧号取粒子的剩余寿命：列 = 寿命 & 3，行 = 寿命 >> 2，
    所以寿命递减时走「爆-散-淡-灭」并变暗。"""
    cell = 8
    im = Image.new("RGBA", (cell * 4, cell * 5))
    px = im.load()

    def put(f, ox, oy, r, level):
        a = level * (255 if r == 0 else 210 if r == 1 else 140 if r == 2 else 80)
        color = WHITE if r == 0 else SPARK
        px[f * cell + 4 + ox, row * cell + 4 + oy] = color + (clamp8(math.floor(a + 0.5)),)

    burst = [(0, 0), (1, 0), (-1, 0), (0, 1), (0, -1), (2, 0), (-2, 0), (0, 2), (0, -2),
             (3, 0), (-3, 0), (0, 3), (0, -3), (1, 1), (-1, -1), (1, -1), (-1, 1)]
    scatter = [(3, 0), (-3, 0), (0, 3), (0, -3), (2, 2), (-2, -2), (2, -2), (-2, 2), (1, 0), (-1, 0), (0, 1), (0, -1)]
    fading = [(2, 0), (-2, 0), (0, 2), (0, -2), (1, 1), (-1, -1), (1, -1), (-1, 1)]
    dying = [(0, 0)]
    shapes = (burst, scatter, fading, dying)
    for row in range(5):
        level = (row + 1) / 5.0
        for f, shape in enumerate(shapes):
            for (ox, oy) in shape:
                r = max(abs(ox), abs(oy))
                put(f, ox, oy, r, level)
    return im


# --- 线框 ---------------------------------------------------------------------

def poly(draw, pts, color, width=1, alpha=255, close=False):
    seq = list(pts) + ([pts[0]] if close else [])
    draw.line(seq, fill=color + (alpha,), width=width)


def ships():
    """32x32，4 列 x 3 行：第 0 格自机；第 4~7 格 DART 4 帧；第 8~11 格 CROSS 4 帧；其余空。
    升级：暗色填充 + 亮色描边（剪影可读），alpha 只有 0 与 255（色键图，填充不得为纯黑）。"""
    cell = 32
    im = Image.new("RGBA", (cell * 4, cell * 3), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    # 自机：箭头形，鼻尖朝上；暗填充、亮描边、白心座舱与引擎光点
    body = [(16, 2), (4, 28), (16, 21), (28, 28)]
    d.polygon(body, fill=NAVY + (255,))
    poly(d, body, PLAYER, close=True)
    poly(d, [(16, 8), (16, 18)], (0xE8, 0xFF, 0xFF))
    poly(d, [(13, 13), (19, 13)], (0xE8, 0xFF, 0xFF))
    poly(d, [(14, 24), (18, 24)], CYAN)
    poly(d, [(10, 24), (7, 29)], PLAYER)
    poly(d, [(22, 24), (25, 29)], PLAYER)
    # DART：朝下的三角，暗品红填充 + 亮缘，里面一道折线逐帧下移，尾部引擎点
    for k in range(4):
        ox, oy = k * cell, cell
        tri = [(ox + 3, oy + 4), (ox + 28, oy + 4), (ox + 16, oy + 29)]
        d.polygon(tri, fill=NAVY_M + (255,))
        poly(d, tri, MAGENTA, close=True)
        poly(d, [(ox + 3, oy + 4), (ox + 28, oy + 4)], (0xFF, 0x9F, 0xE8))
        poly(d, [(ox + 9, oy + 8 + 2 * k), (ox + 16, oy + 15 + 2 * k), (ox + 23, oy + 8 + 2 * k)],
             (0xFF, 0x9F, 0xE8))
        px2 = im.load()
        px2[ox + 15, oy + 27] = AMBER + (255,)
        px2[ox + 16, oy + 27] = AMBER + (255,)
    # CROSS：十字外框，暗琥珀填充，里面一个方块逐帧转 22.5 度、中心白点
    for k in range(4):
        ox, oy = k * cell, 2 * cell
        outline = [(11, 2), (21, 2), (21, 11), (30, 11), (30, 21), (21, 21), (21, 30), (11, 30),
                   (11, 21), (2, 21), (2, 11), (11, 11)]
        pts = [(ox + x, oy + y) for (x, y) in outline]
        d.polygon(pts, fill=NAVY_A + (255,))
        poly(d, pts, AMBER, close=True)
        poly(d, [(ox + 13, oy + 6), (ox + 19, oy + 6)], (0xFF, 0xD8, 0x8A))
        th = math.pi / 8.0 * k
        sq = []
        for j in range(4):
            a = th + math.pi / 2.0 * j
            sq.append((ox + int(math.floor(16 + 6 * math.cos(a) + 0.5)),
                       oy + int(math.floor(16 + 6 * math.sin(a) + 0.5))))
        poly(d, sq, CYAN, close=True)
        px2 = im.load()
        px2[ox + 16, oy + 16] = WHITE + (255,)
    return im


def boss():
    """96x96 一格，2 列 x 3 行共 6 帧：品红六边形（半径 44）顺转、青色六芒星（半径 30）逆转，每帧 10 度；
    六重对称，6 帧转过 60 度首尾相接。升级：六边形内暗品红渐变填充、六根辐条、
    宽淡光晕 + 细亮线的双层边缘；中心白心品红光点更大。
    帧数是按素材包的总长定的（sw 第 06 章 4.7 节）：12 帧时包总长超过 SW5 坏包判据 S5f 的构造所能覆盖的上限。"""
    cell = 96
    im = Image.new("RGBA", (cell * 2, cell * 3), (0, 0, 0, 0))
    px = im.load()
    for i in range(6):
        ox, oy = (i % 2) * cell, (i // 2) * cell
        # 中心光点（比旧版大一档）
        for y in range(cell):
            for x in range(cell):
                dd = math.sqrt((x + 0.5 - 48) ** 2 + (y + 0.5 - 48) ** 2)
                if dd < 15:
                    px[ox + x, oy + y] = glow_pixel(dd, MAGENTA, 3.5, 7.0, 15.0, 130)
        d = ImageDraw.Draw(im)
        phi = math.radians(10.0 * i)

        def vert(r, a):
            return (ox + math.floor(48 + r * math.cos(a) + 0.5), oy + math.floor(48 + r * math.sin(a) + 0.5))

        hexa = [vert(44, phi + math.pi / 3.0 * k) for k in range(6)]
        tri1 = [vert(30, -phi + math.pi / 6.0 + 2.0 * math.pi / 3.0 * k) for k in range(3)]
        tri2 = [vert(30, -phi + math.pi / 2.0 + 2.0 * math.pi / 3.0 * k) for k in range(3)]
        d.polygon(hexa, fill=(0x2E, 0x0A, 0x28, 110))
        d.polygon(tri1 + [tri1[0]], fill=(0x0A, 0x22, 0x30, 90))
        d.polygon(tri2 + [tri2[0]], fill=(0x0A, 0x22, 0x30, 90))
        for k in range(6):
            poly(d, [vert(14, phi + math.pi / 3.0 * k), vert(44, phi + math.pi / 3.0 * k)],
                 (0x7A, 0x2A, 0x68), width=1, alpha=130)
        poly(d, hexa, MAGENTA, width=5, alpha=70, close=True)
        poly(d, tri1, CYAN, width=4, alpha=60, close=True)
        poly(d, tri2, CYAN, width=4, alpha=60, close=True)
        poly(d, hexa, MAGENTA, width=2, alpha=255, close=True)
        poly(d, hexa, (0xFF, 0xB8, 0xEC), width=1, close=True)
        poly(d, tri1, CYAN, width=1, alpha=255, close=True)
        poly(d, tri2, CYAN, width=1, alpha=255, close=True)
    return im


def stars():
    """2x2 星点 4 种：2x2 亮、2x2 暗、1x1 亮、1x1 暗（1x1 的只点左上角，其余是色键）。"""
    im = Image.new("RGBA", (8, 2), (0, 0, 0, 0))
    px = im.load()
    bright, dim = (0xD8, 0xF8, 0xFF), (0x50, 0x88, 0x9E)
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

def neon_strokes(d, strokes, color, core):
    """霓虹线：宽 5 的淡光晕、宽 3 的色线、宽 1 的白心（核心色）。"""
    for s in strokes:
        d.line(s, fill=color + (70,), width=5, joint="curve")
    for s in strokes:
        d.line(s, fill=color + (255,), width=3, joint="curve")
    for s in strokes:
        d.line(s, fill=core + (255,), width=1)


def title(rows):
    """240x90：MYRIAD 六个字母（每个 28x36，间距 8），水平居中、垂直偏上；
    升级：四角括号装饰、上饰线、字母的暗色投影，底部一行 8x16 点阵提示
    「ALL ARROWS = EX STAGE」（提示随标题图一次性 BLIT——域内逐字文本量一大就会
    把 HW 条带流水线拖过 vsync，见 doc/软件升级-玩法与审美.md §3）。

    画布保持 240x90（游戏区中央与 HUD 顶部的位置都按这个尺寸排）。"""
    im = Image.new("RGBA", (240, 90), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    for (cx, cy, sx, sy) in ((2, 2, 1, 1), (237, 2, -1, 1), (2, 87, 1, -1), (237, 87, -1, -1)):
        d.line([(cx, cy), (cx + 10 * sx, cy)], fill=(0x2A, 0x6A, 0x8C, 220), width=2)
        d.line([(cx, cy), (cx, cy + 8 * sy)], fill=(0x2A, 0x6A, 0x8C, 220), width=2)
    yy, col = 6, (0x2A, 0x5A, 0x74)
    d.line([(36, yy), (204, yy)], fill=col + (150,), width=1)
    d.line([(24, yy), (36, yy)], fill=col + (80,), width=1)
    d.line([(204, yy), (216, yy)], fill=col + (80,), width=1)
    d.line([(118, yy), (122, yy)], fill=CYAN + (255,), width=1)
    x0, y0 = 16, 20
    strokes = []
    for n, ch in enumerate("MYRIAD"):
        lx = x0 + n * 36
        for s in LETTERS[ch]:
            strokes.append([(lx + 7 * gx, y0 + 6 * gy) for (gx, gy) in s])
    shifted = [[(x + 2, y + 2) for (x, y) in s] for s in strokes]
    for s in shifted:
        d.line(s, fill=(0x08, 0x18, 0x3A, 200), width=3, joint="curve")
    neon_strokes(d, strokes, CYAN, (0xE0, 0xFF, 0xFF))
    hint = "ALL ARROWS = EX STAGE"
    px = im.load()
    hx = (240 - len(hint) * 8) // 2
    for i, ch in enumerate(hint):
        g = ord(ch) - 0x20
        if not (0 <= g < 95):
            continue
        for r in range(16):
            line = rows[g * 16 + r]
            for c in range(8):
                if line[c] == "#":
                    px[hx + i * 8 + c, 72 + r] = LABEL + (235,)
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


def font_amber(rows):
    """琥珀字库：与青色字库同一网格，字形像素为 #FFB23E，其余是色键。
    R4 HUD 的分数/纪录数值用琥珀，与青色标签拉开层级。"""
    im = Image.new("RGBA", (128, 96), (0, 0, 0, 0))
    px = im.load()
    for g in range(95):
        gx, gy = (g % 16) * 8, (g // 16) * 16
        for r in range(16):
            line = rows[g * 16 + r]
            for c in range(8):
                if line[c] == "#":
                    px[gx + c, gy + r] = AMBER + (255,)
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
    """16x16 图标 4 格：自机图标（暗填充+亮描边）、炸弹菱形（暗琥珀填充+双层）、判定点（白心半径 2.6、品红环 3.6~5.0）、空。"""
    im = Image.new("RGBA", (64, 16), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    body = [(8, 1), (2, 14), (8, 10), (14, 14)]
    d.polygon(body, fill=NAVY + (255,))
    poly(d, body, PLAYER, close=True)
    dia = [(24, 1), (31, 8), (24, 15), (17, 8)]
    d.polygon(dia, fill=NAVY_A + (255,))
    poly(d, dia, AMBER, close=True)
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


# --- 背景合图 -------------------------------------------------------------------

BG_SKY_W, BG_SKY_H = 400, 480
BG_FAR_W, BG_FAR_H = 512, 56
BG_CITY_W, BG_CITY_H = 512, 72
BG_FAR_Y = BG_SKY_H                                   # 远景带在图里的 y
BG_CITY_Y = BG_SKY_H + BG_FAR_H                       # 城市带在图里的 y
BG_LAYERS_W = 512
BG_LAYERS_H = BG_SKY_H + BG_FAR_H + BG_CITY_H         # 624


def bg_sky_rows():
    """天空 400x480：三段垂直渐变（顶 #02040A -> 中 #050F22 -> 底 #0B223E），
    两团星云径向辉光（青在左上、品红在右下），三条高斯亮带当极光。
    每个像素都不为纯黑（纯黑是色键）。返回逐行函数，主循环逐像素填。"""
    nebulas = ((90, 130, 170.0, CYAN, 44), (320, 300, 210.0, MAGENTA, 34))
    bands = (96.0, 168.0, 250.0)

    def px_color(x, y):
        t = y / (BG_SKY_H - 1.0)
        if t < 0.55:
            base = mix(SKY_TOP, SKY_MID, t / 0.55)
        else:
            base = mix(SKY_MID, SKY_BOT, (t - 0.55) / 0.45)
        r, g, b = base
        for (nx, ny, nr, nc, na) in nebulas:
            dd = math.sqrt((x - nx) ** 2 + (y - ny) ** 2) / nr
            if dd < 1.0:
                a = na * (1.0 - dd) ** 2 / 255.0
                r += (nc[0] - r) * a
                g += (nc[1] - g) * a
                b += (nc[2] - b) * a
        for by in bands:
            dt = (y - by) / 7.0
            if -2.6 < dt < 2.6:
                add = 9.0 * math.exp(-dt * dt)
                r = min(255.0, r + add * 0.45)
                g = min(255.0, g + add * 0.9)
                b = min(255.0, b + add * 1.4)
        return (clamp8(int(r + 0.5)), clamp8(int(g + 0.5)), clamp8(int(b + 0.5)), 255)

    return px_color


def bg_layers():
    """背景合图 512x608（槽 15，一张表面装齐三块，xrgb 色键 0）：
      y 0..479    天空 400x480（不透明渐变+星云+极光带）
      y 480..535  远景剪影带 512x56（低楼、无窗、烘焙小星）
      y 536..607  城市剪影带 512x72（高楼、霓虹顶缘、点阵窗）
    带子部分的天区是透明（色键），画的时候叠在天空上做横向循环视差。"""
    im = Image.new("RGBA", (BG_LAYERS_W, BG_LAYERS_H), (0, 0, 0, 0))
    px = im.load()
    sky = bg_sky_rows()
    for y in range(BG_SKY_H):
        for x in range(BG_SKY_W):
            px[x, y] = sky(x, y)

    # 远景带：8 像素一栋，高度 12..36，顶缘亮线，天空里撒烘焙小星
    for bx in range(0, BG_FAR_W, 8):
        i = bx // 8
        h = 12 + ((i * 37 + 11) % 7) * 4
        for y in range(BG_FAR_H - h, BG_FAR_H):
            for x in range(bx, bx + 8):
                px[x, BG_FAR_Y + y] = FAR_FILL + (255,)
        for x in range(bx, bx + 8):
            px[x, BG_FAR_Y + BG_FAR_H - h] = FAR_EDGE + (255,)
    for i in range(140):
        sx = (i * 53 + 17) % BG_FAR_W
        sy = (i * 29 + 5) % (BG_FAR_H - 40)
        if px[sx, BG_FAR_Y + sy][3] == 0:
            px[sx, BG_FAR_Y + sy] = (0x8C, 0xC8, 0xE0, 255)
    # 城市带：16 像素一栋，高度 20..65；霓虹顶缘（青/品红交替）、点阵窗（琥珀/青）
    for bx in range(0, BG_CITY_W, 16):
        i = bx // 16
        h = 20 + ((i * 53 + 7) % 6) * 9
        top = BG_CITY_Y + BG_CITY_H - h
        for y in range(BG_CITY_H - h, BG_CITY_H):
            for x in range(bx, bx + 16):
                px[x, BG_CITY_Y + y] = CITY_FILL + (255,)
        edge = CYAN if (i & 1) == 0 else MAGENTA
        for x in range(bx, bx + 16):
            px[x, top] = edge + (255,)
        px[bx, top] = edge + (255,)
        px[min(bx + 15, BG_LAYERS_W - 1), top] = edge + (255,)
        if (i % 5) == 2 and h >= 47:                      # 部分楼顶竖天线
            for y in range(top - 8, top):
                px[bx + 8, y] = edge + (255,)
            px[bx + 8, top - 8] = (0xFF, 0x6A, 0x8A, 255)
        for wy in range(top + 4, BG_CITY_Y + BG_CITY_H - 4, 5):
            for wx in range(bx + 3, bx + 14, 4):
                if ((i * 13 + (wx - bx) * 7 + (wy - top) * 5) % 9) < 3:
                    wc = AMBER if ((wx + wy) & 1) == 0 else (0x9A, 0xE8, 0xF2)
                    px[wx, wy] = wc + (255,)
    return im


def build():
    """按文件名给出全部图（有序）。"""
    rows = font_rows()
    return [
        ("bul16.png", round_atlas(16, 2.2, 4.0, 5.6, 210, 7.8, 90)),
        ("bul8.png", round_atlas(8, 1.0, 2.0, 2.8, 175, 3.9, 80)),
        ("rice20.png", rice_atlas()),
        ("pshot.png", pshot()),
        ("spark.png", spark()),
        ("ships.png", ships()),
        ("boss.png", boss()),
        ("stars.png", stars()),
        ("title.png", title(rows)),
        ("digits.png", digits(rows)),
        ("icons.png", icons()),
        ("font_cyan.png", font_cyan(rows)),
        ("font_amber.png", font_amber(rows)),
        ("bg_layers.png", bg_layers()),
    ]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", default=OUT_DIR, help="输出目录（缺省 Main/assets/neon/）")
    ap.add_argument("--base", action="store_true",
                    help="写回入库目录时连 Kenney 叠加的 8 张也写成基线（之后必须重跑 apply_kenney_art.py）")
    ap.add_argument("--check", action="store_true",
                    help="重新生成，与 Main/assets/neon/ 里的逐像素比（不比 Kenney 叠加的 8 张）")
    ap.add_argument("--inject", action="store_true", help="注错：第一张参与比对的图改一个像素（与 --check 一起用）")
    args = ap.parse_args()

    imgs = build()
    mine = [(name, im) for name, im in imgs if name not in KENNEY_OWNED]
    if args.inject:
        name, im = mine[0]
        p = im.getpixel((8, 8))
        im.putpixel((8, 8), (p[0] ^ 1,) + tuple(p[1:]))

    if args.check:
        bad = 0
        for name, im in mine:
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
        print("check %d 张，不同 %d 张（另 %d 张是 Kenney 叠加版，不比）" % (len(mine), bad, len(imgs) - len(mine)))
        return 1 if bad else 0

    same = os.path.normcase(os.path.realpath(args.out)) == os.path.normcase(os.path.realpath(OUT_DIR))
    skip = KENNEY_OWNED if same and not args.base else frozenset()
    os.makedirs(args.out, exist_ok=True)
    for name, im in imgs:
        if name in skip:
            print("%-14s 跳过：入库的是 Kenney 叠加版（重做基线用 --base，再重跑 apply_kenney_art.py）" % name)
            continue
        im.save(os.path.join(args.out, name), format="PNG")
        print("%-14s %4dx%-4d %s" % (name, im.size[0], im.size[1], im.mode))
    return 0


if __name__ == "__main__":
    sys.exit(main())
