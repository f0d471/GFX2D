#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""gen_stg_patterns.py —— 游戏弹型判据的独立实现与期望文件

游戏的弹型判据：五种弹型（五组参数）各从空场跑 120 帧，另加五个发射器同场的合并段，
每帧的全部敌弹坐标与期望文件逐帧一致。**期望文件由本脚本这份独立实现生成**，
不是把 C 代码跑一遍存下来——那样判据永远通过。

本脚本只按下面的口径独立实现，不读 C 的源码与生成物：

    正弦表   自己算：1024 项一周加 256 项尾巴，Q1.14，按绝对值四舍五入再补回符号（不用 round()）。
    下标     ((a + 32) >> 6) & 1023。
    速度     vx = (speed * cos) >> 14，vy = (speed * sin) >> 14；Python 的 >> 对负数向负无穷，与 gcc 的算术右移同。
    atan2    先把 x、y 同时算术右移到 [-32767, 32767]，y == 0 时按 x 的符号返回 0 或 32768，
             再八分象限归约、一次除法、五项多项式，末步 (dphi + 2) >> 2（Coranac《Off on a tangent》的 atan2Tonc，
             角度单位换成 16 位 BAM）。归约之后 x > 0、y >= 0，所以 (y << 15) // x 只作用在非负数上，
             与 C 的向零取整不会分叉。其余中间量都在 int32 以内，Python 的无界整数给出同一个值。
    随机数   xorshift32（13、17、5），种子为 0 时改成 1，每一步都截到 32 位。
    发射器   定法：倒数计时、repeat 为总次数、三种方向模式、逐发步进、随机雨先 x 后速度、方向帧。
    池       追加到末尾，满（16384）拒绝并计溢出；一遍更新先判移除标记、再加速度、再判出界（半开区间），
             存活的按原顺序留下。弹型判据不跑碰撞。

输出格式（与 app 测试 `stgTest pat` 逐字相同）：

    seg <段号> <段名> seed=0x<种子> player=<px>,<py>
    emit <序号> count=.. dir=.. kind=.. angle0=.. angle_step=.. spin=.. interval=.. repeat=.. speed=..
         speed_step=.. rand_x=.. rand_speed=.. x=.. y=.. hvx=.. hx_lo=.. hx_hi=..     （一行，不折行）
    f <帧号> <敌弹存活数> <CRC32 八位小写十六进制>
    end <段号> overflow <溢出计数>

CRC32 用 zlib.crc32，对全部敌弹按池顺序、每颗 struct.pack('<iiiiHH', x, y, vx, vy, sprite, flags)。

用法：

    python3 Main/tools/check/gen_stg_patterns.py                 写进 Main/sw/apps/game/gfx2dGame/test/stg_patterns.txt（入库的那份）
    python3 Main/tools/check/gen_stg_patterns.py --out <文件>    写到别处（判据用）
    python3 Main/tools/check/gen_stg_patterns.py --dump <段> <帧>   打出某段某帧之后的全部敌弹（格式同 `stgTest dump`）
    python3 Main/tools/check/gen_stg_patterns.py --compare <C 的轨迹> [--expect <期望文件>] [--c-bin <stgTest>]
        先比参数行，再逐帧比，末行比溢出计数；不一致时打出第一处不同，给了 --c-bin 就再让两边各 dump
        那一帧、指出第一颗不同的弹。一致退出 0，不一致退出 1。

