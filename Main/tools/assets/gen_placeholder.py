#!/usr/bin/env python3
"""生成占位素材 PNG（sw 第 04 章 3.9 节）。

占位素材由脚本生成，不涉及第三方许可；产物入库，打包就不必先跑本脚本。
所有图案都是确定性的：同一个版本的 Pillow 与同一份脚本产出逐字节相同的 PNG，
脚本里没有任何随机数（棋盘、渐变、圆盘、色块全部按坐标算出来）。

用法（WSL 里，仓库根或任意位置）：
    python3 Main/tools/assets/gen_placeholder.py            # 生成到 Main/assets/placeholder/
    python3 Main/tools/assets/gen_placeholder.py --check    # 只校验现有 PNG 与预期像素一致

判据与用法见 Engineering/TechReports/sw/04-素材流水线.md 3.9 节。
"""

import argparse
import os
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
MAIN = os.path.dirname(os.path.dirname(HERE))       # Main/
OUT_DIR = os.path.join(MAIN, "assets", "placeholder")


def bg_gradient(w=640, h=480):
    """竖直渐变叠一层横向微调，纯不透明（xrgb 用）。"""
    im = Image.new("RGB", (w, h))
    px = im.load()
    for y in range(h):
        g = (y * 255) // (h - 1)
        for x in range(w):
            r = (g + x // 8) & 0xFF
            b = (255 - g + x // 16) & 0xFF
            px[x, y] = (r, g, b)
    return im


def bg_checker(w=128, h=128, cell=16):
    """16 像素棋盘的可平铺小块（128×128），纯不透明，由应用重复 BLIT 铺满屏幕。

    首版素材方案只要一张 640×480 整图（1.875 MiB），棋盘做成小块，
    否则两张整图就吃掉 4 MiB 分区的 96%（sw 第 04 章 3.4 节的容量实算）。
    """
    im = Image.new("RGB", (w, h))
    px = im.load()
    for y in range(h):
        for x in range(w):
            on = ((x // cell) + (y // cell)) & 1
            px[x, y] = (0x20, 0x20, 0x20) if on else (0xE0, 0xE0, 0xC0)
    return im


def tile_sheet(cols=8, rows=6, cw=16, ch=16):
    """图集：cols×rows 个 cw×ch 的格子，每格一个可辨认的图案。

    每格都有 alpha：四角逐渐透明、格子中心不透明，用于验证预乘与半透明混合。
    左上角画一个两像素的白点、右下角画一个两像素的黑点，便于目视核对方向。
    """
    w, h = cols * cw, rows * ch
    im = Image.new("RGBA", (w, h))
    px = im.load()
    for cy in range(rows):
        for cx in range(cols):
            i = cy * cols + cx
            base_r = (37 * i + 40) & 0xFF
            base_g = (73 * i + 90) & 0xFF
            base_b = (113 * i + 160) & 0xFF
            for y in range(ch):
                for x in range(cw):
                    # 距格子中心的切比雪夫距离决定 alpha，形成方块状渐隐
                    dx = abs(x - (cw - 1) / 2.0)
                    dy = abs(y - (ch - 1) / 2.0)
                    d = max(dx, dy) / (cw / 2.0)
                    a = int(255 * (1.0 - d))
                    if a < 0:
                        a = 0
                    r, g, b = base_r, base_g, base_b
                    if x < 2 and y < 2:
                        r = g = b = 255              # 左上白点
                    elif x >= cw - 2 and y >= ch - 2:
                        r = g = b = 0                # 右下黑点
                    px[cx * cw + x, cy * ch + y] = (r, g, b, a)
    return im


def bullet(size=32):
    """圆形弹幕：中心不透明，边缘按半径线性降到 0，外圈完全透明。"""
    im = Image.new("RGBA", (size, size))
    px = im.load()
    c = (size - 1) / 2.0
    for y in range(size):
        for x in range(size):
            d = ((x - c) ** 2 + (y - c) ** 2) ** 0.5
            if d > c:
                px[x, y] = (0, 0, 0, 0)
                continue
            a = int(255 * (1.0 - d / c))
            if a < 0:
                a = 0
            if a > 255:
                a = 255
            # 心部偏白、外缘偏红，便于目视看出混叠
            t = a / 255.0
            r = int(255 * (0.35 + 0.65 * t))
            g = int(255 * (0.10 + 0.55 * t))
            b = int(255 * (0.10 + 0.45 * t))
            px[x, y] = (r, g, b, a)
    return im


def key_demo(size=32, key=(255, 0, 255)):
    """色键用图：只有 A=0 与 A=255 两种，背景是 key 色，中间一个不透明方块。

    xrgb 只接受这两种 alpha；可见像素不能与色键字撞色，所以方块用别的颜色。
    """
    im = Image.new("RGBA", (size, size))
    px = im.load()
    for y in range(size):
        for x in range(size):
            if 8 <= x < 24 and 8 <= y < 24:
                px[x, y] = (0x20, 0xE0, 0x40, 255)
            else:
                px[x, y] = (key[0], key[1], key[2], 0)
    return im


def solid(size=8, color=(0x10, 0x80, 0xF0)):
    """最小的独立表面，用来验证 8×8 这种小块（行跨 2^5）。"""
    return Image.new("RGB", (size, size), color)


IMAGES = {
    "bg_gradient.png": ("RGB", bg_gradient),
    "bg_checker.png": ("RGB", bg_checker),
    "tile_sheet.png": ("RGBA", tile_sheet),
    "bullet.png": ("RGBA", bullet),
    "key_demo.png": ("RGBA", key_demo),
    "solid8.png": ("RGB", solid),
}


def main():
    ap = argparse.ArgumentParser(description="生成或校验 SW5 占位素材")
    ap.add_argument("--check", action="store_true", help="只校验现有 PNG，不写文件")
    args = ap.parse_args()
    check_only = args.check
    os.makedirs(OUT_DIR, exist_ok=True)
    rc = 0
    for name, (mode, fn) in sorted(IMAGES.items()):
        want = fn()
        path = os.path.join(OUT_DIR, name)
        if check_only:
            if not os.path.exists(path):
                print("MISSING %s" % path)
                rc = 1
                continue
            with Image.open(path) as got:
                got = got.convert(mode)
            same = (got.size == want.size) and (got.tobytes() == want.tobytes())
            print("%-16s %s" % (name, "ok" if same else "DIFFERENT"))
            if not same:
                rc = 1
        else:
            want.save(path)
            with Image.open(path) as back:
                back = back.convert(mode)
            same = (back.size == want.size) and (back.tobytes() == want.tobytes())
            print("%-16s %4dx%-4d %7d 字节  回读%s"
                  % (name, want.size[0], want.size[1], os.path.getsize(path),
                     "一致" if same else "不一致 FAIL"))
            if not same:
                rc = 1
    print("占位素材 %s 共 %d 张" % ("校验完毕" if check_only else "生成完毕", len(IMAGES)))
    return rc


if __name__ == "__main__":
    sys.exit(main())
