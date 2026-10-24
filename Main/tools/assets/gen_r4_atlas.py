#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""gen_r4_atlas.py —— 生成三维演示模式（R4）的 64x64 纹理图集（engine 第 19 章）

四个 32x32 区块（模式 4 的 TEX 窗口 16KiB，TEXCFG=0，64 词行距交错装载）：

    区块 0 (0,0)    走廊墙：蓝灰面板 + 每 8 px 凹槽 + 角部铆钉，不透明
    区块 1 (32,0)   门框：琥珀横纹 + 亮边，不透明
    区块 2 (0,32)   敌机 billboard：橙色圆形机身 + 座舱，圈外二值透明
    区块 3 (32,32)  敌弹 billboard：红色光球，圈外二值透明

透明像素 = 原始 ARGB 全 0（RAS_KEY=0 色键抠底）；可见像素 alpha=255。
RAST 忽略 alpha 合成，不写半透明。图案全部按坐标算：没有随机数，
同一份脚本在同一个 Python 上给出同样的像素。

产物（都写进 Main/apps 可包含的 src/ 与 PNG 参照目录）：

    sw/apps/game/gfx2dGame/src/r4_atlas.h   const 词数组 4096 + CRC + 版本（入库）
    Main/assets/r4/atlas.png                参照 PNG（入库；不进 assetpak）

用法：

    python3 Main/tools/assets/gen_r4_atlas.py               生成上面两份
    python3 Main/tools/assets/gen_r4_atlas.py --check       重新生成，与入库的逐像素比
    python3 Main/tools/assets/gen_r4_atlas.py --check --inject   注错：改一个像素再比，必须见红
"""

import argparse
import os
import sys
import zlib

from PIL import Image

W = 64
TEXELS = W * W
VERSION = 1

TRANS = (0, 0, 0, 0)
OPAQUE = 255


def px_region0(x, y):
    """走廊墙：#304a68 底，每 8 px 凹槽加深，四角铆钉亮点。"""
    base = (0x30, 0x4A, 0x68)
    lx, ly = x % 8, y % 8
    r, g, b = base
    if lx == 0 or ly == 0:
        r, g, b = 0x24, 0x38, 0x52
    if lx == 4 and ly == 4:
        r, g, b = 0x58, 0x74, 0x92
    return (r, g, b, OPAQUE)


def px_region1(x, y):
    """门框：#c06828 底 + 每 4 px 横纹 + 2 px 亮边框。"""
    lx = x % 32
    ly = y % 32
    if lx < 2 or ly < 2 or lx >= 30 or ly >= 30:
        return (0xE8, 0xA0, 0x50, OPAQUE)
    if (y % 4) < 2:
        return (0xC0, 0x68, 0x28, OPAQUE)
    return (0x8A, 0x4A, 0x18, OPAQUE)


def px_region2(x, y):
    """敌机：圆心 (16,16)，半径 12 机身 #ff7030，座舱 r5 #ffd080，圈外透明。"""
    dx, dy = x - 16, y - 16
    d2 = dx * dx + dy * dy
    if d2 > 12 * 12:
        return TRANS
    if dx * dx + (dy + 2) * (dy + 2) <= 5 * 5:
        return (0xFF, 0xD0, 0x80, OPAQUE)
    if d2 > 9 * 9 and abs(dy) > 7:
        return (0xB0, 0x40, 0x10, OPAQUE)       # 机身下缘暗一圈
    return (0xFF, 0x70, 0x30, OPAQUE)


def px_region3(x, y):
    """敌弹：圆心 (16,16)，光球 r12：核心 r5 亮、体 #ff5050、缘 #a02020。"""
    dx, dy = x - 16, y - 16
    d2 = dx * dx + dy * dy
    if d2 > 12 * 12:
        return TRANS
    if d2 <= 5 * 5:
        return (0xFF, 0xD0, 0xD0, OPAQUE)
    if d2 <= 10 * 10:
        return (0xFF, 0x50, 0x50, OPAQUE)
    return (0xA0, 0x20, 0x20, OPAQUE)


REGIONS = ((0, 0, px_region0), (32, 0, px_region1),
           (0, 32, px_region2), (32, 32, px_region3))


def build_pixels():
    """返回 64x64 的 RGBA 像素列表（行主序）。"""
    grid = [[TRANS] * W for _ in range(W)]
    for ox, oy, fn in REGIONS:
        for y in range(32):
            for x in range(32):
                grid[oy + y][ox + x] = fn(x, y)
    return grid


