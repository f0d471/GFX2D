#!/usr/bin/env python3
"""把 Main/assets/ 下的 PNG 与点阵字库打成素材包，并生成 assets.h（sw 第 04 章 3.6、3.7 节）。

    python3 Main/tools/assets/pack_assets.py
    python3 Main/tools/assets/pack_assets.py --max-pak 1048576     收紧运行预算（E6 出数后用）
    python3 Main/tools/assets/pack_assets.py --assets <目录> --out <目录>

输入  <assets>/manifest.txt（TSV，字段见该文件头）
输出  <out>/assets.pak   包格式的权威是 Main/sw/lib/core/assetpak.h
      <out>/assets.h     稳定编号、包版本与 CRC、宽高/行跨/格式、图集网格

两条不能变的性质：

1. **预乘用 65536 项查表**，口径是 `(c * a + 127) / 255`。Pillow 的 `ImageChops.multiply`
   是 `floor(c * a / 255)`，65536 组里有 31770 组与规格不同（48.5%，最小反例 c=1,a=128），
   拿它图快会让预乘结果与 spec 第 01 章、与板上硬件全面对不上。
2. **相同输入逐字节产出相同的包与头文件**：条目顺序就是 manifest 顺序，没有随机数、
   没有时间戳、没有绝对路径进产物。

判据见 Engineering/TechReports/sw/04-素材流水线.md 3.12 节，串联执行见
Main/tools/check/check_sw5_assets.sh。
"""

import argparse
import os
import re
import struct
import sys
import zlib

from PIL import Image

# --- 包格式常量（与 sw/lib/core/assetpak.h 必须一致）----------------------------
PAK_MAGIC = b"G2DP"
PAK_VERSION = 1
HEADER_LEN = 32
DIR_ENTRY_LEN = 32
MAX_ENTRIES = 256
ENTRY_ALIGN = 64
FLASH_ASSET_MAX = 4 * 1024 * 1024          # 设计文档 2.2 节：素材分区物理上限

FMT_XRGB8888 = 0
FMT_ARGB8888_PM = 1
FMT_ARGB8888_ST = 2

FLAG_KEY = 0x0001

ALPHA_MODES = {"premult": FMT_ARGB8888_PM,
               "straight": FMT_ARGB8888_ST,
               "xrgb": FMT_XRGB8888}

# 只接受能按规格展开成 8 位 RGBA 的模式。I;16 / I / F / CMYK 等一律报错：
# Pillow 对 I;16 的 convert("RGBA") 是截断到 255（1000 -> 255），不是缩放，
# 静默转换会产出一张近全白的图，到画面上才发现。
ALLOWED_PNG_MODES = ("1", "L", "LA", "P", "RGB", "RGBA")
FONT_GLYPHS = 95                            # ASCII 0x20..0x7E
FONT_FIRST = 0x20

NAME_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
PURPOSE_RE = re.compile(r"^[A-Za-z0-9_]+$")

