#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""apply_kenney_art.py —— 用 Kenney Space Shooter Redux (CC0) 贴图替换游戏的霓虹几何素材

第一轮试玩反馈（2026-10-31）：手工几何的飞机/子弹丑，背景阶梯太小。
本脚本在 gen_stg_neon.py 生成的基线上就地替换（同名、同尺寸、同 manifest 槽位）：

    ships.png    自机=playerShip1_blue（蓝→青），DART=enemyRed2（暖色→琥珀），
                 CROSS=enemyGreen2（绿→霓虹绿）；动画帧 = ±8° 摆动，4 帧循环
    boss.png     enemyBlack3 宽翼战列舰（暗部染暗品红，10-31 用户从六候选里选定）缩到 74px，6 帧各转 60°
    pshot.png    laserBlue01（蓝→青）缩到 6x24
    icons.png    第 0 格换自机缩图，其余格保留
    bul16/8.png  重绘圆弹：白芯 + 饱和亮环 + 外辉光（弹幕标准画法）
    rice20.png   重绘米弹：椭圆截面胶囊，16 方向
    bg_layers.png天空区原样保留；远景带 8px/栋 → 32px/栋，城市带 16px/栋 → 64px/栋，
                 楼加高、霓虹顶缘加粗、窗变大（七寸屏可读）

来源：https://opengameart.org/content/space-shooter-redux （Kenney.nl，CC0，
署名可选；正式发布时按 LICENSES.md 记一行）。--src 指向解包后的 PNG 目录。

用法：
    python3 Main/tools/assets/gen_stg_neon.py --base            先把这 8 张写回基线（不加 --base 会跳过它们）
    python3 Main/tools/assets/apply_kenney_art.py --src <SSR-PNG 目录>