def build_words(grid):
    """原始 ARGB 小端词（TEX 窗口按线性词序装载，地址译码由硬件交错）。"""
    words = []
    for y in range(W):
        for x in range(W):
            r, g, b, a = grid[y][x]
            words.append((a << 24) | (r << 16) | (g << 8) | b)
    return words


def words_crc(words):
    c = 0
    for w in words:
        c = zlib.crc32(w.to_bytes(4, "little"), c)
    return c


def write_png(grid, path):
    img = Image.new("RGBA", (W, W))
    for y in range(W):
        for x in range(W):
            img.putpixel((x, y), grid[y][x])
    os.makedirs(os.path.dirname(path), exist_ok=True)
    img.save(path)


HEADER = """/******************************************************************************
* @file r4_atlas.h: 模式 4 的 64x64 纹理图集原始词（生成物，勿手改）
*
* @brief  由 Main/tools/assets/gen_r4_atlas.py 生成（版本 %u，词 CRC 0x%08X）。
*         四个 32x32 区块：(0,0) 墙、(32,0) 门框、(0,32) 敌机、(32,32) 敌弹。
*         透明像素 = 全 0（RAS_KEY=0）；可见像素 alpha=255。
*         TEX 装载按线性词序 gfx_engine_tex_write，交错由硬件地址译码完成。
******************************************************************************/
#ifndef GFX2D_R4_ATLAS_H
#define GFX2D_R4_ATLAS_H

#include <stdint.h>

#define R4_ATLAS_VERSION %u
#define R4_ATLAS_CRC     0x%08Xu
#define R4_ATLAS_WORDS   %u

static const uint32_t r4_atlas_words[R4_ATLAS_WORDS] = {
"""

FOOTER = """};

#endif /* GFX2D_R4_ATLAS_H */
"""


def write_header(words, crc, path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    parts = [HEADER % (VERSION, crc, VERSION, crc, TEXELS)]
    for y in range(W):
        row = ", ".join("0x%08Xu" % words[y * W + x] for x in range(W))
        parts.append("    %s,\n" % row)
    parts.append(FOOTER)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("".join(parts))


def read_header_words(path):
    """从入库头文件解析词与 CRC（--check 的另一半，独立于构建流程）。"""
    words = []
    crc = None
    version = None
    with open(path, encoding="utf-8") as f:
        for line in f:
            if "#define R4_ATLAS_CRC" in line:
                crc = int(line.split()[2].rstrip("uU"), 16)
            elif "#define R4_ATLAS_VERSION" in line:
                version = int(line.split()[2].rstrip("uU"))
            elif line.strip().startswith("0x"):
                words.extend(int(tok.rstrip("uU"), 16) for tok in line.replace(",", " ").split())
    return version, crc, words


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--inject", action="store_true")
    ap.add_argument("--root", default=None, help="仓库根（缺省按脚本位置推）")
    args = ap.parse_args()
    root = args.root
    if root is None:
        # 脚本在 <root>/Main/tools/assets/ 下：上溯四级到仓库根。
        root = os.path.dirname(os.path.dirname(os.path.dirname(
            os.path.dirname(os.path.abspath(__file__)))))

    grid = build_pixels()
    words = build_words(grid)
    crc = words_crc(words)
    if args.inject:
        words[16 * W + 16] ^= 0x1
        crc = words_crc(words)

    src = os.path.join(root, "Main", "sw", "apps", "game", "gfx2dGame", "src", "r4_atlas.h")
    png = os.path.join(root, "Main", "assets", "r4", "atlas.png")
    if args.check:
        version, old_crc, old_words = read_header_words(src)
        problems = []
        if version != VERSION:
            problems.append("version %u != %u" % (version, VERSION))
        if old_crc != crc:
            problems.append("crc 0x%08X != 0x%08X" % (old_crc, crc))
        if len(old_words) != TEXELS:
            problems.append("words %u != %u" % (len(old_words), TEXELS))
        else:
            bad = [i for i in range(TEXELS) if old_words[i] != words[i]]
            if bad:
                problems.append("%u words differ, first at %u" % (len(bad), bad[0]))
        if problems:
            print("R4 ATLAS CHECK FAIL: " + "; ".join(problems))
            return 1
        print("R4 ATLAS CHECK PASS %u words crc 0x%08X" % (TEXELS, crc))
        return 0

    write_header(words, crc, src)
    write_png(grid, png)
    print("R4 ATLAS generated %u words crc 0x%08X -> %s" % (TEXELS, crc, src))
    return 0


if __name__ == "__main__":
    sys.exit(main())
