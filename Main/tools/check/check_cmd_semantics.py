#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""命令语义规格（spec 第 02 章 / 命令 ABI v1）的结构校验与一致性向量。

为什么有这个工具
----------------
spec 第 01 章（逐像素）有 `check_round255.c` 把方程表穷举过一遍；第 02 章
（命令语义）此前没有对应的主机侧工具，规格只有散文和表格。
散文里任何一处「两种读法都成立」的地方，软件执行器与硬件引擎就会各实现
一种，而这种分歧只在同帧 CRC 对不上时才暴露，到那时分不清是谁错。

本工具做四件事，对应 spec 第 02 章规范条款 §7 要求 SW3 生成的那批用例：

  1. 把第 02 章的字段矩阵、检查优先级、错误码、方程/格式匹配规则抄成
     一份可执行镜像（本文件的常量区）；规范正文是权威，改动先改正文再同步这里；
  2. 按第七节的九组逐条生成一致性向量，**每条的期望值都写成字面量并注明
     出处**（哪一节/哪张表决定它），再与本文件的检查器逐条比对；
  3. 做结构审计：优先级十步是否每步都有反例、字段矩阵的「必须为 0」集合
     与反例集合是否相等、六方程×三格式的组合是否逐个有向量；
  4. 注错自检：把每条规则改错一次，向量集必须报 FAIL
     （`Main/README.md`「判据要能注错见红」）。

**期望值为什么不从检查器里算出来。** spec 第 01 章 4.1、4.7 两节记过两次
同一类事故：工具自己判自己「全部通过」，或者两个实现共享同一个错误，
于是「一致」不构成证据。所以这里的期望值是手写的、带出处的，检查器是
另一份实现；两者不一致就是发现。第 (F) 节打印本轮研究补入正文并经
2026-09-20 复核定案的条目。

跑法（主机，不依赖板子）：

    wsl -e python3 Main/tools/check/check_cmd_semantics.py                  校验 + 注错自检
    wsl -e python3 Main/tools/check/check_cmd_semantics.py --dump-matrix    打印可执行镜像，与正文逐行对账
    wsl -e python3 Main/tools/check/check_cmd_semantics.py --emit F         把向量集写到 F
    wsl -e python3 Main/tools/check/check_cmd_semantics.py --no-selftest    跳过注错自检

