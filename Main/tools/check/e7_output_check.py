#!/usr/bin/env python3
"""E7 性能负载的独立输出复算器。

板上 `execSwTest` 的 THR / CMP 两段把每个负载跑完之后的整帧 CRC32
与逐像素加和打出来；
本工具**不看板的任何中间结果**，按《逐像素规格》（spec 第 01 章）与
`lib/gfx/list.c` 的位域，从初始帧缓冲与纹理出发把那一帧重画一遍，
算出同一口径的 CRC32 与逐像素加和。两边相同才算「这些负载确实按规格执行了」。

为什么必须有这一条：`VERIFY` 的十一个整帧用例覆盖的是 SW3 的输入；
性能负载是**另一批输入**（384x256 窗口、2000 个 12x12 弹幕的整表），
没有独立的第二实现时，CRC 对不上无法判断是执行器错了还是探针量错了。

口径与板上逐条对齐：

- 初始帧缓冲：`((x+y)&0xFF)<<16 | ((x*3)&0xFF)<<8 | (y*5)&0xFF`（`seed_fb`）。
- 纹理：`seed_tex` / `seed_cmp_tex`，含 8x8 棋盘色键与预乘契约。
- 混合：spec 第 01 章 3.4 节的 V 顺序，`r255(x) = min(255, (t + (t>>8)) >> 8)`，
  `t = x + 128`；中间量不截断。
- 整帧 CRC：逐行喂 640 个像素（2560 字节），跳过行跨里的间隙（`fb_crc`）。
- 逐像素加和：每个可见像素按小端 32 位字读取，模 2^32 求和；也跳过行跨间隙。
- `s1-exact` 的命令表：与 `build_s1` 逐条相同（同一 xorshift、窗口、色键字）。

用法：

    python3 Main/tools/check/e7_output_check.py <board_log>

退出码 0 要求两面共 18 行的 CRC 与加和均对上，且日志以 `fail 0`、`done` 结束。
"""

import argparse
import re
import struct
import sys
import zlib

FB_W = 640
FB_H = 480
FB_SL = 12                      # stride_log2
SRC_W = SRC_H = 512
SRC_SL = 11
KEY_W = KEY_H = 512
KEY_SL = 11
WIN_W, WIN_H = 384, 256
KEY_COLOR = 0x00123456

EQ_COPY, EQ_ALPHA, EQ_ADD, EQ_CONST, EQ_MUL, EQ_ALPHA_ST = 0, 1, 2, 3, 4, 5
FMT_XRGB, FMT_PM, FMT_ST = 0, 1, 2

MASK = 0xFFFFFFFF

# 与板上 main.c 的 #define 同值。
OFF_BYTES = 8 * 1024 * 1024
TEX_CMP_OFF = OFF_BYTES - 0x00400000
TEX_ST_OFF = OFF_BYTES - 0x00300000
TEX_SRC_OFF = OFF_BYTES - 0x00200000
TEX_KEY_OFF = OFF_BYTES - 0x00100000

# **绝对设备地址**。板上分区指针就是分区偏移（CPU 地址 == 控制器地址，E2 实测），
# 而 `gfx_mem.dev_base` 取的是素材区起点，所以纹理落点是
#   素材区起点 + 离屏层偏移 + TEX_*_OFF。
# 本工具按绝对地址建地址空间，帧缓冲也是绝对地址，两边同一套数才换算得通。
DEV_BASE = 0x01000000          # REGION_ASSET 偏移
OFF_BASE = 0x02800000          # REGION_OFFSCREEN 偏移
FB_BASE = (0x02000000, 0x02400000)   # REGION_FB0 / REGION_FB1 偏移
TEX_CMP = OFF_BASE + TEX_CMP_OFF
TEX_ST = OFF_BASE + TEX_ST_OFF
TEX_SRC = OFF_BASE + TEX_SRC_OFF
TEX_KEY = OFF_BASE + TEX_KEY_OFF
# 板上 `fb_crc` 的 SEED 行给出过 0x6ab1aca3（空帧缓冲），本工具复算得到同一个值，
# 所以下面这个偏移口径与板上一致。

