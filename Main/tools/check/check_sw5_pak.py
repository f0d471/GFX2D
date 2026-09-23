#!/usr/bin/env python3
"""独立校验素材包（SW5 判据：打包与解包不能互相证明）。

这是**另一份实现**：它自己按 assetpak 的字节格式解包、自己按 manifest 从 PNG
与字库文本重算每一项的像素字节，再与 .pak 逐字节比。
它不 import `pack_assets.py`，也不复用它的任何函数——两边同一个错误才能被看见
（spec 第 01 章 4.1 节的教训）。

    python3 Main/tools/check/check_sw5_pak.py --pak Main/assets/build/assets.pak
    python3 Main/tools/check/check_sw5_pak.py --pak F.pak --assets Main/assets

输出与 assetTest 的 C 侧逐项行同格式（`entry <i> id=.. fmt=.. WxH sl=.. len=.. crc=..`），
所以 check_sw5_assets.sh 可以把两边 diff 一下：两边都算对，才说明格式没写歪。

全部通过时末尾 `fail 0` 与 `done`；否则非零退出。
"""

import argparse
import os
import re
import struct
import sys
import zlib

from PIL import Image

HEADER_LEN = 32
DIR_ENTRY = 32
MAX_ENTRIES = 256
ALIGN = 64
MAGIC = b"G2DP"
VERSION = 1

FMT_XRGB = 0
FMT_PM = 1
FMT_ST = 2
FLAG_KEY = 0x0001

NAME_OF_FMT = {FMT_XRGB: "xrgb", FMT_PM: "premult", FMT_ST: "straight"}

ALPHA_MODES = {"xrgb": FMT_XRGB, "premult": FMT_PM, "straight": FMT_ST}
ALLOWED_PNG_MODES = ("1", "L", "LA", "P", "RGB", "RGBA")
FONT_GLYPHS = 95
COLUMNS = ["kind", "id", "name", "alpha_mode", "png", "size", "slice", "key", "purpose"]

g_fail = 0


def bad(msg):
    global g_fail
    g_fail += 1
    print("FAIL %s" % msg)


def need(cond, msg):
    if not cond:
        bad(msg)
    return cond


def premult(c, a):
    """规格口径：各通道取整到 (c * a + 127) / 255。故意写得与打包工具的查表不同。"""
    return (c * a + 127) // 255


def stride_log2_of(w):
    sl = 2
    while (1 << sl) < w * 4:
        sl += 1
    assert sl <= 31
    return sl


def read_manifest(path):
    rows = {}
    order = []
    with open(path, "r", encoding="utf-8") as f:
        header = None
        for line in f.read().splitlines():
            s = line.strip()
            if s == "" or s.startswith("#"):
                continue
            cells = [c.strip() for c in line.split("\t")]
            if header is None:
                header = cells
                assert header == COLUMNS, "manifest 列名不符：%s" % header
                continue
            row = dict(zip(header, cells))
            rows[int(row["id"])] = row
            order.append(int(row["id"]))
    return rows, order


def parse_size(tok):
    m = re.match(r"^(\d+)x(\d+)$", tok)
    assert m, "尺寸格式不对：%r" % tok
    return int(m.group(1)), int(m.group(2))


def load_font_rows(path, cw, ch):
    with open(path, "r", encoding="utf-8") as f:
        lines = f.read().splitlines()
    rows = [s for l in lines if (s := l.strip()) and
            not (s == "#" or (s.startswith("#") and len(s) > 1 and s[1].isspace()))]
    assert len(rows) == FONT_GLYPHS * ch, "字库行数 %d 不对" % len(rows)
    return rows