只用标准库。
"""

import argparse
import math
import os
import struct
import subprocess
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_OUT = os.path.normpath(os.path.join(
    HERE, "..", "..", "sw", "apps", "game", "gfx2dGame", "test", "stg_patterns.txt"))

FRAMES = 120
ONE = 256                       # Q.8 的 1 像素
M32 = 0xFFFFFFFF

# --- 2.1 节：正弦表、下标、速度、atan2 -------------------------------------

SIN_N = 1024
SIN_TAIL = 256
SIN_ONE = 16384


def _round_half_away(v):
    r = int(math.floor(abs(v) + 0.5))
    return -r if v < 0 else r


SIN_TAB = [_round_half_away(SIN_ONE * math.sin(2.0 * math.pi * i / SIN_N)) for i in range(SIN_N)]
SIN_TAB += SIN_TAB[:SIN_TAIL]


def bam_index(a):
    return ((a + 32) >> 6) & (SIN_N - 1)


def vel(speed, a):
    assert -(1 << 17) < speed < (1 << 17), "速度超出 fx_vel 的契约"
    i = bam_index(a)
    return (speed * SIN_TAB[i + SIN_TAIL]) >> 14, (speed * SIN_TAB[i]) >> 14


def atan2_bam(y, x):
    """返回 16 位 BAM，参数顺序同 atan2(y, x)。"""
    while x > 32767 or x < -32767 or y > 32767 or y < -32767:
        x >>= 1
        y >>= 1
    if y == 0:
        return 0 if x >= 0 else 0x8000
    octant = 0
    if y < 0:                           # 转过 π
        x, y = -x, -y
        octant += 4
    if x <= 0:                          # 转过 π/2
        x, y = y, -x
        octant += 2
    if x <= y:                          # 转过 π/4
        x, y = x + y, y - x
        octant += 1
    assert x > 0 and y >= 0
    t = (y << 15) // x                  # 非负除以正数：与 C 的向零取整相同
    t2 = (-t * t) >> 15
    dphi = 0x0470
    dphi = 0x1029 + ((t2 * dphi) >> 15)
    dphi = 0x1F0B + ((t2 * dphi) >> 15)
    dphi = 0x364C + ((t2 * dphi) >> 15)
    dphi = 0xA2FC + ((t2 * dphi) >> 15)
    dphi = (dphi * t) >> 15
    return (octant * 0x2000 + ((dphi + 2) >> 2)) & 0xFFFF


# --- 随机数 ---------------------------------------------------------------

class XorShift32:
    def __init__(self, seed):
        self.s = seed if seed != 0 else 1

    def next(self):
        x = self.s
        x ^= (x << 13) & M32
        x ^= x >> 17
        x ^= (x << 5) & M32
        self.s = x
        return x


# --- 2.2 节：池 -----------------------------------------------------------

CAP_EBULLET = 16384
F_REMOVE = 0x8000
MARGIN = 10
FIELD_W, FIELD_H = 400, 480
X_LO, X_HI = -MARGIN * ONE, (FIELD_W + MARGIN) * ONE
Y_LO, Y_HI = -MARGIN * ONE, (FIELD_H + MARGIN) * ONE


class Pool:
    def __init__(self, cap):
        self.cap = cap
        self.ents = []                  # 每颗 [x, y, vx, vy, sprite, flags]
        self.overflow = 0

    def add(self, x, y, vx, vy, sprite, flags):
        if len(self.ents) >= self.cap:
            self.overflow += 1
            return
        self.ents.append([x, y, vx, vy, sprite, flags & ~F_REMOVE & 0xFFFF])

    def step(self):
        keep = []
        for x, y, vx, vy, sprite, flags in self.ents:
            if flags & F_REMOVE:
                continue
            nx, ny = x + vx, y + vy
            if not (X_LO <= nx < X_HI and Y_LO <= ny < Y_HI):
                continue
            keep.append([nx, ny, vx, vy, sprite, flags])
        self.ents = keep

    def crc(self):
        c = 0
        for e in self.ents:
            c = zlib.crc32(struct.pack("<iiiiHH", *e), c)
        return c & M32


# --- 弹种表、发射记录与五组参数 ----------------------------------

ABS, AIM, SEQ = 0, 1, 2
# 游戏的弹种表：0~3 圆弹、4~7 小圆弹、8~11 米弹，各按颜色号 0~3（青、品红、琥珀、绿）
ROUND16, SMALL8, RICE20 = 0, 4, 8
CYAN, MAGENTA, AMBER, GREEN = 0, 1, 2, 3

# 弹种 -> (起始帧, 有无方向帧)。只抄帧号需要的两项，图像与半径不进弹型判据。
# 圆弹与小圆弹的帧号是颜色号；米弹是颜色号 x 16 再加方向。
KINDS = {}
for _c in (CYAN, MAGENTA, AMBER, GREEN):
    KINDS[ROUND16 + _c] = (_c, False)
    KINDS[SMALL8 + _c] = (_c, False)
    KINDS[RICE20 + _c] = (_c * 16, True)


def sprite_of(kind, a):
    first, directional = KINDS[kind]
    if not directional:
        return first
    return first + (((a + 2048) >> 12) & 15)


def px(v):
    return v * ONE


SEED = 0x5E7B0001
PLAYER = (px(200), px(400))

# 段名、发射记录、发射器起点、宿主 (vx, x_lo, x_hi)。照游戏「五组参数」的表另抄一份。
PATTERNS = [
    ("ring", dict(count=32, dir=ABS, kind=ROUND16 + CYAN, angle0=0, angle_step=2048, spin=0,
                  interval=20, repeat=0, speed=384, speed_step=0, rand_x=0, rand_speed=0),
     (px(200), px(160)), (0, 0, 0)),
    ("spiral", dict(count=3, dir=SEQ, kind=SMALL8 + MAGENTA, angle0=0, angle_step=21845, spin=1200,
                    interval=1, repeat=0, speed=512, speed_step=0, rand_x=0, rand_speed=0),
     (px(200), px(200)), (0, 0, 0)),
    ("aim", dict(count=5, dir=AIM, kind=RICE20 + AMBER, angle0=(-4096) & 0xFFFF, angle_step=2048, spin=0,
                 interval=15, repeat=0, speed=768, speed_step=0, rand_x=0, rand_speed=0),
     (px(200), px(80)), (768, px(80), px(320))),
    ("fan", dict(count=7, dir=ABS, kind=RICE20 + GREEN, angle0=16384 - 3000, angle_step=1000, spin=0,
                 interval=10, repeat=6, speed=640, speed_step=64, rand_x=0, rand_speed=0),
     (px(200), px(120)), (0, 0, 0)),
    ("rain", dict(count=4, dir=ABS, kind=SMALL8 + CYAN, angle0=16384, angle_step=0, spin=0,
                  interval=3, repeat=0, speed=256, speed_step=0, rand_x=190 * ONE, rand_speed=2 * ONE),
     (px(200), px(0)), (0, 0, 0)),
]
SEG_MIX = 5
SEG_N = 6


def seg_name(seg):
    return "mix" if seg == SEG_MIX else PATTERNS[seg][0]


class Emitter:
    def __init__(self, rec, pos, host):
        self.rec = rec
        self.x, self.y = pos
        self.hvx, self.hx_lo, self.hx_hi = host
        self.wait = 0                   # 倒数：为 0 的那一帧齐射
        self.volleys = 0
        self.seq = rec["angle0"]        # SEQ 的累计角

    def done(self):
        return self.rec["repeat"] != 0 and self.volleys >= self.rec["repeat"]

    def tick(self, pool, player, rng):
        r = self.rec
        if self.done():
            return
        if self.wait != 0:
            self.wait -= 1
            return
        if r["dir"] == AIM:
            base = (atan2_bam(player[1] - self.y, player[0] - self.x) + r["angle0"]) & 0xFFFF
        elif r["dir"] == SEQ:
            base = self.seq
            self.seq = (self.seq + r["spin"]) & 0xFFFF
        else:
            base = r["angle0"]
        for j in range(r["count"]):
            a = (base + j * r["angle_step"]) & 0xFFFF
            speed = r["speed"] + j * r["speed_step"]
            bx = self.x
            if r["rand_x"] != 0:        # 先取 x
                bx += rng.next() % (2 * r["rand_x"] + 1) - r["rand_x"]
            if r["rand_speed"] != 0:    # 再取速度
                speed += rng.next() % (r["rand_speed"] + 1)
            vx, vy = vel(speed, a)
            pool.add(bx, self.y, vx, vy, sprite_of(r["kind"], a), r["kind"])
        self.volleys += 1
        self.wait = r["interval"] - 1

    def host_move(self):
        if self.hvx == 0:
            return
        self.x += self.hvx
        if self.x < self.hx_lo or self.x >= self.hx_hi:
            self.hvx = -self.hvx


class Field:
    def __init__(self, seg):
        self.pool = Pool(CAP_EBULLET)
        self.rng = XorShift32(SEED)
        self.player = PLAYER
        idx = range(len(PATTERNS)) if seg == SEG_MIX else [seg]
        self.ems = [Emitter(PATTERNS[i][1], PATTERNS[i][2], PATTERNS[i][3]) for i in idx]

    def advance(self):
        """一帧的 ①②：池更新，发射器按数组顺序齐射，各自的宿主随后移动。"""
        self.pool.step()
        for em in self.ems:
            em.tick(self.pool, self.player, self.rng)
            em.host_move()


def param_lines(seg, field):
    out = ["seg %d %s seed=0x%08x player=%d,%d" % (seg, seg_name(seg), SEED, field.player[0], field.player[1])]
    for i, em in enumerate(field.ems):
        r = em.rec
        out.append(
            "emit %d count=%d dir=%d kind=%d angle0=%d angle_step=%d spin=%d interval=%d repeat=%d"
            " speed=%d speed_step=%d rand_x=%d rand_speed=%d x=%d y=%d hvx=%d hx_lo=%d hx_hi=%d"
            % (i, r["count"], r["dir"], r["kind"], r["angle0"], r["angle_step"], r["spin"],
               r["interval"], r["repeat"], r["speed"], r["speed_step"], r["rand_x"], r["rand_speed"],
               em.x, em.y, em.hvx, em.hx_lo, em.hx_hi))
    return out


def trace():
    out = []
    for seg in range(SEG_N):
        f = Field(seg)
        out += param_lines(seg, f)
        for k in range(FRAMES):
            f.advance()
            out.append("f %d %d %08x" % (k, len(f.pool.ents), f.pool.crc()))
        out.append("end %d overflow %d" % (seg, f.pool.overflow))
    return "\n".join(out) + "\n"


def dump(seg, frame):
    f = Field(seg)
    for _ in range(frame + 1):
        f.advance()
    out = ["dump %d %d %d" % (seg, frame, len(f.pool.ents))]
    for i, e in enumerate(f.pool.ents):
        out.append("b %d %d %d %d %d %d %d" % (i, *e))
    return "\n".join(out) + "\n"


# --- 比对 -----------------------------------------------------------------

def split_segments(text):
    """{段号: {"params": [...], "frames": [...], "end": str}}"""
    segs = {}
    cur = None
    for line in text.splitlines():
        if line.startswith("seg "):
            cur = int(line.split()[1])
            segs[cur] = {"params": [line], "frames": [], "end": None}
        elif cur is None:
            continue
        elif line.startswith("emit "):
            segs[cur]["params"].append(line)
        elif line.startswith("f "):
            segs[cur]["frames"].append(line)
        elif line.startswith("end "):
            segs[cur]["end"] = line
    return segs


def first_bullet_diff(dump_a, dump_b):
    a = [l for l in dump_a.splitlines() if l.startswith("b ")]
    b = [l for l in dump_b.splitlines() if l.startswith("b ")]
    for i in range(max(len(a), len(b))):
        la = a[i] if i < len(a) else "(无)"
        lb = b[i] if i < len(b) else "(无)"
        if la != lb:
            return i, la, lb
    return None


def compare(c_text, expect_text, c_bin):
    got = split_segments(c_text)
    want = split_segments(expect_text)
    bad = 0
    for seg in range(SEG_N):
        if seg not in got:
            print("seg %d: C 侧没有这一段" % seg)
            bad += 1
            continue
        g, w = got[seg], want[seg]
        if g["params"] != w["params"]:
            print("seg %d: 参数不一致" % seg)
            for lg, lw in zip(g["params"], w["params"]):
                if lg != lw:
                    print("  C      " + lg)
                    print("  期望   " + lw)
            if len(g["params"]) != len(w["params"]):
                print("  发射器个数 C %d、期望 %d" % (len(g["params"]) - 1, len(w["params"]) - 1))
            bad += 1
            continue
        diff_frame = None
        for k in range(max(len(g["frames"]), len(w["frames"]))):
            lg = g["frames"][k] if k < len(g["frames"]) else "(无)"
            lw = w["frames"][k] if k < len(w["frames"]) else "(无)"
            if lg != lw:
                diff_frame = k
                print("seg %d %s: 第 %d 帧不一致" % (seg, seg_name(seg), k))
                print("  C      " + lg)
                print("  期望   " + lw)
                break
        if diff_frame is not None:
            bad += 1
            if c_bin:
                c_dump = subprocess.run([c_bin, "dump", str(seg), str(diff_frame)],
                                        capture_output=True, text=True).stdout
                p_dump = dump(seg, diff_frame)
                print("  --- C 的第 %d 帧 ---" % diff_frame)
                sys.stdout.write("".join("  " + l + "\n" for l in c_dump.splitlines()))
                print("  --- 期望的第 %d 帧 ---" % diff_frame)
                sys.stdout.write("".join("  " + l + "\n" for l in p_dump.splitlines()))
                d = first_bullet_diff(c_dump, p_dump)
                if d is not None:
                    print("  第一颗不同的弹：第 %d 颗" % d[0])
                    print("    C      " + d[1])
                    print("    期望   " + d[2])
            continue
        if g["end"] != w["end"]:
            print("seg %d: 末行不一致：C '%s'，期望 '%s'" % (seg, g["end"], w["end"]))
            bad += 1
    return bad


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", help="写到这个文件（缺省是入库的期望文件）")
    ap.add_argument("--dump", nargs=2, type=int, metavar=("SEG", "FRAME"))
    ap.add_argument("--compare", metavar="C_TRACE")
    ap.add_argument("--expect", default=DEFAULT_OUT)
    ap.add_argument("--c-bin")
    args = ap.parse_args()

    if args.dump:
        seg, frame = args.dump
        if not (0 <= seg < SEG_N and 0 <= frame < FRAMES):
            print("段号或帧号越界", file=sys.stderr)
            return 2
        sys.stdout.write(dump(seg, frame))
        return 0

    if args.compare:
        with open(args.compare, encoding="utf-8") as fp:
            c_text = fp.read()
        with open(args.expect, encoding="utf-8") as fp:
            expect_text = fp.read()
        bad = compare(c_text, expect_text, args.c_bin)
        print("compare %s" % ("ok" if bad == 0 else "FAIL %d" % bad))
        return 0 if bad == 0 else 1

    text = trace()
    out = args.out or DEFAULT_OUT
    with open(out, "w", encoding="utf-8", newline="\n") as fp:
        fp.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