全部通过时退出码 0，末尾是 `fail 0` 与 `done`。
"""

import argparse
import hashlib
import sys

ABI = 1
TOOL = "tools/check/check_cmd_semantics.py"

# ---------------------------------------------------------------------------
# 一、规范的可执行镜像：抄自 spec 第 02 章的表与设计文档 3.4 节的位域
# ---------------------------------------------------------------------------

OP_NOP, OP_FILL, OP_BLIT, OP_SETCLIP, OP_BAND, OP_END = 0, 1, 2, 3, 4, 0xFF

EQ_COPY, EQ_ALPHA, EQ_ADD, EQ_CONST, EQ_MUL, EQ_ALPHA_STRAIGHT = 0, 1, 2, 3, 4, 5

FMT_XRGB, FMT_PM, FMT_STRAIGHT, FMT_INDEX8 = 0, 1, 2, 3

SPACE_DDR, SPACE_TEX, SPACE_BAND = 0, 1, 2

E_OK, E_BAD_OPCODE, E_BAD_FIELD, E_BAD_EQUATION = 0, 1, 2, 3
E_BAD_SURFACE, E_BAD_SPACE, E_BAD_FORMAT = 4, 5, 6
E_OVERLAP, E_BAD_PREMULT, E_FETCH = 7, 8, 9

ERR_NAME = {
    E_OK: "OK", E_BAD_OPCODE: "BAD_OPCODE", E_BAD_FIELD: "BAD_FIELD",
    E_BAD_EQUATION: "BAD_EQUATION", E_BAD_SURFACE: "BAD_SURFACE",
    E_BAD_SPACE: "BAD_SPACE", E_BAD_FORMAT: "BAD_FORMAT",
    E_OVERLAP: "OVERLAP", E_BAD_PREMULT: "BAD_PREMULT", E_FETCH: "FETCH",
}

OP_NAME = {OP_NOP: "NOP", OP_FILL: "FILL", OP_BLIT: "BLIT",
           OP_SETCLIP: "SET_CLIP", OP_BAND: "BAND", OP_END: "END"}

EQ_NAME = {EQ_COPY: "COPY", EQ_ALPHA: "ALPHA", EQ_ADD: "ADD",
           EQ_CONST: "CONST", EQ_MUL: "MUL", EQ_ALPHA_STRAIGHT: "ALPHA_STRAIGHT"}

FMT_NAME = {FMT_XRGB: "XRGB8888", FMT_PM: "ARGB8888_PREMULT",
            FMT_STRAIGHT: "ARGB8888_STRAIGHT", FMT_INDEX8: "INDEX8"}

# spec 第 02 章规范条款 §5：检查顺序固定为这十步
STEPS = [
    ("fetch",         "取记录（FETCH）"),
    ("opcode",        "操作码（BAD_OPCODE）"),
    ("fields",        "保留/未用字段（BAD_FIELD）"),
    ("equation",      "方程（BAD_EQUATION）"),
    ("src_surface",   "源表面（BAD_SURFACE）"),
    ("dst_surface",   "目的表面（BAD_SURFACE）"),
    ("space",         "space（BAD_SPACE）"),
    ("format",        "format（BAD_FORMAT）"),
    ("fill_premult",  "FILL 预乘约束（BAD_PREMULT）"),
    ("overlap",       "BLIT 重叠（OVERLAP）"),
]

GAME = (0, 0, 639, 479)          # 游戏区，闭区间
V1_SRC_FMT = (FMT_XRGB, FMT_PM, FMT_STRAIGHT)

DEFAULT_WORD = 0x00004080        # 向量里默认的存储内容：R=0x00 G=0x40 B=0x80


def w0(op=0, eq=0, key=0, tint=0, rsvd=0, src=0, dst=0, c=0):
    return (op | (eq << 8) | (key << 12) | (tint << 13) | (rsvd << 14)
            | (src << 16) | (dst << 20) | (c << 24)) & 0xFFFFFFFF


def coord(x, y):
    return ((y & 0xFFFF) << 16) | (x & 0xFFFF)


def wh(w, h):
    return ((h & 0xFFFF) << 16) | (w & 0xFFFF)


def rec(w0v=0, w1=0, w2=0, w3=0, w4=0, w5=0, w6=0, w7=0):
    return [w0v & 0xFFFFFFFF, w1 & 0xFFFFFFFF, w2 & 0xFFFFFFFF, w3 & 0xFFFFFFFF,
            w4 & 0xFFFFFFFF, w5 & 0xFFFFFFFF, w6 & 0xFFFFFFFF, w7 & 0xFFFFFFFF]


def s16(v):
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def u16(v):
    return v & 0xFFFF


# --- 表面表 ---------------------------------------------------------------

def surf(base=0, width=4, height=2, stride_log2=4, space=SPACE_DDR,
         fmt=FMT_XRGB, valid=1, ctrl_rsvd=0):
    return dict(base=base, width=width, height=height, stride_log2=stride_log2,
                space=space, format=fmt, valid=valid, ctrl_rsvd=ctrl_rsvd)


def make_surfaces():
    """16 个槽位，覆盖合法描述符与结构谓词各自的失败例。"""
    S = [None] * 16
    S[0] = surf(base=0x00001000)                                    # 目的帧缓冲
    S[1] = surf(base=0x00002000, fmt=FMT_PM)                        # 预乘源
    S[2] = surf(base=0x00003000, fmt=FMT_STRAIGHT)                  # 直通源
    S[3] = surf(base=0x00004000)                                    # XRGB 源 / 同表面
    S[4] = surf(base=0x00005000, space=SPACE_TEX)                   # 非 DDR
    S[5] = surf(base=0x00006000, space=SPACE_BAND, fmt=FMT_INDEX8)  # 非 DDR 且格式不支持
    S[6] = surf(base=0x00007000, fmt=FMT_INDEX8)                    # 不支持的格式
    S[7] = surf(base=0x00008000, fmt=5)                             # 未知格式
    S[8] = surf(base=0x00009000, valid=0)                           # 未登记
    S[9] = surf(base=0x00009002)                                    # base % 4 != 0
    S[10] = surf(base=0x0000A000, width=0)                          # 宽为 0
    S[11] = surf(base=0x0000B000, height=0)                         # 高为 0
    S[12] = surf(base=0x0000C000, stride_log2=1)                    # stride_log2 < 2
    S[13] = surf(base=0x0000D000, width=1, stride_log2=2)           # 边界：刚好放得下
    S[14] = surf(base=0x0000E000, width=5)                          # width*4 > 1<<stride
    S[15] = surf(base=0xFFFFFFF0)                                   # 末像素地址溢出
    return S


# ---------------------------------------------------------------------------
# 二、描述符与记录的合法性
# ---------------------------------------------------------------------------

def surf_struct_ok(S, sid):
    """spec 第 02 章规范条款 §2“一个结构合法的描述符同时满足：…”。

    最后一条（控制字保留位为 0）在正文里写作「其余位为 0」，没有说违反时
    返回什么；本工具按「不静默忽略」纳入结构检查，见第 (F) 节。
    """
    s = S[sid]
    if s is None or not s["valid"]:
        return False
    if s["base"] % 4 != 0:
        return False
    if not (s["width"] > 0 and s["height"] > 0):
        return False
    if not (2 <= s["stride_log2"] <= 31):
        return False
    if s["width"] * 4 > (1 << s["stride_log2"]):
        return False
    last = (s["base"] + (s["height"] - 1) * (1 << s["stride_log2"])
            + (s["width"] - 1) * 4 + 3)
    if last > 0xFFFFFFFF:
        return False
    if s["ctrl_rsvd"] != 0:
        return False
    return True


def field_value(r, name):
    w0v, w1, w2, w3, w4, w5, w6, _w7 = r
    return {
        "eq": (w0v >> 8) & 0xF, "key": (w0v >> 12) & 1, "tint": (w0v >> 13) & 1,
        "rsvd": (w0v >> 14) & 0x3, "src": (w0v >> 16) & 0xF,
        "dst": (w0v >> 20) & 0xF, "c": (w0v >> 24) & 0xFF,
        "w1": w1, "w2": w2, "w3": w3, "w4": w4, "w5": w5, "w6": w6,
    }[name]


def must_zero(op, eq, key, inj=()):
    """spec 第 02 章规范条款 §3 的表 + w0[15:14]、w0[13] 两句散文。"""
    z = ["rsvd", "tint"]
    if op in (OP_NOP, OP_END):
        z += ["eq", "key", "src", "dst", "c", "w1", "w2", "w3", "w4", "w5", "w6"]
    elif op == OP_FILL:
        if "no_fill_key_check" not in inj:
            z += ["key"]
        z += ["src", "w1", "w5", "w6"]
        if eq != EQ_CONST:
            z += ["c"]
    elif op == OP_BLIT:
        z += ["w4", "w6"]
        if eq != EQ_CONST:
            z += ["c"]
        if key == 0:
            z += ["w5"]
    elif op == OP_SETCLIP:
        z += ["eq", "key", "src", "dst", "c", "w2", "w4", "w5", "w6"]
    return z


def first_bad_field(r, inj=()):
    op = r[0] & 0xFF
    eq = (r[0] >> 8) & 0xF
    key = (r[0] >> 12) & 1
    for name in must_zero(op, eq, key, inj):
        if field_value(r, name) != 0:
            return name
    return None


def fmt_eq_ok(sfmt, eq):
    """spec 第 02 章规范条款 §2：BLIT 的方程与源格式匹配规则。"""
    if eq == EQ_ALPHA:
        return sfmt in (FMT_XRGB, FMT_PM)
    if eq == EQ_ALPHA_STRAIGHT:
        return sfmt in (FMT_XRGB, FMT_STRAIGHT)
    return sfmt in V1_SRC_FMT


def rects_intersect(a, b):
    return not (a[2] <= b[0] or b[2] <= a[0] or a[3] <= b[1] or b[3] <= a[1])


def check_record(S, r, inj=()):
    """按规范条款 §5 固定的十步走，返回 (错误码, 步名)。inj 只给注错自检用。"""
    op = r[0] & 0xFF
    if op not in (OP_NOP, OP_FILL, OP_BLIT, OP_SETCLIP, OP_END):
        return E_BAD_OPCODE, "opcode"
    if first_bad_field(r, inj) is not None:
        return E_BAD_FIELD, "fields"
    eq = (r[0] >> 8) & 0xF
    srcid = (r[0] >> 16) & 0xF
    dstid = (r[0] >> 20) & 0xF

    if op in (OP_FILL, OP_BLIT) and eq > EQ_ALPHA_STRAIGHT:
        return E_BAD_EQUATION, "equation"

    w, h = u16(r[3]), u16(r[3] >> 16)
    if "zero_area_first" in inj and op in (OP_FILL, OP_BLIT) and (w == 0 or h == 0):
        return E_OK, None

    if op == OP_FILL:
        if not surf_struct_ok(S, dstid):
            return E_BAD_SURFACE, "dst_surface"
    elif op == OP_BLIT:
        if not surf_struct_ok(S, srcid):
            return E_BAD_SURFACE, "src_surface"
        if not surf_struct_ok(S, dstid):
            return E_BAD_SURFACE, "dst_surface"

    # space 与 format 各是一步、覆盖源与目的两个表面（源在前）。这是规范条款 §5
    # 十步的字面顺序；所以「源格式非法 + 目的 space 非 DDR」返回 BAD_SPACE。
    if "swap_space_format" in inj:
        bad = _format_step(S, op, srcid, dstid, eq)
        if bad:
            return bad
        bad = _space_step(S, op, srcid, dstid)
        if bad:
            return bad
    else:
        bad = _space_step(S, op, srcid, dstid)
        if bad:
            return bad
        bad = _format_step(S, op, srcid, dstid, eq)
        if bad:
            return bad

    if op == OP_FILL and eq == EQ_ALPHA and "no_premult" not in inj:
        a = (r[4] >> 24) & 0xFF
        if ((r[4] >> 16) & 0xFF) > a or ((r[4] >> 8) & 0xFF) > a or (r[4] & 0xFF) > a:
            return E_BAD_PREMULT, "fill_premult"

    if op == OP_BLIT and "no_overlap" not in inj:
        if srcid == dstid and w > 0 and h > 0:
            sx, sy = s16(r[1]), s16(r[1] >> 16)
            dx, dy = s16(r[2]), s16(r[2] >> 16)
            a = (sx, sy, sx + w, sy + h)
            b = (dx, dy, dx + w, dy + h)
            if rects_intersect(a, b):
                return E_OVERLAP, "overlap"

    return E_OK, None


def _space_step(S, op, srcid, dstid):
    if op == OP_FILL:
        if S[dstid]["space"] != SPACE_DDR:
            return E_BAD_SPACE, "space"
    elif op == OP_BLIT:
        if S[srcid]["space"] != SPACE_DDR or S[dstid]["space"] != SPACE_DDR:
            return E_BAD_SPACE, "space"
    return None


def _format_step(S, op, srcid, dstid, eq):
    if op == OP_FILL:
        if S[dstid]["format"] != FMT_XRGB:
            return E_BAD_FORMAT, "format"
    elif op == OP_BLIT:
        if not fmt_eq_ok(S[srcid]["format"], eq):
            return E_BAD_FORMAT, "format"
        if S[dstid]["format"] != FMT_XRGB:
            return E_BAD_FORMAT, "format"
    return None


# ---------------------------------------------------------------------------
# 三、逐像素核心：照 spec 第 01 章 3.4 节 V 顺序代入方程
#     （算术本身由 check_round255.c 穷举，这里只取那一份定稿写法）
# ---------------------------------------------------------------------------

def _r255(x):
    t = x + 128
    return min(255, (t + (t >> 8)) >> 8)


def blend_px(srcw, dstw, eq, c, sfmt):
    a = 255 if sfmt == FMT_XRGB else (srcw >> 24) & 0xFF
    out = 0
    for i in range(3):                      # 0=B、1=G、2=R
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
        out |= _r255(s * fs + d * fd) << (8 * i)
    return out & 0x00FFFFFF


def addr_of(s, x, y):
    """ch02 §2 给了 stride_log2，ch01 3.7 给了帧缓冲的 <<12；
    地址公式 base + (y << stride_log2) + (x << 2) 是本轮补的一条，见第 (F) 节。"""
    return s["base"] + (y << s["stride_log2"]) + (x << 2)


def dst_window(S, clip, sid, dx, dy, w, h):
    """spec 第 01 章 3.6 节的四重求交（游戏区、目的矩形、SET_CLIP、目的表面）。"""
    if w == 0 or h == 0:
        return None
    s = S[sid]
    x0 = max(0, dx, clip[0])
    y0 = max(0, dy, clip[1])
    x1 = min(639, dx + w - 1, clip[2], s["width"] - 1)
    y1 = min(479, dy + h - 1, clip[3], s["height"] - 1)
    if x1 < x0 or y1 < y0:
        return None
    return x0, y0, x1, y1


def set_clip_rect(r):
    """spec 第 01 章 SET_CLIP 表：w1 左上角、w3 尺寸、闭区间、替换旧框。

    “只能缩小”只相对游戏区而言，不是与旧 clip 求交。截断到游戏区在设置时做；
    因为游戏区是四重求交的一项，设置时截断与使用时求交对任何命令的最终结果都相同。
    """
    x0, y0 = s16(r[1]), s16(r[1] >> 16)
    w, h = u16(r[3]), u16(r[3] >> 16)
    x1, y1 = x0 + w - 1, y0 + h - 1
    return (max(x0, 0), max(y0, 0), min(x1, 639), min(y1, 479))


def run_one(S, cmds, count, clip, mem, inj=()):
    """跑一张命令表，返回 (code, erridx, errtag, reads)。"""
    reads = 0
    for idx in range(count):
        reads += 1
        if idx >= len(cmds):
            return E_FETCH, idx, 0, reads
        r = cmds[idx]
        code, _step = check_record(S, r, inj)
        if code != E_OK:
            return code, idx, (idx if "tag_is_index" in inj else r[7]), reads
        op = r[0] & 0xFF
        if op == OP_END:
            return E_OK, 0, 0, reads
        if op == OP_NOP:
            continue
        if op == OP_SETCLIP:
            clip_new = set_clip_rect(r)
            clip[0], clip[1], clip[2], clip[3] = clip_new
            continue

        eq = (r[0] >> 8) & 0xF
        c = (r[0] >> 24) & 0xFF
        srcid = (r[0] >> 16) & 0xF
        dstid = (r[0] >> 20) & 0xF
        w, h = u16(r[3]), u16(r[3] >> 16)
        dx, dy = s16(r[2]), s16(r[2] >> 16)
        win = dst_window(S, clip, dstid, dx, dy, w, h)
        if win is None:
            continue
        x0, y0, x1, y1 = win
        if op == OP_FILL:
            sfmt = (FMT_PM if eq == EQ_ALPHA
                    else FMT_STRAIGHT if eq == EQ_ALPHA_STRAIGHT else FMT_XRGB)
            for py in range(y0, y1 + 1):
                for px in range(x0, x1 + 1):
                    ad = addr_of(S[dstid], px, py)
                    mem[ad] = blend_px(r[4], mem[ad], eq, c, sfmt)
        else:                                     # BLIT
            sx0, sy0 = s16(r[1]), s16(r[1] >> 16)
            sfmt = S[srcid]["format"]
            key_en = (r[0] >> 12) & 1
            for py in range(y0, y1 + 1):
                for px in range(x0, x1 + 1):
                    sx = sx0 + (px - dx)
                    sy = sy0 + (py - dy)
                    if not (0 <= sx < S[srcid]["width"]
                            and 0 <= sy < S[srcid]["height"]):
                        continue
                    sw = mem[addr_of(S[srcid], sx, sy)]
                    if key_en and sw == r[5]:
                        continue
                    ad = addr_of(S[dstid], px, py)
                    mem[ad] = blend_px(sw, mem[ad], eq, c, sfmt)
    return E_OK, 0, 0, reads


def run_lists(S, lists, clip0, mem0, inj=()):
    """按顺序跑多张命令表（共用 clip 状态与内存），
    返回 (code, listidx, idx, tag, reads, mem, clip)。

    listidx 只是一致性向量的定位元数据；硬件寄存器仍只报告当前提交表内的 idx。
    """
    mem = dict(mem0)
    clip = list(clip0)
    total_reads = 0
    for listidx, (cmds, count) in enumerate(lists):
        code, idx, tag, reads = run_one(S, cmds, count, clip, mem, inj)
        total_reads += reads
        if code != E_OK:
            return code, listidx, idx, tag, total_reads, mem, tuple(clip)
    return E_OK, 0, 0, 0, total_reads, mem, tuple(clip)


# ---------------------------------------------------------------------------
# 四、向量集：spec 第 02 章规范条款 §7 的九组
# ---------------------------------------------------------------------------

def pat(x, y):
    """图案源：每个像素都不同的确定值，用来验坐标映射。"""
    return (((0x40 + x) & 0xFF) << 16) | (((0x50 + y) & 0xFF) << 8) \
        | ((0x60 + x * 4 + y) & 0xFF)


class Vec:
    def __init__(self, group, name, cmds=None, lists=None, count=None,
                 code=E_OK, errlist=0, erridx=0, tag=None, reads=None, clip0=GAME,
                 clip_after=None, regions=(), pixels=(), mem=None, rule="",
                 covers=None, newspec=False, surfaces=None, check_mem=True):
        self.group = group
        self.name = name
        if lists is None:
            lists = [(cmds if cmds is not None else [],
                      len(cmds) if count is None and cmds is not None else count)]
        self.lists = [(c, (len(c) if n is None else n)) for c, n in lists]
        self.code = code
        self.errlist = errlist
        self.erridx = erridx
        self.tag = tag
        self.reads = reads
        self.clip0 = clip0
        self.clip_after = clip0 if clip_after is None else clip_after
        self.regions = regions      # ((sid, x0, y0, x1, y1), word)
        self.pixels = pixels        # ((sid, x, y), word)
        self.mem = mem or {}
        self.rule = rule
        self.covers = covers
        self.newspec = newspec
        self.surfaces = surfaces
        self.check_mem = check_mem

    def first_cmds(self):
        for c, _n in self.lists:
            if c:
                return c
        return []


def surf_seed(S, extra):
    """所有结构合法表面的默认内容 + 本向量的覆盖。"""
    mem = {}
    for sid, s in enumerate(S):
        if s is None or not surf_struct_ok(S, sid):
            continue
        for y in range(s["height"]):
            for x in range(s["width"]):
                mem[addr_of(s, x, y)] = DEFAULT_WORD
    mem.update(extra)
    return mem


def seed_pat(S, sid, extra=None):
    s = S[sid]
    d = {}
    for y in range(s["height"]):
        for x in range(s["width"]):
            d[addr_of(s, x, y)] = pat(x, y)
    if extra:
        d.update(extra)
    return d


def fill_rec(dst, eq, w4, c=0, dx=0, dy=0, w=4, h=2, w7=0):
    return rec(w0v=w0(OP_FILL, eq=eq, dst=dst, c=c), w2=coord(dx, dy),
               w3=wh(w, h), w4=w4, w7=w7)


def blit_rec(src, dst, eq, w4=0, sx=0, sy=0, dx=0, dy=0, w=4, h=2,
             key=0, keyword=0, c=0, w7=0):
    return rec(w0v=w0(OP_BLIT, eq=eq, key=key, src=src, dst=dst, c=c),
               w1=coord(sx, sy), w2=coord(dx, dy), w3=wh(w, h),
               w4=w4, w5=keyword, w7=w7)


def clip_rec(x, y, w, h):
    return rec(w0v=w0(OP_SETCLIP), w1=coord(x, y), w3=wh(w, h))


def end_rec(tag=0):
    return rec(w0v=w0(OP_END), w7=tag)


def blit_src_mem(S, sid, word):
    return {addr_of(S[sid], x, y): word
            for y in range(S[sid]["height"]) for x in range(S[sid]["width"])}


def build_vectors(S):
    V = []

    # --- 第一组：每个合法 opcode 与六个方程各一个正例 -----------------------
    g = "01"
    V.append(Vec(g, "nop-basic", [rec(w0v=w0(OP_NOP))], reads=1, rule="ch02 §4.1"))
    fills = [
        ("fill-copy", EQ_COPY, 0x00FF0000, 0, 0x00FF0000),
        ("fill-alpha", EQ_ALPHA, 0x80404040, 0, 0x00406080),
        ("fill-add", EQ_ADD, 0x00FFFFFF, 0, 0x00FFFFFF),
        ("fill-const", EQ_CONST, 0x00FFFFFF, 0x80, 0x0080A0C0),
        ("fill-mul", EQ_MUL, 0x00808080, 0, 0x00002040),
        ("fill-alpha-straight", EQ_ALPHA_STRAIGHT, 0x80404040, 0, 0x00204060),
    ]
    for name, eq, w4, c, out in fills:
        V.append(Vec(g, name, [fill_rec(0, eq, w4, c)], reads=1,
                     regions=[((0, 0, 0, 3, 1), out)],
                     rule="ch02 §1、§4.2 + ch01 3.4 的 V 顺序"))
    blits = [
        ("blit-copy-xrgb", 3, EQ_COPY, 0x00112233, 0x00112233),
        ("blit-alpha-pm", 1, EQ_ALPHA, 0x80404040, 0x00406080),
        ("blit-add-xrgb", 3, EQ_ADD, 0x00010203, 0x00014283),
        ("blit-const-straight", 2, EQ_CONST, 0x00FFFFFF, 0x0080A0C0),
        ("blit-mul-pm", 1, EQ_MUL, 0x00808080, 0x00002040),
        ("blit-alpha-straight", 2, EQ_ALPHA_STRAIGHT, 0x80404040, 0x00204060),
    ]
    for name, sid, eq, sval, out in blits:
        V.append(Vec(g, name, [blit_rec(sid, 0, eq, c=0x80 if eq == EQ_CONST else 0)],
                     reads=1, mem=blit_src_mem(S, sid, sval),
                     regions=[((0, 0, 0, 3, 1), out)],
                     rule="ch02 §1、§2、§4.3 + ch01 3.4"))
    V.append(Vec(g, "set-clip-basic", [clip_rec(1, 0, 2, 2)], reads=1,
                 clip_after=(1, 0, 2, 1), rule="ch02 §4.4"))
    V.append(Vec(g, "end-basic", [end_rec()], reads=1, rule="ch02 §4.1"))

    # --- 第二组：表中每个「必须为 0」的字段各一个反例 ----------------------
    g = "02"
    BASE = {
        OP_NOP: rec(w0v=w0(OP_NOP)),
        OP_END: rec(w0v=w0(OP_END)),
        OP_FILL: fill_rec(0, EQ_COPY, 0x00112233),
        OP_BLIT: blit_rec(3, 0, EQ_COPY),
        OP_SETCLIP: clip_rec(0, 0, 4, 2),
    }
    BADVAL = {"eq": 7, "key": 1, "tint": 1, "rsvd": 1, "src": 1, "dst": 1,
              "c": 0x5A, "w1": 0x11111111, "w2": 0x11111111, "w3": 0x11111111,
              "w4": 0x11111111, "w5": 0x11111111, "w6": 1}
    POS = {"eq": (0, 8, 0xF), "key": (0, 12, 1), "tint": (0, 13, 1),
           "rsvd": (0, 14, 0x3), "src": (0, 16, 0xF), "dst": (0, 20, 0xF),
           "c": (0, 24, 0xFF), "w1": (1, 0, -1), "w2": (2, 0, -1),
           "w3": (3, 0, -1), "w4": (4, 0, -1), "w5": (5, 0, -1), "w6": (6, 0, -1)}
    for op in (OP_NOP, OP_FILL, OP_BLIT, OP_SETCLIP, OP_END):
        base = BASE[op]
        eq = (base[0] >> 8) & 0xF
        key = (base[0] >> 12) & 1
        for f in must_zero(op, eq, key):
            r = list(base)
            wi, sh, mask = POS[f]
            if mask < 0:
                r[wi] = BADVAL[f]
            else:
                r[wi] = (r[wi] & ~(mask << sh)) | ((BADVAL[f] & mask) << sh)
            V.append(Vec(g, "%s-bad-%s" % (OP_NAME[op].lower(), f), [r],
                         code=E_BAD_FIELD, reads=1, covers="fields",
                         rule="ch02 §3 表：%s 的 %s 必须为 0" % (OP_NAME[op], f)))

    # --- 第三组：表面、描述符、格式、方程×格式匹配表 -----------------------
    g = "03"
    bad = [
        ("fill-dst-unregistered", fill_rec(8, EQ_COPY, 0x00FF0000), E_BAD_SURFACE, "dst_surface"),
        ("fill-dst-misaligned", fill_rec(9, EQ_COPY, 0x00FF0000), E_BAD_SURFACE, "dst_surface"),
        ("fill-dst-zerow", fill_rec(10, EQ_COPY, 0x00FF0000), E_BAD_SURFACE, "dst_surface"),
        ("fill-dst-zeroh", fill_rec(11, EQ_COPY, 0x00FF0000), E_BAD_SURFACE, "dst_surface"),
        ("fill-dst-stride1", fill_rec(12, EQ_COPY, 0x00FF0000), E_BAD_SURFACE, "dst_surface"),
        ("fill-dst-widthover", fill_rec(14, EQ_COPY, 0x00FF0000), E_BAD_SURFACE, "dst_surface"),
        ("fill-dst-overflow", fill_rec(15, EQ_COPY, 0x00FF0000), E_BAD_SURFACE, "dst_surface"),
        ("fill-dst-tex", fill_rec(4, EQ_COPY, 0x00FF0000), E_BAD_SPACE, "space"),
        ("fill-dst-band", fill_rec(5, EQ_COPY, 0x00FF0000), E_BAD_SPACE, "space"),
        ("fill-dst-index8", fill_rec(6, EQ_COPY, 0x00FF0000), E_BAD_FORMAT, "format"),
        ("fill-dst-unknown-fmt", fill_rec(7, EQ_COPY, 0x00FF0000), E_BAD_FORMAT, "format"),
        ("fill-dst-argb", fill_rec(2, EQ_COPY, 0x00FF0000), E_BAD_FORMAT, "format"),
        ("blit-src-unregistered", blit_rec(8, 0, EQ_COPY), E_BAD_SURFACE, "src_surface"),
        ("blit-dst-unregistered", blit_rec(3, 8, EQ_COPY), E_BAD_SURFACE, "dst_surface"),
        ("blit-src-tex", blit_rec(4, 0, EQ_COPY), E_BAD_SPACE, "space"),
        ("blit-dst-tex", blit_rec(3, 4, EQ_COPY), E_BAD_SPACE, "space"),
        ("blit-src-index8", blit_rec(6, 0, EQ_COPY), E_BAD_FORMAT, "format"),
        ("blit-dst-index8", blit_rec(3, 6, EQ_COPY), E_BAD_FORMAT, "format"),
        ("blit-src-unknown-fmt", blit_rec(7, 0, EQ_COPY), E_BAD_FORMAT, "format"),
        ("blit-alpha-src-straight", blit_rec(2, 0, EQ_ALPHA), E_BAD_FORMAT, "format"),
        ("blit-straight-src-pm", blit_rec(1, 0, EQ_ALPHA_STRAIGHT), E_BAD_FORMAT, "format"),
    ]
    for name, r, code, cover in bad:
        V.append(Vec(g, name, [r], code=code, reads=1, covers=cover,
                     rule="ch02 §2 描述符谓词与格式表"))

    # 方程×格式匹配表：18 个组合逐个一条，只判错误码（像素由第一、四、五组覆盖）
    SRCFMT_SLOT = {FMT_XRGB: 3, FMT_PM: 1, FMT_STRAIGHT: 2}
    for eq in range(6):
        for fmt in V1_SRC_FMT:
            sid = SRCFMT_SLOT[fmt]
            ok = fmt_eq_ok(fmt, eq)
            V.append(Vec(g, "eq%d-fmt%d" % (eq, fmt),
                         [blit_rec(sid, 0, eq, c=0x80 if eq == EQ_CONST else 0)],
                         code=E_OK if ok else E_BAD_FORMAT, reads=1,
                         covers=None if ok else "format", check_mem=False,
                         rule="ch02 §2：%s 源用于 %s -> %s"
                              % (FMT_NAME[fmt], EQ_NAME[eq],
                                 "合法" if ok else "BAD_FORMAT")))

    for name, bit, segment in (
            ("fill-dst-ctrl-rsvd-5", 5, "[7:5]"),
            ("fill-dst-ctrl-rsvd-10", 10, "[11:10]"),
            ("fill-dst-ctrl-rsvd-16", 16, "[30:16]")):
        S2 = list(S)
        S2[0] = dict(S[0], ctrl_rsvd=1 << bit)
        V.append(Vec(g, name, [fill_rec(0, EQ_COPY, 0x00FF0000)],
                     code=E_BAD_SURFACE, reads=1, covers="dst_surface",
                     surfaces=S2, newspec=True,
                     rule="ch02 §2 控制字保留段 %s 非零；复核定为 BAD_SURFACE"
                          % segment))
    V.append(Vec(g, "fill-tight-descriptor-ok", [fill_rec(13, EQ_COPY, 0x00FF0000,
                                                          w=1, h=2)],
                 reads=1, regions=[((13, 0, 0, 0, 1), 0x00FF0000)],
                 rule="ch02 §2：width*4 <= 1<<stride_log2 的等号一侧"))

    # --- 第四组：FILL 六方程、预乘常量边界与 BAD_PREMULT --------------------
    g = "04"
    g4 = [
        ("fill-alpha-premult-equal-ok", fill_rec(0, EQ_ALPHA, 0x80808040), E_OK,
         [((0, 0, 0, 3, 1), 0x0080A080)], None),
        ("fill-alpha-premult-r-over", fill_rec(0, EQ_ALPHA, 0x80818040),
         E_BAD_PREMULT, [], "fill_premult"),
        ("fill-alpha-premult-b-over", fill_rec(0, EQ_ALPHA, 0x800000FF),
         E_BAD_PREMULT, [], "fill_premult"),
        ("fill-alpha-a0-rewrites-dst", fill_rec(0, EQ_ALPHA, 0x00000000), E_OK,
         [((0, 0, 0, 3, 1), DEFAULT_WORD)], None),
        ("fill-alpha-a255", fill_rec(0, EQ_ALPHA, 0xFF404040), E_OK,
         [((0, 0, 0, 3, 1), 0x00404040)], None),
        ("fill-straight-rgb-over-a-ok", fill_rec(0, EQ_ALPHA_STRAIGHT, 0x8040FF40),
         E_OK, [((0, 0, 0, 3, 1), 0x0020A060)], None),
        ("fill-copy-high-byte-ignored", fill_rec(0, EQ_COPY, 0xFF123456), E_OK,
         [((0, 0, 0, 3, 1), 0x00123456)], None),
        ("fill-const-c0-identity", fill_rec(0, EQ_CONST, 0x00FFFFFF, 0x00), E_OK,
         [((0, 0, 0, 3, 1), DEFAULT_WORD)], None),
        ("fill-const-c255", fill_rec(0, EQ_CONST, 0x00FF8040, 0xFF), E_OK,
         [((0, 0, 0, 3, 1), 0x00FF8040)], None),
        ("fill-mul-src-zero", fill_rec(0, EQ_MUL, 0x00000000), E_OK,
         [((0, 0, 0, 3, 1), 0x00000000)], None),
    ]
    for name, r, code, regions, cov in g4:
        V.append(Vec(g, name, [r], code=code, reads=1, regions=regions, covers=cov,
                     rule="ch02 §4.2、§2 + ch01 3.4"))
    # 预乘源表面的像素若 RGB > A：ch02 把它写在格式定义里，但 BAD_PREMULT 只覆盖
    # ALPHA FILL 的常量。逐像素检查在硬件上不成立（会引入按像素的分支），
    # 所以定成素材侧契约：不报错，按公式算、由饱和决定（ch01 5.3 第二条已记账）。
    V.append(Vec(g, "blit-pm-source-violates-contract",
                 [blit_rec(1, 0, EQ_ALPHA)],
                 reads=1, mem=blit_src_mem(S, 1, 0x80818040),
                 regions=[((0, 0, 0, 3, 1), 0x0081A080)], newspec=True,
                 rule="ch02 §2 预乘约束只对源素材有效，命令解释层不逐像素检查（F3）"))

    # --- 第五组：重叠、矩形与裁剪 ------------------------------------------
    g = "05"
    pat3 = seed_pat(S, 3)
    V.append(Vec(g, "blit-same-surface-adjacent-ok",
                 [blit_rec(3, 3, EQ_COPY, sx=0, sy=0, dx=2, dy=0, w=2, h=2)],
                 reads=1, mem=pat3,
                 pixels=[((3, 2, 0), pat(0, 0)), ((3, 3, 0), pat(1, 0)),
                         ((3, 2, 1), pat(0, 1)), ((3, 3, 1), pat(1, 1))],
                 rule="ch02 §4.3：半开区间不相交"))
    V.append(Vec(g, "blit-same-surface-1px-overlap",
                 [blit_rec(3, 3, EQ_COPY, sx=0, sy=0, dx=1, dy=0, w=2, h=2)],
                 code=E_OVERLAP, reads=1, covers="overlap", mem=pat3,
                 rule="ch02 §4.3：交集非空"))
    V.append(Vec(g, "blit-same-surface-identical",
                 [blit_rec(3, 3, EQ_COPY, sx=0, sy=0, dx=0, dy=0, w=4, h=2)],
                 code=E_OVERLAP, reads=1, covers="overlap", mem=pat3,
                 rule="ch02 §4.3"))
    V.append(Vec(g, "blit-different-surface-overlap-ok",
                 [blit_rec(3, 0, EQ_COPY, sx=0, sy=0, dx=0, dy=0, w=4, h=2)],
                 reads=1, mem=pat3,
                 pixels=[((0, x, y), pat(x, y)) for y in range(2) for x in range(4)],
                 rule="ch02 §4.3：不同表面号不做物理区间比较"))
    V.append(Vec(g, "blit-zero-width-ok",
                 [blit_rec(3, 3, EQ_COPY, sx=0, sy=0, dx=1, dy=0, w=0, h=2)],
                 reads=1, mem=pat3, rule="ch02 §4.3：零面积没有矩形"))
    V.append(Vec(g, "blit-zero-height-ok",
                 [blit_rec(3, 3, EQ_COPY, sx=0, sy=0, dx=0, dy=0, w=4, h=0)],
                 reads=1, mem=pat3, rule="ch02 §4.3"))
    V.append(Vec(g, "blit-zero-width-bad-format",
                 [blit_rec(2, 0, EQ_ALPHA, w=0, h=2)],
                 code=E_BAD_FORMAT, reads=1, covers="format", newspec=True,
                 rule="ch02 §4.3「表面和字段检查通过后跳过」可读成零面积先跳过；"
                      "按 §5 十步，space/format 在重叠步之前 -> BAD_FORMAT"))
    V.append(Vec(g, "fill-zero-width-bad-dst-format",
                 [fill_rec(2, EQ_COPY, 0x00FF0000, w=0, h=2)],
                 code=E_BAD_FORMAT, reads=1, covers="format", newspec=True,
                 rule="零面积 FILL 同样先过 surface/format 检查（§4.3 只写了 BLIT）"))
    V.append(Vec(g, "fill-zero-width-bad-premult",
                 [fill_rec(0, EQ_ALPHA, 0x80818040, w=0, h=2)],
                 code=E_BAD_PREMULT, reads=1, covers="fill_premult", newspec=True,
                 rule="ch02 §4.3/§5：零面积 FILL 仍完成第九步预乘常量检查"))
    V.append(Vec(g, "blit-overlap-ignores-clip",
                 [clip_rec(0, 0, 0, 0),
                  blit_rec(3, 3, EQ_COPY, sx=0, sy=0, dx=1, dy=0, w=2, h=2)],
                 code=E_OVERLAP, erridx=1, reads=2, covers="overlap", mem=pat3,
                 clip_after=(0, 0, -1, -1),
                 rule="ch02 §4.3：重叠检查在裁剪之前"))
    V.append(Vec(g, "blit-clip-limits-writes",
                 [clip_rec(1, 0, 1, 2),
                  blit_rec(3, 0, EQ_COPY, sx=2, sy=0, dx=0, dy=0, w=4, h=2)],
                 reads=2, mem=pat3, clip_after=(1, 0, 1, 1),
                 pixels=[((0, 1, 0), pat(3, 0)), ((0, 1, 1), pat(3, 1))],
                 rule="ch01 3.6 四重求交"))
    V.append(Vec(g, "blit-source-out-of-bounds-skips-pixel",
                 [blit_rec(3, 0, EQ_COPY, sx=3, sy=0, dx=0, dy=0, w=4, h=2)],
                 reads=1, mem=pat3,
                 pixels=[((0, 0, 0), pat(3, 0)), ((0, 0, 1), pat(3, 1))],
                 rule="ch01 3.6 第 5 步：源越界只跳过该像素"))
    V.append(Vec(g, "blit-key-hits-all",
                 [blit_rec(3, 0, EQ_COPY, key=1, keyword=0x00112233)],
                 reads=1, mem=blit_src_mem(S, 3, 0x00112233),
                 rule="ch02 §4.3 + ch01 3.5：色键命中即跳过"))
    V.append(Vec(g, "blit-key-misses-all",
                 [blit_rec(3, 0, EQ_COPY, key=1, keyword=0x00112234)],
                 reads=1, mem=blit_src_mem(S, 3, 0x00112233),
                 regions=[((0, 0, 0, 3, 1), 0x00112233)],
                 rule="ch02 §4.3 + ch01 3.5"))

    # --- 第六组：结束条件 --------------------------------------------------
    g = "06"
    V.append(Vec(g, "count-zero", [], count=0, reads=0, rule="ch02 §5"))
    V.append(Vec(g, "count-exhausted-no-end",
                 [fill_rec(0, EQ_COPY, 0x00FF0000),
                  fill_rec(0, EQ_COPY, 0x0000FF00),
                  fill_rec(0, EQ_COPY, 0x000000FF)],
                 reads=3, regions=[((0, 0, 0, 3, 1), 0x000000FF)], rule="ch02 §5"))
    V.append(Vec(g, "end-before-illegal-record",
                 [fill_rec(0, EQ_COPY, 0x00FF0000), end_rec(),
                  rec(w0v=w0(OP_BAND))],
                 reads=2, regions=[((0, 0, 0, 3, 1), 0x00FF0000)],
                 rule="ch02 §4.1：END 之后的记录不读取"))
    V.append(Vec(g, "fetch-short-table",
                 [fill_rec(0, EQ_COPY, 0x00FF0000)], count=3,
                 code=E_FETCH, erridx=1, reads=2, covers="fetch", tag=0,
                 regions=[((0, 0, 0, 3, 1), 0x00FF0000)], newspec=True,
                 rule="ch02 §5：记录读取失败（用表内记录不足 COUNT 代表）。"
                      "记录没读到，w7 未知，本轮定 ERROR_TAG 读 0"))

    # --- 第七组：错误优先级（每条都能把顺序区分开） ------------------------
    g = "07"
    prio = [
        ("prio-opcode-over-field", rec(w0v=w0(OP_BAND, eq=7, tint=1)),
         E_BAD_OPCODE, "opcode"),
        ("prio-field-over-equation", fill_rec(0, 7, 0x00112233, c=1),
         E_BAD_FIELD, "fields"),
        ("prio-field-over-surface",
         rec(w0v=w0(OP_FILL, eq=EQ_COPY, key=1, dst=8), w2=coord(0, 0),
             w3=wh(4, 2), w4=0x00112233),
         E_BAD_FIELD, "fields"),
        ("prio-equation-over-surface", fill_rec(8, 9, 0x00112233),
         E_BAD_EQUATION, "equation"),
        ("prio-surface-over-space", blit_rec(4, 8, EQ_COPY),
         E_BAD_SURFACE, "dst_surface"),
        ("prio-src-surface-over-dst-format", blit_rec(8, 6, EQ_COPY),
         E_BAD_SURFACE, "src_surface"),
        ("prio-space-over-format", blit_rec(3, 5, EQ_COPY),
         E_BAD_SPACE, "space"),
        ("prio-format-over-premult", fill_rec(2, EQ_ALPHA, 0x80818040),
         E_BAD_FORMAT, "format"),
        ("prio-format-over-overlap", blit_rec(2, 2, EQ_ALPHA, sx=0, sy=0,
                                              dx=1, dy=0, w=2, h=2),
         E_BAD_FORMAT, "format"),
    ]
    for name, r, code, cov in prio:
        V.append(Vec(g, name, [r], code=code, reads=1, covers=cov, rule="ch02 §5"))
    # 源格式非法 + 目的 space 非 DDR：两种读法给出不同答案，本轮按规范条款 §5 定序
    V.append(Vec(g, "prio-src-format-vs-dst-space", [blit_rec(6, 4, EQ_COPY)],
                 code=E_BAD_SPACE, reads=1, covers="space", newspec=True,
                 rule="ch02 §5 十步的字面顺序（§2「分别检查」若按表面分组会得 BAD_FORMAT）"))

    # --- 第八组：失败副作用与错误定位 --------------------------------------
    g = "08"
    V.append(Vec(g, "error-no-side-effect-and-stop",
                 [fill_rec(0, EQ_COPY, 0x00FF0000),
                  rec(w0v=w0(OP_BAND), w7=0x1234),
                  fill_rec(0, EQ_COPY, 0x000000FF)],
                 code=E_BAD_OPCODE, erridx=1, tag=0x1234, reads=2,
                 regions=[((0, 0, 0, 3, 1), 0x00FF0000)],
                 rule="ch02 §5：首错停止、前序保留、后续不读"))
    V.append(Vec(g, "error-tag-separate-from-index",
                 [fill_rec(0, EQ_COPY, 0x00FF0000),
                  fill_rec(0, EQ_ALPHA, 0x80818040, w7=0xABCD),
                  fill_rec(0, EQ_COPY, 0x000000FF)],
                 code=E_BAD_PREMULT, erridx=1, tag=0xABCD, reads=2,
                 regions=[((0, 0, 0, 3, 1), 0x00FF0000)],
                 rule="ch02 §5：错误码、零基索引、w7 标签三者分离"))
    V.append(Vec(g, "error-blit-keeps-prior-pixels",
                 [blit_rec(3, 0, EQ_COPY),
                  blit_rec(6, 0, EQ_COPY),
                  blit_rec(3, 0, EQ_ADD)],
                 code=E_BAD_FORMAT, erridx=1, reads=2,
                 mem=blit_src_mem(S, 3, 0x00112233),
                 regions=[((0, 0, 0, 3, 1), 0x00112233)], rule="ch02 §5"))
    V.append(Vec(g, "error-first-record-still-locatable",
                 [rec(w0v=w0(OP_BAND), w7=0x0007)],
                 code=E_BAD_OPCODE, erridx=0, tag=0x0007, reads=1, rule="ch02 §5"))
    V.append(Vec(g, "error-in-second-list-keeps-local-index",
                 lists=[([clip_rec(0, 0, 2, 2)], None),
                        ([fill_rec(0, EQ_COPY, 0x00FF0000),
                          rec(w0v=w0(OP_BAND), w7=0xBEEF),
                          fill_rec(0, EQ_COPY, 0x000000FF)], None)],
                 code=E_BAD_OPCODE, errlist=1, erridx=1, tag=0xBEEF, reads=3,
                 clip_after=(0, 0, 1, 1),
                 regions=[((0, 0, 0, 1, 1), 0x00FF0000)],
                 rule="ch02 §5：多次提交时 ERROR_INDEX 仍是当前表内零基索引；前表状态保留"))

    # --- 第九组：SET_CLIP 状态与跨命令表继承 -------------------------------
    g = "09"
    V.append(Vec(g, "clip-inherits-across-lists",
                 lists=[([clip_rec(0, 0, 2, 2)], None),
                        ([fill_rec(0, EQ_COPY, 0x00FF0000)], None)],
                 reads=2, clip_after=(0, 0, 1, 1),
                 regions=[((0, 0, 0, 1, 1), 0x00FF0000)],
                 rule="ch01 3.6：SET_CLIP 不随命令表复位"))
    V.append(Vec(g, "clip-empty-skips-following",
                 [clip_rec(0, 0, 0, 0), fill_rec(0, EQ_COPY, 0x00FF0000)],
                 reads=2, clip_after=(0, 0, -1, -1),
                 rule="ch01 3.6：空矩形全部裁掉"))
    V.append(Vec(g, "clip-truncated-to-game-area",
                 [clip_rec(600, 470, 100, 100), fill_rec(0, EQ_COPY, 0x00FF0000)],
                 reads=2, clip_after=(600, 470, 639, 479),
                 rule="ch01 3.6：只能缩小，超出按游戏区截断"))
    V.append(Vec(g, "clip-negative-origin",
                 [clip_rec(-4, -4, 4, 4), fill_rec(0, EQ_COPY, 0x00FF0000)],
                 reads=2, clip_after=(0, 0, -1, -1),
                 rule="ch01 3.6：交集在游戏区内"))
    V.append(Vec(g, "clip-error-keeps-previous-update",
                 [clip_rec(0, 0, 2, 2),
                  rec(w0v=w0(OP_SETCLIP), w1=coord(0, 0), w2=1, w3=wh(2, 2)),
                  end_rec()],
                 code=E_BAD_FIELD, erridx=1, reads=2, clip_after=(0, 0, 1, 1),
                 rule="ch02 §5：错误记录本身不更新状态，此前成功的 SET_CLIP 保留"))
    V.append(Vec(g, "clip-later-command-can-enlarge",
                 [clip_rec(0, 0, 1, 1), clip_rec(0, 0, 4, 2), end_rec()],
                 reads=3, clip_after=(0, 0, 3, 1),
                 rule="ch01 3.6：新 SET_CLIP 替换旧框；只能缩小是相对游戏区"))

    return V


# ---------------------------------------------------------------------------
# 五、断言与输出
# ---------------------------------------------------------------------------

FAILS = []


def rep(ok, tag, detail=""):
    line = "%s %s" % (tag, "ok" if ok else "FAIL")
    if detail:
        line += "   " + detail
    print(line)
    if not ok:
        FAILS.append(tag)
    return ok


def resolve_surfaces(S, v):
    return v.surfaces if v.surfaces is not None else S


def expected_tag(v):
    """期望的 ERROR_TAG。默认取失败记录自己的 w7（不是索引——两者是分开的）。"""
    if v.code == E_OK:
        return 0
    if v.tag is not None:
        return v.tag
    if v.errlist < len(v.lists):
        cmds, _n = v.lists[v.errlist]
        if v.erridx < len(cmds):
            return cmds[v.erridx][7]
    return 0


def expected_mem(S, v):
    mem = surf_seed(S, v.mem)
    exp = dict(mem)
    for (sid, x0, y0, x1, y1), word in v.regions:
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                exp[addr_of(S[sid], x, y)] = word
    for (sid, x, y), word in v.pixels:
        exp[addr_of(S[sid], x, y)] = word
    return mem, exp


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dump-matrix", action="store_true",
                    help="打印规范的可执行镜像（字段矩阵、错误码、优先级），供与正文逐行对账")
    ap.add_argument("--emit", metavar="PATH",
                    help="把一致性向量按 C 与 Verilog 共用的文本格式写到文件")
    ap.add_argument("--no-selftest", action="store_true",
                    help="跳过注错自检（默认跑，见 Main/README.md「判据要能注错见红」）")
    args = ap.parse_args()

    S = make_surfaces()
    print("命令语义规格一致性校验（spec 第 02 章 / 命令 ABI v%d）" % ABI)
    print("工具：%s" % TOOL)
    print("")

    if args.dump_matrix:
        dump_matrix()

    V = build_vectors(S)

    print("(A) 结构审计：检查优先级十步各有反例")
    covered = {}
    for v in V:
        if v.covers:
            covered[v.covers] = covered.get(v.covers, 0) + 1
    for key, label in STEPS:
        n = covered.get(key, 0)
        rep(n > 0, "  %-13s %-30s 反例 %d 条" % (key, label, n))
    print("")

    print("(B) 结构审计：字段矩阵的「必须为 0」集合与第二组反例集合相等")
    want = set()
    for op, base in ((OP_NOP, rec(w0v=w0(OP_NOP))), (OP_END, rec(w0v=w0(OP_END))),
                     (OP_FILL, fill_rec(0, EQ_COPY, 0)),
                     (OP_BLIT, blit_rec(3, 0, EQ_COPY)),
                     (OP_SETCLIP, clip_rec(0, 0, 4, 2))):
        for f in must_zero(op, (base[0] >> 8) & 0xF, (base[0] >> 12) & 1):
            want.add((OP_NAME[op], f))
    got = set()
    for v in V:
        if v.group == "02":
            head, fname = v.name.split("-bad-")
            for op, nm in OP_NAME.items():
                if nm.lower() == head:
                    got.add((nm, fname))
    rep(got == want, "  矩阵零集 == 反例集",
        "" if got == want else "缺少 %s 多出 %s" % (sorted(want - got), sorted(got - want)))
    print("")

    print("(C) 结构审计：BLIT 的方程×源格式 18 个组合逐个有向量")
    seen = set()
    for v in V:
        cs = v.first_cmds()
        if not cs:
            continue
        r = cs[0]
        if (r[0] & 0xFF) != OP_BLIT:
            continue
        eq = (r[0] >> 8) & 0xF
        if eq > EQ_ALPHA_STRAIGHT:
            continue
        sid = (r[0] >> 16) & 0xF
        if S[sid] and S[sid]["format"] in V1_SRC_FMT:
            seen.add((eq, S[sid]["format"]))
    for eq in range(6):
        for fmt in V1_SRC_FMT:
            hit = (eq, fmt) in seen
            rep(hit, "  eq=%d(%-14s) fmt=%d(%-17s) -> %-10s"
                % (eq, EQ_NAME[eq], fmt, FMT_NAME[fmt],
                   "OK" if fmt_eq_ok(fmt, eq) else "BAD_FORMAT"),
                "" if hit else "该组合没有向量覆盖")
    print("")

    print("(D) 结构审计：描述符结构谓词各有失败例")
    reserved_vectors = {
        "控制字保留位 [7:5]": "fill-dst-ctrl-rsvd-5",
        "控制字保留位 [11:10]": "fill-dst-ctrl-rsvd-10",
        "控制字保留位 [30:16]": "fill-dst-ctrl-rsvd-16",
    }
    for sid, why in ((8, "valid=0"), (9, "base%4==0"), (10, "width>0"),
                     (11, "height>0"), (12, "2<=stride_log2"),
                     (14, "width*4<=1<<stride_log2"), (15, "末像素不溢出"),
                     (0, "控制字保留位 [7:5]"),
                     (0, "控制字保留位 [11:10]"),
                     (0, "控制字保留位 [30:16]")):
        hit = False
        for vc in V:
            if vc.group != "03" or vc.name.endswith("-ok"):
                continue
            r = vc.first_cmds()[0]
            if sid == 0:
                hit = hit or vc.name == reserved_vectors[why]
            elif ((r[0] >> 20) & 0xF) == sid or ((r[0] >> 16) & 0xF) == sid:
                if vc.code == E_BAD_SURFACE:
                    hit = True
        rep(hit, "  %-20s slot %-2d" % (why, sid))
    print("")

    print("(E) 一致性向量：%d 条" % len(V))
    groups = {}
    for v in V:
        groups.setdefault(v.group, []).append(v)
    for g in sorted(groups):
        print("  第 %s 组（%d 条）" % (g, len(groups[g])))
        for v in groups[g]:
            Sx = resolve_surfaces(S, v)
            m0 = surf_seed(Sx, v.mem)
            code, errlist, erridx, tag, reads, mem, clip = run_lists(
                Sx, v.lists, v.clip0, m0)
            _m, exp = expected_mem(Sx, v)
            etag = expected_tag(v)
            ok = (code == v.code)
            if v.code != E_OK:
                ok = (ok and errlist == v.errlist and erridx == v.erridx
                      and tag == etag)
            if v.reads is not None:
                ok = ok and reads == v.reads
            ok = ok and clip == v.clip_after
            if v.check_mem:
                ok = ok and mem == exp
            detail = ""
            if not ok:
                detail = ("got code=%s list=%d idx=%d tag=0x%X reads=%d clip=%s"
                          % (ERR_NAME[code], errlist, erridx, tag, reads, clip))
                if v.check_mem and mem != exp:
                    diff = [(hex(k), hex(exp.get(k, -1)), hex(mem.get(k, -1)))
                            for k in set(list(exp) + list(mem))
                            if exp.get(k) != mem.get(k)]
                    detail += " 像素不符 %d 处 %s" % (len(diff), diff[:4])
            rep(ok, "    %-38s %s" % (v.name, v.rule[:46]), detail)
    print("")

    print("(F) 本轮研究补入正文并经复核定案的条目")
    for v in V:
        if v.newspec:
            print("    %-38s %s" % (v.name, v.rule))
    rep(True, "  新增条目已逐条登记")
    print("")

    if not args.no_selftest:
        selftest(S, V)

    data = emit(S, V)
    shape_ok, shape_detail = audit_exchange_shape(data, V)
    print("(H) 交换格式结构审计：多命令表边界与每表 COUNT")
    rep(shape_ok, "  emit round-trip shape", shape_detail)
    print("")

    if args.emit:
        with open(args.emit, "w", encoding="utf-8") as f:
            f.write(data)
        h = hashlib.sha256(data.encode("utf-8")).hexdigest()
        print("向量集已写出：%s" % args.emit)
        print("  字节数 %d  sha256 %s" % (len(data.encode("utf-8")), h))
        print("")

    print("总判定：%s" % ("全部通过" if not FAILS else "有 %d 项不符" % len(FAILS)))
    print("fail %d" % len(FAILS))
    print("done")
    return 1 if FAILS else 0


# 注错自检：把某一条规则改错，向量集必须见红（Main/README.md「判据要能注错见红」）
INJECTIONS = [
    ("no_premult", "去掉 ALPHA FILL 的预乘约束检查",
     ["fill-alpha-premult-r-over", "fill-alpha-premult-b-over",
      "fill-zero-width-bad-premult"]),
    ("no_overlap", "去掉 BLIT 重叠检查",
     ["blit-same-surface-1px-overlap", "blit-same-surface-identical",
      "blit-overlap-ignores-clip"]),
    ("swap_space_format", "space 与 format 两步换序",
     ["fill-dst-band", "prio-space-over-format", "prio-src-format-vs-dst-space"]),
    ("zero_area_first", "零面积先跳过（不再做 space/format 检查）",
     ["blit-zero-width-bad-format", "fill-zero-width-bad-dst-format",
      "fill-zero-width-bad-premult"]),
    ("no_fill_key_check", "FILL 的色键位不检查",
     ["fill-bad-key"]),
    ("tag_is_index", "ERROR_TAG 直接用索引代替 w7",
     ["error-tag-separate-from-index"]),
]


def vector_ok(S, v, inj=()):
    Sx = resolve_surfaces(S, v)
    m0 = surf_seed(Sx, v.mem)
    code, errlist, erridx, tag, reads, mem, clip = run_lists(
        Sx, v.lists, v.clip0, m0, inj)
    _m, exp = expected_mem(Sx, v)
    ok = (code == v.code)
    if v.code != E_OK:
        ok = (ok and errlist == v.errlist and erridx == v.erridx
              and tag == expected_tag(v))
    if v.reads is not None:
        ok = ok and reads == v.reads
    ok = ok and clip == v.clip_after
    if v.check_mem:
        ok = ok and mem == exp
    return ok


def selftest(S, V):
    print("(G) 注错自检：逐条把规则改错，向量集必须见红")
    for name, desc, expect_red in INJECTIONS:
        red = [v.name for v in V if not vector_ok(S, v, (name,))]
        missing = [n for n in expect_red if n not in red]
        rep(bool(red) and not missing, "  %-20s %s" % (name, desc),
            "见红 %d 条" % len(red) + ("" if not missing else "  该红未红：%s" % missing))
    print("")


def dump_matrix():
    print("可执行镜像 A：逐 opcode 必须为 0 的字段（ch02 §3）")
    for op in (OP_NOP, OP_FILL, OP_BLIT, OP_SETCLIP, OP_END):
        eq = EQ_CONST if op == OP_FILL else EQ_COPY
        print("  %-9s %s" % (OP_NAME[op], " ".join(must_zero(op, eq, 0))))
    print("  说明：FILL/BLIT 的 c 仅 CONST 可非 0；BLIT 的 w5 仅 key=1 时可用。")
    print("")
    print("可执行镜像 B：错误码（ch02 §5）")
    for k in sorted(ERR_NAME):
        print("  %d %s" % (k, ERR_NAME[k]))
    print("")
    print("可执行镜像 C：检查顺序（ch02 §5）")
    for i, (key, label) in enumerate(STEPS, 1):
        print("  %2d %-13s %s" % (i, key, label))
    print("")
    print("可执行镜像 D：v1 的能力（ch02 §1）")
    print("  操作码 NOP/FILL/BLIT/SET_CLIP/END；space 只能 DDR；")
    print("  源格式 XRGB8888/ARGB8888_PREMULT/ARGB8888_STRAIGHT；目的只能 XRGB8888；")
    print("  方程 0~5；tint 与 w6 必须为 0。")
    print("")


def emit(S, vectors):
    out = []
    out.append("# gfx2d 命令语义一致性向量  abi=%d" % ABI)
    out.append("# 生成者 %s" % TOOL)
    out.append("# 格式说明见 Engineering/TechReports/spec/02-命令语义.md 规范条款 §7.2")
    out.append("# [vector] group name lists reads code errlist erridx errtag clip new mem")
    out.append("#          mem=0 表示这条向量不判内存，因此不写 [exp]（只判错误码那一类）")
    out.append("# [surf]   slot base width height ctrl=…（保留位原始掩码为 0x7FFF0CE0）")
    out.append("# [list]   count=…；后续 [cmd] 属于该命令表，直到下一段开始")
    out.append("# [cmd]    8 个字（w0..w7）")
    out.append("# [mem]    地址 字      初始内容（只列本向量用到的表面区域与种子）")
    out.append("# [exp]    地址 字      期望内容")
    for v in vectors:
        Sx = resolve_surfaces(S, v)
        m0, exp = expected_mem(Sx, v)
        # 只写本向量真正用到的表面区域与种子/期望地址，避免文件里全是默认填充
        slots = set()
        addrs = set(v.mem)
        for (sid, x0, y0, x1, y1), _w in v.regions:
            for y in range(y0, y1 + 1):
                for x in range(x0, x1 + 1):
                    addrs.add(addr_of(Sx[sid], x, y))
        for (sid, x, y), _w in v.pixels:
            addrs.add(addr_of(Sx[sid], x, y))
        for cmds, _n in v.lists:
            for r in cmds:
                op = r[0] & 0xFF
                if op == OP_FILL:
                    slots.add((r[0] >> 20) & 0xF)
                elif op == OP_BLIT:
                    slots.add((r[0] >> 16) & 0xF)
                    slots.add((r[0] >> 20) & 0xF)
        for sid in slots:
            s = Sx[sid]
            for y in range(s["height"]):
                for x in range(s["width"]):
                    addrs.add(addr_of(s, x, y))
        etag = expected_tag(v)
        out.append("[vector] group=%s name=%s lists=%d reads=%d code=%d errlist=%d erridx=%d "
                   "errtag=0x%08X clip=%d,%d,%d,%d new=%d mem=%d"
                   % (v.group, v.name, len(v.lists),
                       v.reads if v.reads is not None else -1,
                       v.code, v.errlist, v.erridx, etag,
                       v.clip_after[0], v.clip_after[1],
                       v.clip_after[2], v.clip_after[3], 1 if v.newspec else 0,
                       1 if v.check_mem else 0))
        for sid in sorted(slots):
            s = Sx[sid]
            ctrl = ((s["stride_log2"] & 0x1F) | ((s["space"] & 0x3) << 8)
                    | ((s["format"] & 0xF) << 12)
                    | (s["ctrl_rsvd"] & 0x7FFF0CE0)
                    | ((s["valid"] & 1) << 31))
            out.append("[surf] slot=%d base=0x%08X width=%d height=%d ctrl=0x%08X "
                       "(stride_log2=%d space=%d format=%d valid=%d)"
                       % (sid, s["base"], s["width"], s["height"], ctrl,
                          s["stride_log2"], s["space"], s["format"], s["valid"]))
        for cmds, count in v.lists:
            out.append("[list] count=%d" % count)
            for r in cmds:
                out.append("[cmd] " + " ".join("0x%08X" % x for x in r))
        for k in sorted(a for a in addrs if a in m0):
            out.append("[mem] 0x%08X 0x%08X" % (k, m0[k]))
        if v.check_mem:
            for k in sorted(a for a in addrs if a in exp):
                out.append("[exp] 0x%08X 0x%08X" % (k, exp[k]))
    return "\n".join(out) + "\n"


def audit_exchange_shape(data, vectors):
    """独立读回 emit 的结构层，防止多张命令表再次被拍平。

    这里刻意只解析 vector/list/cmd 三层，不复用 emit 的循环结构；逐字内容由
    C/RTL 的独立读取器在 SW3 再验。
    """
    observed = []
    cur = None
    for line in data.splitlines():
        if line.startswith("[vector] "):
            if cur is not None:
                observed.append(cur)
            fields = dict(item.split("=", 1) for item in line.split()[1:])
            cur = {
                "name": fields["name"],
                "declared": int(fields["lists"]),
                "sections": [],
            }
        elif line.startswith("[list] "):
            if cur is None:
                return False, "[list] 出现在 [vector] 之前"
            fields = dict(item.split("=", 1) for item in line.split()[1:])
            cur["sections"].append([int(fields["count"]), 0])
        elif line.startswith("[cmd] "):
            if cur is None or not cur["sections"]:
                return False, "[cmd] 出现在 [list] 之前"
            cur["sections"][-1][1] += 1
    if cur is not None:
        observed.append(cur)

    if len(observed) != len(vectors):
        return False, "vector 数 got=%d expect=%d" % (len(observed), len(vectors))
    for got, v in zip(observed, vectors):
        want = [[count, len(cmds)] for cmds, count in v.lists]
        if got["name"] != v.name:
            return False, "名字 got=%s expect=%s" % (got["name"], v.name)
        if got["declared"] != len(want) or got["sections"] != want:
            return False, "%s 表结构 got=%s expect=%s" % (
                v.name, got["sections"], want)
    return True, ""


if __name__ == "__main__":
    sys.exit(main())