# 预乘查表：索引 (c << 8) | a
PM_LUT = bytes((c * a + 127) // 255 for c in range(256) for a in range(256))
assert len(PM_LUT) == 65536


class PakError(Exception):
    pass


def need(cond, msg):
    if not cond:
        raise PakError(msg)


def align_up(v, a):
    return (v + a - 1) // a * a


def stride_log2_for(w):
    """不小于 w*4 的最小 2 的幂的指数，下限 2（gfx_surface_struct_ok 的下限）。"""
    need(w > 0, "宽度必须大于 0")
    sl = 2
    while (1 << sl) < w * 4:
        sl += 1
    need(sl <= 31, "宽度 %d 需要 stride_log2 = %d，超过 31" % (w, sl))
    return sl


# --- manifest -----------------------------------------------------------------

COLUMNS = ["kind", "id", "name", "alpha_mode", "png", "size", "slice", "key", "purpose"]


class Item(object):
    def __init__(self, row, index):
        self.row = row
        self.index = index                  # 包内条目序号，等于 manifest 顺序
        self.kind = row["kind"]
        try:
            self.id = int(row["id"], 10)
        except ValueError:
            raise PakError("id 必须是十进制整数，实际 %r" % row["id"])
        self.name = row["name"]
        self.alpha_mode = row["alpha_mode"]
        self.rel = row["png"]
        self.path = None
        self.w = self.h = 0
        self.sl = 0
        self.fmt = ALPHA_MODES.get(self.alpha_mode)
        self.key = None
        self.flags = 0
        self.data = b""
        self.crc = 0
        self.offset = 0
        self.cols = self.rows = 0
        self.cw = self.ch = 0

    @property
    def length(self):
        return len(self.data)


def parse_size(tok, what):
    m = re.match(r"^(\d+)x(\d+)$", tok)
    need(m is not None, "%s 必须是 WxH 形式，实际是 %r" % (what, tok))
    w, h = int(m.group(1)), int(m.group(2))
    need(w > 0 and h > 0, "%s 的宽高必须大于 0" % what)
    return w, h


def safe_join(root, rel):
    """相对路径必须落在素材根内：不接受绝对路径，也不接受 .. 跳出。"""
    need(not os.path.isabs(rel), "不接受绝对路径：%r" % rel)
    need("\\" not in rel, "路径分隔符用 /，不要用反斜杠：%r" % rel)
    full = os.path.realpath(os.path.join(root, rel))
    base = os.path.realpath(root)
    need(full == base or full.startswith(base + os.sep),
         "路径跳出素材根目录：%r" % rel)
    need(os.path.isfile(full), "找不到文件：%r" % rel)
    return full


def read_manifest(path):
    need(os.path.isfile(path), "找不到 manifest：%s" % path)
    with open(path, "r", encoding="utf-8") as f:
        raw = f.read().splitlines()

    header = None
    rows = []
    for lineno, line in enumerate(raw, 1):
        s = line.strip()
        if s == "" or s.startswith("#"):
            continue
        cells = line.split("\t")
        if header is None:
            header = [c.strip() for c in cells]
            need(header == COLUMNS,
                 "manifest 的列名必须是 %s，实际是 %s" % (COLUMNS, header))
            continue
        need(len(cells) == len(COLUMNS),
             "第 %d 行有 %d 列，应为 %d 列" % (lineno, len(cells), len(COLUMNS)))
        rows.append((lineno, dict(zip(COLUMNS, [c.strip() for c in cells]))))

    need(header is not None, "manifest 缺少列名行")
    need(rows, "manifest 里没有任何素材")
    need(len(rows) <= MAX_ENTRIES, "条目数 %d 超过 v1 上限 %d" % (len(rows), MAX_ENTRIES))
    return rows


# --- 载入与转换 ---------------------------------------------------------------

def load_png_rgba(path, rel, expect_size):
    try:
        im = Image.open(path)
    except Exception as exc:                     # Pillow 的异常类型不止一种
        raise PakError("%s 打不开：%s" % (rel, exc))
    with im:
        need(im.format == "PNG", "%s 不是 PNG 文件（实际格式 %s）" % (rel, im.format))
        need(im.mode in ALLOWED_PNG_MODES,
             "%s 是 %s 模式；只接受 %s（16 位灰度 I;16 的 convert('RGBA') 是截断到 255，不是缩放）"
             % (rel, im.mode, "/".join(ALLOWED_PNG_MODES)))
        need(im.size == expect_size,
             "%s 实际 %dx%d，manifest 写的是 %dx%d"
             % (rel, im.size[0], im.size[1], expect_size[0], expect_size[1]))
        return im.convert("RGBA").tobytes(), im.size


def load_font_rgba(path, rel, expect_size, cw, ch):
    """把 8×16 点阵字库文本渲染成图集位图。

    文本格式（Main/assets/font/ascii8x16.txt）：# 后跟空白是注释，空行忽略，
    其余每行是一个字形的一行像素，恰好 8 个字符，'.'=0、'#'=1、最左是最高位；
    每个字形 16 行，按 ASCII 0x20..0x7E 共 95 个。
    """
    with open(path, "r", encoding="utf-8") as f:
        lines = f.read().splitlines()
    rows = []
    for lineno, line in enumerate(lines, 1):
        s = line.strip()
        if s == "" or s == "#" or (s.startswith("#") and len(s) > 1 and s[1].isspace()):
            continue
        need(len(s) == cw, "%s 第 %d 行 %d 个字符，应为 %d" % (rel, lineno, len(s), cw))
        need(all(c in ".#" for c in s), "%s 第 %d 行有非法字符" % (rel, lineno))
        rows.append(s)
    need(len(rows) == FONT_GLYPHS * ch,
         "%s 有 %d 行数据，应为 %d（%d 个字形 × %d 行）"
         % (rel, len(rows), FONT_GLYPHS * ch, FONT_GLYPHS, ch))

    w, h = expect_size
    need(w % cw == 0 and h % ch == 0, "字库图集 %dx%d 不能被单元 %dx%d 整除" % (w, h, cw, ch))
    cols, grow = w // cw, h // ch
    need(cols * grow >= FONT_GLYPHS,
         "字库图集 %dx%d 只有 %d 格，放不下 %d 个字形" % (w, h, cols * grow, FONT_GLYPHS))

    # alpha 全 255（xrgb 不带色键时要求如此）：未用到的格子也是不透明的黑，
    # 字形以外的地方靠 BLIT 的色键 0x00000000 跳过。
    buf = bytearray([0x00, 0x00, 0x00, 0xFF] * (w * h))
    for g in range(FONT_GLYPHS):
        gx, gy = (g % cols) * cw, (g // cols) * ch
        for r in range(ch):
            line = rows[g * ch + r]
            for c in range(cw):
                if line[c] != "#":
                    continue
                o = ((gy + r) * w + (gx + c)) * 4
                buf[o] = 0xFF
                buf[o + 1] = 0xFF
                buf[o + 2] = 0xFF
    return bytes(buf), (w, h)


def to_packed_bytes(rgba, w, h, mode, key):
    """RGBA 像素 -> 目标字节：每行 stride 字节、行尾填 0、字节序 B,G,R,A（xrgb 末字节 0）。"""
    sl = stride_log2_for(w)
    stride = 1 << sl
    out = bytearray(h * stride)
    stats = {"changed": 0, "max_delta": 0, "rounded_up": 0, "key_px": 0}

    key_bytes = None
    if key is not None:
        key_bytes = bytes((key & 0xFF, (key >> 8) & 0xFF, (key >> 16) & 0xFF, 0))

    for y in range(h):
        so = y * w * 4
        do = y * stride
        for x in range(w):
            i = so + x * 4
            r, g, b, a = rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3]

            if mode == "premult":
                pr = PM_LUT[(r << 8) | a]
                pg = PM_LUT[(g << 8) | a]
                pb = PM_LUT[(b << 8) | a]
                if (pr, pg, pb) != (r, g, b):
                    stats["changed"] += 1
                    d = max(abs(pr - r), abs(pg - g), abs(pb - b))
                    if d > stats["max_delta"]:
                        stats["max_delta"] = d
                for c0, c1 in ((r, pr), (g, pg), (b, pb)):
                    if c1 != (c0 * a) // 255:
                        stats["rounded_up"] += 1
                need(pr <= a and pg <= a and pb <= a,
                     "预乘后 R/G/B 必须 <= A（c=%d a=%d -> %d）" % (r, a, pr))
                r, g, b = pr, pg, pb
            elif mode == "xrgb":
                if key is None:
                    need(a == 255,
                         "无色键的 xrgb 要求 alpha 全 255，(%d,%d) 是 %d" % (x, y, a))
                else:
                    need(a == 0 or a == 255,
                         "带色键的 xrgb 只接受 alpha 0/255，(%d,%d) 是 %d" % (x, y, a))
                    if a == 0:
                        r, g, b = key_bytes[2], key_bytes[1], key_bytes[0]
                        stats["key_px"] += 1
                    else:
                        word = (r << 16) | (g << 8) | b
                        need(word != key,
                             "可见像素 (%d,%d) 的颜色 0x%06X 与色键字撞色" % (x, y, word))
                a = 0
            # straight：RGB 与 A 原样保留

            out[do + x * 4 + 0] = b
            out[do + x * 4 + 1] = g
            out[do + x * 4 + 2] = r
            out[do + x * 4 + 3] = a

    return bytes(out), sl, stats


def build_item(root, lineno, row, index):
    it = Item(row, index)
    where = "manifest 第 %d 行" % lineno

    need(it.kind in ("surf", "atlas", "font"), "%s：kind 只能是 surf/atlas/font" % where)
    need(1 <= it.id <= 0xFFFFFFFF, "%s：id 超出 1..4294967295" % where)
    need(NAME_RE.match(it.name) is not None,
         "%s：name 只能是字母数字下划线且不以数字开头，实际 %r" % (where, it.name))
    need(it.alpha_mode in ALPHA_MODES,
         "%s：alpha_mode 只能是 %s" % (where, "/".join(sorted(ALPHA_MODES))))
    need(PURPOSE_RE.match(row["purpose"]) is not None,
         "%s：purpose 只能是字母数字下划线" % where)

    size = parse_size(row["size"], "%s 的 size" % where)
    need(size[0] <= 0xFFFF and size[1] <= 0xFFFF,
         "%s：宽高不能超过包目录的 u16 上限" % where)
    need(size[1] * (1 << stride_log2_for(size[0])) <= FLASH_ASSET_MAX,
         "%s：单项按实际行跨计算已超过 4 MiB 素材分区" % where)
    it.path = safe_join(root, it.rel)

    if row["slice"] != "-":
        it.cw, it.ch = parse_size(row["slice"], "%s 的 slice" % where)

    # 色键：只有 xrgb 能带
    if row["key"] != "-":
        need(re.match(r"^0x[0-9A-Fa-f]{8}$", row["key"]) is not None,
             "%s：色键要写成 0x00RRGGBB，实际 %r" % (where, row["key"]))
        it.key = int(row["key"], 16)
        need(it.key <= 0xFFFFFF, "%s：色键高字节必须为 0" % where)
        need(it.alpha_mode == "xrgb",
             "%s：色键只与 xrgb 搭配（比较域是源表面的 32 位存储字，"
             "预乘/直通条目的字已被改过）" % where)
        it.flags |= FLAG_KEY

    if it.kind == "font":
        need((it.cw, it.ch) == (8, 16), "%s：字库单元必须是 8x16" % where)
        need(it.alpha_mode == "xrgb", "%s：字库目前只支持 xrgb" % where)
        need(row["slice"] != "-", "%s：font 必须给 slice（单元尺寸）" % where)
        rgba, (w, h) = load_font_rgba(it.path, it.rel, size, it.cw, it.ch)
    else:
        rgba, (w, h) = load_png_rgba(it.path, it.rel, size)
        if it.kind == "atlas":
            need(row["slice"] != "-", "%s：atlas 必须给 slice（单元尺寸）" % where)
            need(w % it.cw == 0 and h % it.ch == 0,
                 "%s：图集 %dx%d 不能被单元 %dx%d 整除" % (where, w, h, it.cw, it.ch))
        else:
            need(row["slice"] == "-", "%s：surf 的 slice 必须是 -" % where)

    it.w, it.h = w, h
    it.data, it.sl, it.stats = to_packed_bytes(rgba, w, h, it.alpha_mode, it.key)
    need(len(it.data) == h * (1 << it.sl),
         "%s：数据长 %d 与 height<<stride_log2 不符" % (where, len(it.data)))
    it.crc = zlib.crc32(it.data) & 0xFFFFFFFF
    if it.kind in ("atlas", "font"):
        it.cols, it.rows = w // it.cw, h // it.ch
    return it


# --- 组包 ---------------------------------------------------------------------

def build_pak(items):
    off = HEADER_LEN + DIR_ENTRY_LEN * len(items)
    for it in items:
        off = align_up(off, ENTRY_ALIGN)
        it.offset = off
        off += it.length
    total = off
    need(total <= 0xFFFFFFFF, "包总长超出 32 位")

    buf = bytearray(total)                      # 间隙与行尾天然是 0
    for it in items:
        buf[it.offset:it.offset + it.length] = it.data

    dir_bytes = DIR_ENTRY_LEN * len(items)
    struct.pack_into("<4sIIIIIII", buf, 0,
                     PAK_MAGIC, PAK_VERSION, HEADER_LEN, len(items),
                     total, 0, dir_bytes, 0)
    for i, it in enumerate(items):
        o = HEADER_LEN + i * DIR_ENTRY_LEN
        struct.pack_into("<IIIHHBBHIII", buf, o,
                         it.id, it.offset, it.length, it.w, it.h,
                         it.sl, it.fmt, it.flags, it.key or 0, it.crc, 0)

    pack_crc = zlib.crc32(bytes(buf)) & 0xFFFFFFFF      # 此时头里的 CRC 字段是 0
    struct.pack_into("<I", buf, 20, pack_crc)
    return bytes(buf), pack_crc


def build_header(items, total, pack_crc, max_pak):
    L = []
    a = L.append
    a("/* 由 Main/tools/assets/pack_assets.py 从 manifest.txt 生成，不要手改。")
    a(" *")
    a(" * 包格式的权威是 Main/sw/lib/core/assetpak.h；")
    a(" * ENTRY 是包内条目序号（assetpak_entry_at 用），ID 是 manifest 里写明的不变编号")
    a(" * （assetpak_find 用）。包 ID 不是表面槽号：gfx_surface_table 只有 16 槽，")
    a(" * 应用按当前画面需要选择要登记哪几条，不能按 ID 一次全登。 */")
    a("#ifndef GFX2D_ASSETS_H")
    a("#define GFX2D_ASSETS_H")
    a("")
    a("#define ASSET_PAK_VERSION     %du" % PAK_VERSION)
    a("#define ASSET_PAK_TOTAL_LEN   %du" % total)
    a("#define ASSET_PAK_CRC32       0x%08Xu" % pack_crc)
    a("#define ASSET_PAK_ENTRY_COUNT %du" % len(items))
    a("#define ASSET_PAK_MAX_BYTES   %du" % max_pak)
    a("")
    for it in items:
        n = it.name.upper()
        a("/* %s：%s %dx%d，%s，%d 字节 */"
          % (it.name, it.kind, it.w, it.h, it.alpha_mode, it.length))
        a("#define ASSET_%s_ID          %du" % (n, it.id))
        a("#define ASSET_%s_ENTRY       %du" % (n, it.index))
        a("#define ASSET_%s_W           %du" % (n, it.w))
        a("#define ASSET_%s_H           %du" % (n, it.h))
        a("#define ASSET_%s_STRIDE_LOG2 %du" % (n, it.sl))
        a("#define ASSET_%s_FORMAT      %du" % (n, it.fmt))
        a("#define ASSET_%s_LENGTH      %du" % (n, it.length))
        if it.flags & FLAG_KEY:
            a("#define ASSET_%s_KEY         0x%08Xu" % (n, it.key))
        if it.kind in ("atlas", "font"):
            a("#define ASSET_%s_COLS        %du" % (n, it.cols))
            a("#define ASSET_%s_ROWS        %du" % (n, it.rows))
            a("#define ASSET_%s_CELL_W      %du" % (n, it.cw))
            a("#define ASSET_%s_CELL_H      %du" % (n, it.ch))
            a("#define ASSET_%s_CELLS       %du" % (n, it.cols * it.rows))
        a("")
    a("#endif /* GFX2D_ASSETS_H */")
    return "\n".join(L) + "\n"


def check_unique(items):
    """显式 ID 与生成的 C 宏名都必须唯一。"""
    seen_id, seen_macro = {}, {}
    for it in items:
        if it.id in seen_id:
            raise PakError("id %d 重复：%s 与 %s" % (it.id, seen_id[it.id], it.name))
        macro = it.name.upper()
        if macro in seen_macro:
            old = seen_macro[macro]
            raise PakError("name %s 与 %s 会生成同名宏 ASSET_%s"
                           % (old, it.name, macro))
        seen_id[it.id] = it.name
        seen_macro[macro] = it.name


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    main_dir = os.path.dirname(os.path.dirname(here))
    ap = argparse.ArgumentParser(description="gfx2d 素材打包（SW5）")
    ap.add_argument("--assets", default=os.path.join(main_dir, "assets"),
                    help="素材根目录，默认 Main/assets")
    ap.add_argument("--out", default=None, help="输出目录，默认 <assets>/build")
    ap.add_argument("--manifest", default=None, help="manifest 路径，默认 <assets>/manifest.txt")
    ap.add_argument("--max-pak", type=int, default=FLASH_ASSET_MAX,
                    help="包总长上限（字节），默认 %d（4 MiB Flash 分区）" % FLASH_ASSET_MAX)
    args = ap.parse_args()

    root = os.path.abspath(args.assets)
    out_dir = os.path.abspath(args.out) if args.out else os.path.join(root, "build")
    manifest = args.manifest or os.path.join(root, "manifest.txt")

    if not 0 < args.max_pak <= FLASH_ASSET_MAX:
        print("ERROR: --max-pak 必须在 1..%d 字节内" % FLASH_ASSET_MAX,
              file=sys.stderr)
        return 1

    try:
        rows = read_manifest(manifest)
        items = []
        estimate = HEADER_LEN + DIR_ENTRY_LEN * len(rows)
        for i, (ln, row) in enumerate(rows):
            it = build_item(root, ln, row, i)
            estimate = align_up(estimate, ENTRY_ALIGN) + it.length
            need(estimate <= args.max_pak,
                 "包总长至少 %d 字节，超过运行预算 %d 字节" % (estimate, args.max_pak))
            items.append(it)
        check_unique(items)

        pak, pack_crc = build_pak(items)
        need(len(pak) <= args.max_pak,
             "包总长 %d 字节超过运行预算 %d 字节（Flash 素材分区物理上限 %d）"
             % (len(pak), args.max_pak, FLASH_ASSET_MAX))
    except PakError as exc:
        print("ERROR: %s" % exc, file=sys.stderr)
        return 1

    os.makedirs(out_dir, exist_ok=True)
    pak_path = os.path.join(out_dir, "assets.pak")
    hdr_path = os.path.join(out_dir, "assets.h")
    with open(pak_path, "wb") as f:
        f.write(pak)
    with open(hdr_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(build_header(items, len(pak), pack_crc, args.max_pak))

    print("pack_assets: %d 项，%d 字节 = %.3f MiB，占 4 MiB 分区的 %.1f%%"
          % (len(items), len(pak), len(pak) / 1048576.0,
             len(pak) * 100.0 / FLASH_ASSET_MAX))
    print("  pak crc32 0x%08X  ->  %s" % (pack_crc, os.path.relpath(pak_path, main_dir)))
    for it in items:
        line = ("  [%d] id=%-2d %-12s %-11s %3dx%-3d sl=%2d fmt=%d %8d 字节 crc=%08X"
                % (it.index, it.id, it.name, it.alpha_mode, it.w, it.h,
                   it.sl, it.fmt, it.length, it.crc))
        if it.alpha_mode == "premult":
            line += "  预乘改动 %d 像素/最大 %d 级/进位 %d 处" % (
                it.stats["changed"], it.stats["max_delta"], it.stats["rounded_up"])
        elif it.flags & FLAG_KEY:
            line += "  色键像素 %d" % it.stats["key_px"]
        print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