def expected_bytes(assets_dir, row):
    """按 manifest 重新算一项的目标字节（BGRA、行跨、行尾 0、预乘/直通/xrgb 色键）。"""
    kind = row["kind"]
    mode = row["alpha_mode"]
    w, h = parse_size(row["size"])
    cw = ch = 0
    if row["slice"] != "-":
        cw, ch = parse_size(row["slice"])
    sl = stride_log2_of(w)
    stride = 1 << sl
    key = None
    if row["key"] != "-":
        key = int(row["key"], 16)

    out = bytearray(h * stride)          # 行尾与未用像素天然是 0

    if kind == "font":
        rows = load_font_rows(os.path.join(assets_dir, row["png"]), cw, ch)
        cols = w // cw
        for g in range(FONT_GLYPHS):
            gx, gy = (g % cols) * cw, (g // cols) * ch
            for r in range(ch):
                line = rows[g * ch + r]
                for c in range(cw):
                    o = (gy + r) * stride + (gx + c) * 4
                    v = 255 if line[c] == "#" else 0
                    out[o + 0] = v      # B
                    out[o + 1] = v      # G
                    out[o + 2] = v      # R
                    # 第 4 个字节留 0：xrgb 的高字节写 0（2.1 节的字节序约定），
                    # 字库的来源 alpha 是 255，但存进包里的那一字节是 0。
        return bytes(out), sl, w, h

    with Image.open(os.path.join(assets_dir, row["png"])) as im:
        assert im.format == "PNG", "%s 不是 PNG 文件" % row["png"]
        assert im.mode in ALLOWED_PNG_MODES, "%s 模式 %s 不该被接受" % (row["png"], im.mode)
        assert im.size == (w, h), "%s 尺寸 %s 与 manifest 不符" % (row["png"], im.size)
        rgba = im.convert("RGBA").tobytes()

    for y in range(h):
        for x in range(w):
            i = (y * w + x) * 4
            r, g, b, a = rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3]
            o = y * stride + x * 4
            if mode == "premult":
                r, g, b = premult(r, a), premult(g, a), premult(b, a)
                assert r <= a and g <= a and b <= a
                out[o + 0], out[o + 1], out[o + 2], out[o + 3] = b, g, r, a
            elif mode == "straight":
                out[o + 0], out[o + 1], out[o + 2], out[o + 3] = b, g, r, a
            else:                       # xrgb
                if key is None:
                    assert a == 255
                    out[o + 0], out[o + 1], out[o + 2] = b, g, r
                else:
                    assert a in (0, 255)
                    if a == 0:
                        out[o + 0] = key & 0xFF
                        out[o + 1] = (key >> 8) & 0xFF
                        out[o + 2] = (key >> 16) & 0xFF
                    else:
                        assert (r << 16) | (g << 8) | b != key, "可见像素与色键撞色"
                        out[o + 0], out[o + 1], out[o + 2] = b, g, r
                out[o + 3] = 0
    return bytes(out), sl, w, h


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    main_dir = os.path.dirname(os.path.dirname(here))
    ap = argparse.ArgumentParser(description="独立校验素材包（SW5）")
    ap.add_argument("--pak", default=os.path.join(main_dir, "assets", "build", "assets.pak"))
    ap.add_argument("--assets", default=os.path.join(main_dir, "assets"))
    ap.add_argument("--manifest", default=None)
    ap.add_argument("--max-pak", type=int, default=4 * 1024 * 1024)
    args = ap.parse_args()

    assets_dir = os.path.abspath(args.assets)
    manifest = args.manifest or os.path.join(assets_dir, "manifest.txt")
    rows, order = read_manifest(manifest)

    with open(args.pak, "rb") as f:
        pak = f.read()

    # --- 头 ---
    need(len(pak) >= HEADER_LEN, "包不足 32 字节")
    magic, version, hlen, count, total, pack_crc, dirb, rsv = struct.unpack_from(
        "<4sIIIIIII", pak, 0)
    need(magic == MAGIC, "magic 不是 G2DP：%r" % magic)
    need(version == VERSION, "版本不是 %d：%d" % (VERSION, version))
    need(hlen == HEADER_LEN, "头长不是 32")
    need(rsv == 0, "头保留字段不是 0")
    need(1 <= count <= MAX_ENTRIES, "条目数 %d 越界" % count)
    need(dirb == count * DIR_ENTRY, "目录字节数 %d 与条目数 %d 不符" % (dirb, count))
    need(total == len(pak), "总长 %d 与文件大小 %d 不符" % (total, len(pak)))
    need(total <= args.max_pak, "总长超过运行预算")

    # --- 整包 CRC（头里的那 4 字节按 0 参与）---
    recomputed = zlib.crc32(pak[:20] + b"\x00\x00\x00\x00" + pak[24:]) & 0xFFFFFFFF
    need(recomputed == pack_crc,
         "整包 CRC 不符：头里 %08X，重算 %08X" % (pack_crc, recomputed))
    print("pak crc=%08X entries=%d total=%d" % (pack_crc, count, total))

    # --- 目录 ---
    entries = []
    for i in range(count):
        o = HEADER_LEN + i * DIR_ENTRY
        (eid, off, length, w, h, sl, fmt, flags, key, dcrc, ersv) = struct.unpack_from(
            "<IIIHHBBHIII", pak, o)
        need(ersv == 0, "第 %d 项保留字段不是 0" % i)
        need(flags & ~FLAG_KEY == 0, "第 %d 项标志位越界" % i)
        need((flags & FLAG_KEY) != 0 or key == 0, "第 %d 项没有色键标志却有键字" % i)
        need(fmt in (FMT_XRGB, FMT_PM, FMT_ST), "第 %d 项格式 %d 不是 v1 的三种" % (i, fmt))
        need(w > 0 and h > 0, "第 %d 项宽高为 0" % i)
        need(2 <= sl <= 31, "第 %d 项 stride_log2=%d 越界" % (i, sl))
        need(w * 4 <= (1 << sl), "第 %d 项宽度乘 4 大于行跨" % i)
        need(length == h << sl, "第 %d 项数据长与行跨不符" % i)
        need(off % ALIGN == 0, "第 %d 项起点未按 64 对齐" % i)
        need(off >= HEADER_LEN + dirb, "第 %d 项起点落进头或目录" % i)
        need(off + length <= total, "第 %d 项越过包末端" % i)
        entries.append(dict(index=i, id=eid, off=off, length=length, w=w, h=h,
                            sl=sl, fmt=fmt, flags=flags, key=key, dcrc=dcrc))

    ids = [e["id"] for e in entries]
    need(len(set(ids)) == len(ids), "ID 有重复")
    for a in range(count):
        for b in range(a + 1, count):
            ea, eb = entries[a], entries[b]
            if ea["off"] < eb["off"] + eb["length"] and eb["off"] < ea["off"] + ea["length"]:
                bad("第 %d 项与第 %d 项的数据区重叠" % (a, b))

    # --- 逐项：CRC + 独立重算的像素字节 ---
    crc_lines = []
    for e in entries:
        data = pak[e["off"]:e["off"] + e["length"]]
        need(zlib.crc32(data) & 0xFFFFFFFF == e["dcrc"],
             "第 %d 项数据 CRC 不符" % e["index"])
        if e["id"] not in rows:
            bad("第 %d 项的 ID %d 不在 manifest 里" % (e["index"], e["id"]))
            continue
        row = rows[e["id"]]
        need(ALPHA_MODES[row["alpha_mode"]] == e["fmt"],
             "第 %d 项格式与 manifest 的 alpha_mode 不符" % e["index"])
        exp, sl, w, h = expected_bytes(assets_dir, row)
        need((sl, w, h) == (e["sl"], e["w"], e["h"]),
             "第 %d 项几何与 manifest 不符" % e["index"])
        need(len(exp) == e["length"], "第 %d 项长度不符" % e["index"])
        if exp != data:
            first = next(i for i in range(len(exp)) if exp[i] != data[i])
            bad("第 %d 项像素字节与独立重算不符，首个不同在偏移 %d（包 %02X 期望 %02X）"
                % (e["index"], first, data[first], exp[first]))
        # 行尾必须是 0
        stride = 1 << e["sl"]
        for y in range(e["h"]):
            tail = data[y * stride + e["w"] * 4:(y + 1) * stride]
            need(all(b == 0 for b in tail), "第 %d 项第 %d 行行尾不是 0" % (e["index"], y))
        # 与 manifest 顺序一致
        need(order[e["index"]] == e["id"], "第 %d 项的顺序与 manifest 不一致" % e["index"])
        crc_lines.append("entry %d id=%d fmt=%d %dx%d sl=%d len=%d crc=%08x"
                         % (e["index"], e["id"], e["fmt"], e["w"], e["h"],
                            e["sl"], e["length"], e["dcrc"]))

    need(count == len(rows), "条目数 %d 与 manifest 的 %d 行不符" % (count, len(rows)))

    for l in crc_lines:
        print(l)

    print("fail %d" % g_fail)
    print("done")
    return 0 if g_fail == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