# (名字, 方程, 色键, 是否 BLIT, dx, dy, w, h, argb, c) —— 与 g_loads 同序同值。
LOADS = [
    ("fill-copy",     EQ_COPY,    0, 0,  80,  60, WIN_W, WIN_H, 0x00204060, 0),
    ("fill-const",    EQ_CONST,   0, 0,  80,  60, WIN_W, WIN_H, 0x00010101, 1),
    ("blit-copy",     EQ_COPY,    0, 1, 128,  96, WIN_W, WIN_H, 0,          0),
    ("blit-key",      EQ_COPY,    1, 1,  64,  64, WIN_W, WIN_H, 0,          0),
    ("blit-alpha-pm", EQ_ALPHA,   0, 1, 128,  96, WIN_W, WIN_H, 0,          0),
    ("blit-alpha-st", EQ_ALPHA_ST, 0, 1, 128, 96, WIN_W, WIN_H, 0,          0),
    ("blit-add",      EQ_ADD,     0, 1, 128,  96, WIN_W, WIN_H, 0,          0),
    ("blit-mul",      EQ_MUL,     0, 1, 128,  96, WIN_W, WIN_H, 0,          0),
]

CMP_N = 2000
CMP_SPR = 12
CMP_X0, CMP_X1, CMP_Y0, CMP_Y1 = 100, 540, 100, 340
CMP_HUD = (128, 96, WIN_W, WIN_H)


def r255(x):
    t = (x + 128) & MASK
    q = (t + (t >> 8)) >> 8
    return 255 if q > 255 else q


def blend(srcw, dstw, eq, c, sfmt):
    """spec 第 01 章 3.4 节定稿的 V 顺序，只算 R、G、B。"""
    a = 255 if sfmt == FMT_XRGB else ((srcw >> 24) & 0xFF)
    out = 0
    for i in range(3):
        s = (srcw >> (8 * i)) & 0xFF
        d = (dstw >> (8 * i)) & 0xFF
        if eq == EQ_COPY:
            fs, fd = 255, 0
        elif eq == EQ_ALPHA:
            fs, fd = 255, 255 - a
        elif eq == EQ_ADD:
            fs, fd = 255, 255
        elif eq == EQ_CONST:
            fs, fd = c, 255 - c
        elif eq == EQ_MUL:
            fs, fd = d, 0
        else:
            fs, fd = a, 255 - a
        out |= r255(s * fs + d * fd) << (8 * i)
    return out & 0x00FFFFFF


class Mem:
    """设备地址空间。用 8MB 的字节数组当整块实测域，地址按 dev_base=0 的偏移算。"""

    def __init__(self, size=0x03000000):
        self.buf = bytearray(size)
        self.size = size

    def __setitem__(self, addr, v):
        addr &= ~3
        if addr + 4 <= self.size:
            self.buf[addr:addr + 4] = (v & MASK).to_bytes(4, "little")

    def __getitem__(self, addr):
        addr &= ~3
        if addr + 4 > self.size:
            return 0
        return int.from_bytes(self.buf[addr:addr + 4], "little")

    def crc_rows(self, base, h, sl, w):
        """逐行喂 w 个像素、跳过行跨间隙：与板上 fb_crc 同一口径。"""
        crc = 0
        for y in range(h):
            off = base + (y << sl)
            crc = zlib.crc32(self.buf[off:off + w * 4], crc)
        return crc & MASK

    def pxsum_rows(self, base, h, sl, w):
        total = 0
        for y in range(h):
            off = base + (y << sl)
            total += sum(v for (v,) in struct.iter_unpack("<I", self.buf[off:off + w * 4]))
        return total & MASK


def seed_fb(m, base):
    for y in range(FB_H):
        for x in range(FB_W):
            v = (((x + y) & 0xFF) << 16) | (((x * 3) & 0xFF) << 8) | ((y * 5) & 0xFF)
            m[base + (y << FB_SL) + (x << 2)] = v


