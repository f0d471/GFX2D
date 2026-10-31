#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""gen_r4_atlas.py —— 生成三维演示模式（R4）的 64x64 纹理图集（engine 第 19 章）

四个 32x32 区块（模式 4 的 TEX 窗口 16KiB，TEXCFG=0，64 词行距交错装载）：

    区块 0 (0,0)    走廊墙：哑灰蓝整面 + 顶缘亮边 + 水平接缝，不透明
    区块 1 (32,0)   门框：青芯能量横带，不透明
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
import math
import os
import sys
import zlib

from PIL import Image

W = 64
TEXELS = W * W
VERSION = 12

TRANS = (0, 0, 0, 0)
OPAQUE = 255


def px_region0(x, y):
    """走廊墙(深灰蓝·蓝描线·深度零特征):t 轴(深度)方向完全均匀(面片按 phase 每秒回收,深度纹样会被拉伸再跳回=「一抽一抽」,故 t 轴零特征)。墙的远近边界按
    phase(4m,每秒)循环回收,深度方向的任何纹样都会被连续拉伸再跳回——
    顶缘亮线/接缝/渐变就是用户实测「纹理一抽一抽」的根源;均匀后伸缩不可见。
    基色回退深灰蓝;u=21 板缝=蓝描线(纹素低红高绿蓝,过暗乘色后仍读作青蓝线)。"""
    if x == 0: return (118, 130, 144, OPAQUE)        # 面片角缝:暗一线
    if x == 21 or x == 22: return (110, 215, 255, OPAQUE)   # 蓝描线(用户定案「中间的曲线变蓝」)
    return (138, 150, 164, OPAQUE)


def px_region1(x, y):
    """门框能量段:深蓝基底+青芯能量横带(白芯+青体+暗距),带间琥珀刻度。
    u 向跨 32..63 贴在门框厚度上,t 向跨 0..31 沿门框高度拉伸成横档。"""
    lx = x % 32
    ly = y % 32
    edge = min(lx, 31 - lx, ly, 31 - ly)
    if edge == 0: return (6, 12, 20, OPAQUE)
    if edge <= 1: return (40, 58, 74, OPAQUE)
    grad = (31 - ly) * 22 // 31
    r, g, b = 46 + grad, 64 + grad, 88 + grad
    if ly in (13, 14): r, g, b = 235, 252, 255        # 能量带白芯
    elif ly in (11, 12) or ly in (15, 16): r, g, b = 60, 205, 245   # 青体
    elif ly in (9, 10) or ly in (17, 18): r, g, b = 20, 84, 110    # 暗距
    elif ly in (21, 22): r, g, b = 34, 140, 175       # 回声带(暗一档)
    return (r, g, b, OPAQUE)


def px_region2(x, y):
    """自机弹 billboard(用户挑 SSR laserBlue11=X 形):青体白芯双斜杠,
    外缘菱形收边,圈外二值透明。白色顶点色,不吃墙乘色。"""
    if abs(x - 16) + abs(y - 16) > 15:
        return TRANS
    d1 = abs(x - y)
    d2 = abs(x + y - 31)
    if min(d1, d2) <= 1.5:
        return (235, 250, 255, OPAQUE)            # 白芯
    if min(d1, d2) <= 5:
        return (90, 190, 255, OPAQUE)             # 青体
    return TRANS


def px_region3(x, y):
    """敌弹 billboard(用户挑 SSR laserBlue09=四芒星盘):白芯圆核+青蓝
    十字四芒+暗青外缘,圈外二值透明。"""
    dx, dy = x - 16, y - 16
    d = math.hypot(dx, dy)
    if d > 15:
        return TRANS
    if (abs(dx) <= 3 or abs(dy) <= 3) and d <= 14:
        return (120, 205, 255, OPAQUE)            # 四芒臂
    if d <= 6.5:
        return (240, 250, 255, OPAQUE)            # 白核
    if d <= 11:
        return (80, 175, 245, OPAQUE)             # 亮青环
    if d <= 14:
        return (36, 110, 175, OPAQUE)             # 暗青外缘
    return TRANS


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