之后跑 pack_assets.py 重打包。无随机数，同输入同像素。只重画其它素材时单跑 gen_stg_neon.py 即可，
它默认不碰这 8 张；不要用 --base 又不跑本脚本，那样 Kenney 层会被打回基线（e05fd33 出过一次）。
"""

import argparse
import colorsys
import os
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
MAIN = os.path.dirname(os.path.dirname(HERE))
NEON = os.path.join(MAIN, "assets", "neon")

CYAN = (0x35, 0xF2, 0xFF)
MAGENTA = (0xFF, 0x4F, 0xD8)
AMBER = (0xFF, 0xB2, 0x3E)
GREEN = (0x8D, 0xFF, 0x6A)
BULLET_COLORS = (CYAN, MAGENTA, AMBER, GREEN)
WHITE = (255, 255, 255)

FAR_FILL = (0x0A, 0x1C, 0x33)
FAR_EDGE = (0x2A, 0x74, 0x9E)          # 比原来的 0x16,0x44,0x66 提亮
CITY_FILL = (0x06, 0x10, 0x24)
BOSS_TINT = (0x2A, 0x0A, 0x24)         # gen_stg_neon 的 NAVY_M

BG_SKY_W, BG_SKY_H = 400, 480
BG_FAR_W, BG_FAR_H = 512, 56
BG_CITY_W, BG_CITY_H = 512, 72
BG_FAR_Y = BG_SKY_H
BG_CITY_Y = BG_SKY_H + BG_FAR_H


def clamp8(v):
    return 0 if v < 0 else (255 if v > 255 else int(v))


def mix(a, b, t):
    return (clamp8(a[0] + (b[0] - a[0]) * t),
            clamp8(a[1] + (b[1] - a[1]) * t),
            clamp8(a[2] + (b[2] - a[2]) * t))


def shift_hue(im, lo, hi, target_h, sat_mul=1.0, light_add=0.0):
    """把 hue 落在 [lo,hi]（度）的像素换到 target_h；其余不动。wrap 由调用方拆区间。"""
    px = im.load()
    w, h = im.size
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            if a == 0:
                continue
            hh, ll, ss = colorsys.rgb_to_hls(r / 255.0, g / 255.0, b / 255.0)
            deg = hh * 360.0
            if lo <= deg <= hi and ss > 0.25:
                rr, gg, bb = colorsys.hls_to_rgb(target_h / 360.0, min(0.82, ll + light_add),
                                                 min(1.0, ss * sat_mul))
                px[x, y] = (clamp8(rr * 255), clamp8(gg * 255), clamp8(bb * 255), a)
    return im


def recolor_blue_to_cyan(im):
    return shift_hue(im, 190, 245, 187.0, 1.25, 0.06)


def recolor_warm_to_amber(im):
    im = shift_hue(im, 0, 45, 36.0, 1.15, 0.08)
    return shift_hue(im, 315, 360, 36.0, 1.15, 0.08)


def recolor_green(im):
    return shift_hue(im, 70, 165, 108.0, 1.5, 0.12)


def tint_dark_magenta(im):
    """暗部染暗品红（Boss 主题色），亮部保留白/灰。"""
    px = im.load()
    w, h = im.size
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            if a == 0:
                continue
            luma = (r * 299 + g * 587 + b * 114) / 255000.0
            if luma < 0.62:
                t = (0.62 - luma) / 0.62
                c = mix((r, g, b), BOSS_TINT, 0.85 * t)
                px[x, y] = (c[0], c[1], c[2], a)
    return im


def trim(im):
    box = im.getbbox()
    return im.crop(box) if box else im


def fit(im, cell, margin=2):
    """等比缩到 (cell-2*margin) 内、居中放进 cell x cell。"""
    target = cell - 2 * margin
    w, h = im.size
    s = min(target / w, target / h)
    nw, nh = max(1, round(w * s)), max(1, round(h * s))
    im = im.resize((nw, nh), Image.BICUBIC)
    out = Image.new("RGBA", (cell, cell), (0, 0, 0, 0))
    out.paste(im, ((cell - nw) // 2, (cell - nh) // 2), im)
    return out


def hard_alpha(im):
    """xrgb 色键槽要求 alpha 只有 0/255；半透明边缘按 128 阈值二值化。"""
    px = im.load()
    w, h = im.size
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            px[x, y] = (r, g, b, 255) if a >= 128 else (0, 0, 0, 0)
    return im


def rot(im, deg):
    return im.rotate(deg, resample=Image.BICUBIC, expand=False) if deg else im


def do_ships(src):
    player = recolor_blue_to_cyan(trim(Image.open(os.path.join(src, "playerShip1_blue.png")).convert("RGBA")))
    dart = recolor_warm_to_amber(trim(Image.open(os.path.join(src, "Enemies", "enemyRed2.png")).convert("RGBA")))
    cross = recolor_green(trim(Image.open(os.path.join(src, "Enemies", "enemyGreen2.png")).convert("RGBA")))

    sheet = Image.new("RGBA", (128, 96), (0, 0, 0, 0))
    sheet.paste(hard_alpha(fit(player, 32)), (0, 0), hard_alpha(fit(player, 32)))
    dfit = hard_alpha(fit(dart, 32))
    cfit = hard_alpha(fit(cross, 32))
    for k, deg in enumerate((-8, 0, 8, 0)):
        d = hard_alpha(rot(dfit, deg))
        c = hard_alpha(rot(cfit, deg))
        sheet.paste(d, (32 * k, 32), d)
        sheet.paste(c, (32 * k, 64), c)
    sheet.save(os.path.join(NEON, "ships.png"))
    print("ships.png  128x96 ok")


def do_boss(src):
    boss = tint_dark_magenta(trim(Image.open(os.path.join(src, "Enemies", "enemyBlack3.png")).convert("RGBA")))
    w, h = boss.size
    s = 74.0 / max(w, h)
    boss = boss.resize((max(1, round(w * s)), max(1, round(h * s))), Image.BICUBIC)
    sheet = Image.new("RGBA", (192, 288), (0, 0, 0, 0))
    for k in range(6):
        f = rot(boss, k * 60) if k else boss.copy()
        ox, oy = (k % 2) * 96, (k // 2) * 96          # 2 列 x 3 行,与 gen_stg_neon 和 stg_draw 的取帧一致
        sheet.paste(f, (ox + (96 - f.width) // 2, oy + (96 - f.height) // 2), f)
    sheet.save(os.path.join(NEON, "boss.png"))
    print("boss.png   192x288 ok")


def do_pshot(src):
    laser = recolor_blue_to_cyan(Image.open(os.path.join(src, "Lasers", "laserBlue01.png")).convert("RGBA"))
    laser = laser.resize((6, 24), Image.LANCZOS)
    laser.save(os.path.join(NEON, "pshot.png"))
    print("pshot.png  6x24 ok")


def do_icons(src):
    path = os.path.join(NEON, "icons.png")
    icons = Image.open(path).convert("RGBA")
    player = recolor_blue_to_cyan(trim(Image.open(os.path.join(src, "playerShip1_blue.png")).convert("RGBA")))
    icon = hard_alpha(fit(player, 16, margin=1))
    icons.paste(icon, (0, 0), icon)
    icons.save(path)
    print("icons.png  64x16 ok")


def round_bullet(cell, ring, glow_a):
    """白芯 + 亮环 + 外辉光的圆弹，半直 return Image RGBA。"""
    im = Image.new("RGBA", (cell, cell), (0, 0, 0, 0))
    px = im.load()
    c = (cell - 1) / 2.0
    core = cell * 0.135            # 16px→2.16
    ring_o = cell * 0.325          # 16px→5.2
    glow_o = cell * 0.475          # 16px→7.6
    for y in range(cell):
        for x in range(cell):
            r = ((x - c) ** 2 + (y - c) ** 2) ** 0.5
            if r > glow_o:
                continue
            if r <= core:
                col, a = WHITE, 255
            elif r < core * 1.35:
                col, a = mix(WHITE, ring, (r - core) / (core * 0.35)), 255
            elif r <= ring_o:
                col, a = ring, 255
            elif r < ring_o * 1.15:
                col, a = mix(ring, ring, 0.0), 255 if r < ring_o * 1.08 else 160
            else:
                a = int(glow_a * ((1.0 - (r - ring_o * 1.15) / (glow_o - ring_o * 1.15)) ** 1.5))
                col = ring
            px[x, y] = (col[0], col[1], col[2], clamp8(a))
    return im


def do_bul16():
    sheet = Image.new("RGBA", (64, 16), (0, 0, 0, 0))
    for i, col in enumerate(BULLET_COLORS):
        b = round_bullet(16, col, 150)
        sheet.paste(b, (16 * i, 0), b)
    sheet.save(os.path.join(NEON, "bul16.png"))
    print("bul16.png  64x16 ok")


def do_bul8():
    sheet = Image.new("RGBA", (32, 8), (0, 0, 0, 0))
    for i, col in enumerate(BULLET_COLORS):
        b = round_bullet(8, col, 140)
        sheet.paste(b, (8 * i, 0), b)
    sheet.save(os.path.join(NEON, "bul8.png"))
    print("bul8.png   32x8 ok")


def rice_cell(cell, ring, deg):
    im = Image.new("RGBA", (cell, cell), (0, 0, 0, 0))
    px = im.load()
    c = cell / 2.0
    th = deg * 3.14159265358979 / 180.0
    ux, uy = __import__("math").cos(th), -__import__("math").sin(th)
    core_b = cell * 0.055
    ring_b = cell * 0.135
    glow_b = cell * 0.21
    half = cell * 0.33
    for y in range(cell):
        for x in range(cell):
            dx, dy = x + 0.5 - c, y + 0.5 - c
            u = dx * ux + dy * uy
            v = -dx * uy + dy * ux
            au, bv = abs(u) / (half + 0.001), abs(v)
            if au >= 1.0 or bv > glow_b:
                continue
            if bv <= core_b:
                col, a = WHITE, 255
            elif bv < core_b * 1.4:
                col, a = mix(WHITE, ring, (bv - core_b) / (core_b * 0.4)), 255
            elif bv <= ring_b:
                col, a = ring, 255
            elif bv < ring_b * 1.18:
                col, a = ring, 200
            else:
                a = int(150 * (1.0 - (bv - ring_b * 1.18) / (glow_b - ring_b * 1.18)))
                col = ring
            if au > 0.72:
                a = int(a * (1.0 - (au - 0.72) / 0.28) ** 0.7)
            px[x, y] = (col[0], col[1], col[2], clamp8(a))
    return im


def do_rice20():
    sheet = Image.new("RGBA", (320, 80), (0, 0, 0, 0))
    for row, col in enumerate(BULLET_COLORS):
        for d in range(16):
            b = rice_cell(20, col, d * 22.5)
            sheet.paste(b, (20 * d, 20 * row), b)
    sheet.save(os.path.join(NEON, "rice20.png"))
    print("rice20.png 320x80 ok")


def do_bg_layers():
    path = os.path.join(NEON, "bg_layers.png")
    old = Image.open(path).convert("RGBA")
    im = Image.new("RGBA", (512, 608), (0, 0, 0, 0))
    im.paste(old.crop((0, 0, BG_SKY_W, BG_SKY_H)), (0, 0))
    px = im.load()

    # 远景带：32 像素一栋（原 8），高 20..45，顶缘亮线
    for bx in range(0, BG_FAR_W, 32):
        i = bx // 32
        h = 20 + ((i * 37 + 11) % 6) * 5
        for y in range(BG_FAR_H - h, BG_FAR_H):
            for x in range(bx, min(bx + 30, BG_FAR_W)):
                px[x, BG_FAR_Y + y] = FAR_FILL + (255,)
        for x in range(bx, min(bx + 30, BG_FAR_W)):
            px[x, BG_FAR_Y + BG_FAR_H - h] = FAR_EDGE + (255,)
    for i in range(70):                                    # 烘焙小星（楼顶空隙）
        sx = (i * 53 + 17) % BG_FAR_W
        sy = (i * 29 + 5) % (BG_FAR_H - 46)
        if px[sx, BG_FAR_Y + sy][3] == 0:
            px[sx, BG_FAR_Y + sy] = (0x8C, 0xC8, 0xE0, 255)

    # 城市带：64 像素一栋（原 16），高 26..66；霓虹顶缘 2px + 亮芯 1px、大窗 3x3
    for bx in range(0, BG_CITY_W, 64):
        i = bx // 64
        h = 26 + ((i * 53 + 7) % 5) * 10
        top = BG_CITY_Y + BG_CITY_H - h
        for y in range(BG_CITY_H - h, BG_CITY_H):
            for x in range(bx, min(bx + 60, BG_FAR_W)):
                px[x, BG_CITY_Y + y] = CITY_FILL + (255,)
        edge = CYAN if (i & 1) == 0 else MAGENTA
        for x in range(bx, min(bx + 60, BG_FAR_W)):
            px[x, top] = edge + (255,)
            px[x, top + 1] = mix(edge, WHITE, 0.45) + (255,)
        if (i % 3) == 2 and h <= 60:                       # 天线
            for y in range(top - 9, top):
                px[bx + 30, y] = edge + (255,)
            px[bx + 30, top - 9] = (0xFF, 0x6A, 0x8A, 255)
        wy = top + 5
        while wy < BG_CITY_Y + BG_CITY_H - 5:
            wx = bx + 6
            while wx < min(bx + 54, BG_FAR_W):
                if ((i * 13 + (wx - bx) * 7 + (wy - top) * 5) % 9) < 3:
                    wc = AMBER if ((wx + wy) & 1) == 0 else (0x9A, 0xE8, 0xF2)
                    for yy in range(3):
                        for xx in range(3):
                            px[wx + xx, wy + yy] = wc + (255,)
                wx += 9
            wy += 10
    im.save(path)
    print("bg_layers.png 512x608 ok")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True, help="Space Shooter Redux 解包后的 PNG 目录")
    args = ap.parse_args()
    src = args.src
    for rel in ("playerShip1_blue.png", os.path.join("Enemies", "enemyRed2.png"),
                os.path.join("Enemies", "enemyGreen2.png"), os.path.join("Enemies", "enemyBlack3.png"),
                os.path.join("Lasers", "laserBlue01.png")):
        if not os.path.exists(os.path.join(src, rel)):
            print("缺源文件:", rel, file=sys.stderr)
            return 1
    do_ships(src)
    do_boss(src)
    do_pshot(src)
    do_icons(src)
    do_bul16()
    do_bul8()
    do_rice20()
    do_bg_layers()
    return 0


if __name__ == "__main__":
    sys.exit(main())