def seed_tex(m, base, w, h, sl, fmt, with_key):
    for y in range(h):
        for x in range(w):
            a = (x * 5) & 0xFF
            rr = (x * 7 + y * 3) & 0xFF
            gg = (y * 5 + 17) & 0xFF
            bb = (x + y * 2) & 0xFF
            if fmt == FMT_PM:
                v = (a << 24) | (((a * rr) // 255) << 16) | (((a * gg) // 255) << 8) | ((a * bb) // 255)
            elif fmt == FMT_ST:
                v = (a << 24) | (rr << 16) | (gg << 8) | bb
            else:
                v = (rr << 16) | (gg << 8) | bb
                if with_key and (((x >> 2) + (y >> 2)) & 1) != 0:
                    v = KEY_COLOR
            m[base + (y << sl) + (x << 2)] = v


def seed_cmp_tex(m, base):
    for y in range(SRC_H):
        for x in range(SRC_W):
            a = 64 + ((x * 5 + y * 3) % 192)
            r = (x * 7 + y * 3) & 0xFF
            g = (y * 5 + 17) & 0xFF
            b = (x + y * 2) & 0xFF
            v = (a << 24) | (((a * r) // 255) << 16) | (((a * g) // 255) << 8) | ((a * b) // 255)
            if (((x >> 2) + (y >> 2)) & 1) != 0:
                v = 0
            m[base + (y << SRC_SL) + (x << 2)] = v


def do_fill(m, dst, win, argb, eq, c):
    x0, y0, x1, y1 = win
    for py in range(y0, y1 + 1):
        row = dst + (py << FB_SL)
        for px in range(x0, x1 + 1):
            addr = row + (px << 2)
            m[addr] = blend(argb, m[addr], eq, c, FMT_XRGB)


def do_blit(m, src, dst, win, dx, dy, w, h, eq, c, key_en, key, sfmt, sw, sh, ssl):
    x0, y0, x1, y1 = win
    for py in range(y0, y1 + 1):
        sy = py - dy
        if sy < 0 or sy >= sh:
            continue
        srow = src + (sy << ssl)
        drow = dst + (py << FB_SL)
        for px in range(x0, x1 + 1):
            sx = px - dx
            if sx < 0 or sx >= sw:
                continue
            srcw = m[srow + (sx << 2)]
            if key_en and srcw == key:
                continue
            addr = drow + (px << 2)
            m[addr] = blend(srcw, m[addr], eq, c, sfmt)


def win_of(dx, dy, w, h):
    return (dx, dy, dx + w - 1, dy + h - 1)


def seed_all_tex(m):
    """四块纹理，与板上 main() 和 VERIFY 之后的重铺逐字节相同。

    注意顺序：板上先 `run_verify()`（它会把 `s_tex_key` 当 64x64 纹理改写），
    然后**重新铺一遍**四块纹理再进性能段；本函数对应的是重铺之后的状态。"""
    seed_tex(m, TEX_SRC, SRC_W, SRC_H, SRC_SL, FMT_PM, 0)
    seed_tex(m, TEX_ST, SRC_W, SRC_H, SRC_SL, FMT_ST, 0)
    seed_tex(m, TEX_KEY, KEY_W, KEY_H, KEY_SL, FMT_XRGB, 1)
    seed_cmp_tex(m, TEX_CMP)


def expected(fb_i):
    """算出某个目的面上 THR 与 S1 各负载的整帧 CRC。

    每个负载都从「整帧刚铺成种子值」出发——板上每轮 `seed_fb` 一次，
    所以这里也必须**每轮重铺整帧**。不能用「只把本负载的窗口还原」这种省事的做法：
    FILL 的窗口会被目的表面夹到 [80,463]x[60,315]，BLIT 的窗口会被源表面夹到
    [128,511]x[96,351]，两个窗口不重合，上一轮脏掉的那一角不会被本轮的窗口盖回来。
    （这一条是实际踩到的：只还原窗口时 blit-copy 的整帧 CRC 对不上，
    而它的**窗口** CRC 完全对得上——差全在窗口外。）"""
    fb = FB_BASE[fb_i]
    tex_src = TEX_SRC
    tex_st = TEX_ST
    tex_key = TEX_KEY
    tex_cmp = TEX_CMP

    m = Mem()
    seed_all_tex(m)

    out = {}
    for name, eq, key_en, isblit, dx, dy, w, h, argb, c in LOADS:
        seed_fb(m, fb)
        if not isblit:
            do_fill(m, fb, win_of(dx, dy, w, h), argb, eq, c)
        else:
            if key_en and eq == EQ_COPY:
                s, sw, sh, ssl, sfmt = tex_key, KEY_W, KEY_H, KEY_SL, FMT_XRGB
            elif eq == EQ_ALPHA_ST:
                s, sw, sh, ssl, sfmt = tex_st, SRC_W, SRC_H, SRC_SL, FMT_ST
            elif eq == EQ_ALPHA:
                s, sw, sh, ssl, sfmt = tex_src, SRC_W, SRC_H, SRC_SL, FMT_PM
            else:
                s, sw, sh, ssl, sfmt = tex_src, SRC_W, SRC_H, SRC_SL, FMT_XRGB
            do_blit(m, s, fb, win_of(dx, dy, w, h), dx, dy, w, h, eq, c,
                    key_en, KEY_COLOR, sfmt, sw, sh, ssl)
        out[name] = (m.crc_rows(fb, FB_H, FB_SL, FB_W),
                     m.pxsum_rows(fb, FB_H, FB_SL, FB_W))
    # CMP：整屏 FILL + 带色键 HUD + 2000 个 12x12 预乘 ALPHA 色键弹幕。
    seed_fb(m, fb)
    do_fill(m, fb, (0, 0, FB_W - 1, FB_H - 1), 0x00101820, EQ_COPY, 0)
    hx, hy, hw, hh = CMP_HUD
    do_blit(m, tex_key, fb, win_of(hx, hy, hw, hh), hx, hy, hw, hh,
            EQ_COPY, 0, 1, KEY_COLOR, FMT_XRGB, KEY_W, KEY_H, KEY_SL)
    rng = 0x12345678
    for _ in range(CMP_N):
        rng ^= (rng << 13) & MASK
        rng ^= rng >> 17
        rng ^= (rng << 5) & MASK
        x = CMP_X0 + (rng % (CMP_X1 - CMP_X0))
        rng ^= (rng << 13) & MASK
        rng ^= rng >> 17
        rng ^= (rng << 5) & MASK
        y = CMP_Y0 + (rng % (CMP_Y1 - CMP_Y0))
        do_blit(m, tex_cmp, fb, win_of(x, y, CMP_SPR, CMP_SPR), x, y, CMP_SPR, CMP_SPR,
                EQ_ALPHA, 0, 1, 0, FMT_PM, SRC_W, SRC_H, SRC_SL)
    out["s1-exact"] = (m.crc_rows(fb, FB_H, FB_SL, FB_W),
                        m.pxsum_rows(fb, FB_H, FB_SL, FB_W))
    return out


def seed_fb_win(m, base, dx, dy, w, h):
    """只把窗口还原成 seed_fb 的值：负载之间互不影响（板上每轮都重铺）。"""
    for y in range(dy, dy + h):
        row = base + (y << FB_SL)
        for x in range(dx, dx + w):
            m[row + (x << 2)] = (((x + y) & 0xFF) << 16) | (((x * 3) & 0xFF) << 8) | ((y * 5) & 0xFF)


def parse_log(path):
    """从加载标记之后取出 (名字, fb, crc, pxsum) 及程序结束标记。"""
    out = []
    seen_hdr = False
    fail0 = done = False
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if line.startswith("# ---"):
                seen_hdr = True
                continue
            if line.startswith("#") or not seen_hdr:
                continue
            if line == "fail 0":
                fail0 = True
            if line == "done":
                done = True
            fld = line.split()
            if len(fld) >= 4 and fld[1] in ("fb0", "fb1"):
                try:
                    i = fld.index("crc")
                except ValueError:
                    continue
                if i + 1 >= len(fld):
                    continue
                m = re.fullmatch(r"0x([0-9a-fA-F]{8})", fld[i + 1])
                if m:
                    try:
                        j = fld.index("pxsum")
                        n = re.fullmatch(r"0x([0-9a-fA-F]{8})", fld[j + 1])
                    except (ValueError, IndexError):
                        n = None
                    out.append((fld[0], int(fld[1][2]), int(m.group(1), 16),
                                int(n.group(1), 16) if n else None))
    return out, fail0, done


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    args = ap.parse_args()

    got, fail0, done = parse_log(args.log)
    if not got:
        print("日志里没有 <名字> fb0/fb1 ... crc 0x... 行；产物是不是太旧？")
        return 1

    bad = 0
    if len(got) != 18 or not fail0 or not done:
        print(f"FAIL 日志完整性：负载 {len(got)}/18，fail 0={fail0}，done={done}")
        bad += 1
    for fb_i in (0, 1):
        rows = [(n, c, p) for (n, f, c, p) in got if f == fb_i]
        if not rows:
            bad += 1
            continue
        want = expected(fb_i)
        names = set()
        for name, crc, pxsum in rows:
            if name not in want:
                print(f"FAIL fb{fb_i} 未知负载 {name}")
                bad += 1
                continue
            if name in names:
                print(f"FAIL fb{fb_i} 重复负载 {name}")
                bad += 1
            names.add(name)
            w_crc, w_sum = want[name]
            ok = (w_crc == crc and w_sum == pxsum)
            if not ok:
                bad += 1
            got_sum = f"0x{pxsum:08x}" if pxsum is not None else "缺失"
            print(f"{'ok  ' if ok else 'FAIL'} fb{fb_i} {name:16s} "
                  f"CRC 板 0x{crc:08x}/复算 0x{w_crc:08x} "
                  f"加和 板 {got_sum}/复算 0x{w_sum:08x}")
        missing = [k for k in want if k not in names]
        for k in missing:
            print(f"MISS fb{fb_i} {k:16s} 日志里没有这一行")
            bad += 1

    print(f"--- E7 独立输出复算：{len(got)} 行，{bad} 项不符或缺失")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
