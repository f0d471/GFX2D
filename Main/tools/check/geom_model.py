#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""geom_model.py: 引擎几何层（engine_geom）的独立模型

这一份不读 RTL，按逐像素规格与命令语义规格的规则从命令算出：
  * 目的矩形、游戏区、裁剪框、目的表面、源表面**五者求交**（FILL 只有前四项）；
  * 逐行的读源、读目的、写目的描述符（16 字节对齐的覆盖区间，不跨 4KB，不超过 192 拍）；
  * 逐行的 I5 边带（拍数、首拍前导填充像素、本行像素数、源与目的两路的拍内像素相位、
    方程、`key`、色键字、常量源字、`c`、源格式、这一行读不读目的）；
  * 读写避让的三种规则。

用法：
  python3 geom_model.py --vectors <check_cmd_semantics --emit 的产物>
  python3 geom_model.py --cases   <geom_cases_dump 的产物>
  python3 geom_model.py --directed
  python3 geom_model.py --selftest
  python3 geom_model.py --dump-records <输入> <输出>  导出 $readmemh 能读的记录，给几何层的 Verilog 测试台

--vectors / --cases 除了描述符规矩与读写避让，还做两项对照，防止模型只和自己一致：
  * 每条 FILL/BLIT 按软件执行器的写法（四者求交的窗口里逐像素判源越界）逐行比，
    同时给出执行器口径的 pixel_total 与 src_oob；
  * 向量带参考实现的末态裁剪框（clip=）时，模型走完之后的裁剪框必须与它相同。
命令表的走法与 check_cmd_semantics.py 的 run_lists() 同口径：裁剪框跨表继承，出错处截断（run_vector）。

权威是 Main/doc/赛题二总体设计.md 3.4、3.5 节的规格与 Main/rtl/engine/engine_geom.v 的入库实现；
研究记录见 Main/sim/engine/geom/README.md。
"""

import argparse
import sys

# ---------------------------------------------------------------- 常量

SCREEN_W = 640
SCREEN_H = 480

OP_NOP = 0
OP_FILL = 1
OP_BLIT = 2
OP_SET_CLIP = 3
OP_END = 0xFF

EQ_COPY = 0
EQ_ALPHA = 1
EQ_ADD = 2
EQ_CONST = 3
EQ_MUL = 4
EQ_ALPHA_STRAIGHT = 5

FMT_XRGB8888 = 0
FMT_ARGB8888_PM = 1
FMT_ARGB8888_ST = 2

SURF_N = 16
AXI_MAX_BURST_LEN = 192          # D3、B2 的核参数
MAX_ROW_PIX = SCREEN_W           # 游戏区是五者之一，一行最多 640 像素
MAX_SEGS_PER_ROW = 2             # 一行取整后最多 2576 字节 < 4096，最多跨一次 4KB

EQ_NAME = {0: "COPY", 1: "ALPHA", 2: "ADD", 3: "CONST", 4: "MUL", 5: "ALPHA_STRAIGHT"}


def s16(v):
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def u16(v):
    return v & 0xFFFF


def coord(x, y):
    """w1 / w2 的布局：低半字 x、高半字 y，两个都是有符号。"""
    return ((y & 0xFFFF) << 16) | (x & 0xFFFF)


def w_h(w, h):
    """w3 的布局：低半字宽、高半字高，两个都是无符号。"""
    return ((h & 0xFFFF) << 16) | (w & 0xFFFF)


def fill_src_fmt(op, eq):
    """FILL 的常量源字按方程解释，规范条款 §4.2；BLIT 用源表面的格式。"""
    if op == OP_FILL:
        if eq == EQ_ALPHA:
            return FMT_ARGB8888_PM
        if eq == EQ_ALPHA_STRAIGHT:
            return FMT_ARGB8888_ST
        return FMT_XRGB8888
    return None


# ---------------------------------------------------------------- 表面与命令

class Surf:
    __slots__ = ("slot", "base", "width", "height", "stride_log2", "space",
                 "fmt", "valid", "ctrl_rsvd")

    def __init__(self, slot, base=0, width=0, height=0, stride_log2=0,
                 space=0, fmt=0, valid=0, ctrl_rsvd=0):
        self.slot = slot
        self.base = base
        self.width = width
        self.height = height
        self.stride_log2 = stride_log2
        self.space = space
        self.fmt = fmt
        self.valid = valid
        self.ctrl_rsvd = ctrl_rsvd

    def addr(self, x, y):
        return (self.base + (y << self.stride_log2) + (x << 2)) & 0xFFFFFFFF


class Cmd:
    __slots__ = ("w",)

    def __init__(self, words):
        self.w = list(words)

    @property
    def op(self):
        return self.w[0] & 0xFF

    @property
    def eq(self):
        return (self.w[0] >> 8) & 0xF

    @property
    def key_en(self):
        return (self.w[0] >> 12) & 1

    @property
    def src(self):
        return (self.w[0] >> 16) & 0xF

    @property
    def dst(self):
        return (self.w[0] >> 20) & 0xF

    @property
    def c(self):
        return (self.w[0] >> 24) & 0xFF

    @property
    def sx(self):
        return s16(self.w[1])

    @property
    def sy(self):
        return s16(self.w[1] >> 16)

    @property
    def dx(self):
        return s16(self.w[2])

    @property
    def dy(self):
        return s16(self.w[2] >> 16)

    @property
    def wd(self):
        return u16(self.w[3])

    @property
    def ht(self):
        return u16(self.w[3] >> 16)

    @property
    def w4(self):
        return self.w[4]

    @property
    def w5(self):
        return self.w[5]


# ---------------------------------------------------------------- 五者求交

def intersect(surf, cmd, clip):
    """返回闭区间矩形 (x0, y0, x1, y1)，空则 None。

    五项的次序与逐像素规格的定稿次序相同：目的矩形、游戏区、裁剪框、目的表面、源表面。
    中间量一律 32 位有符号；空宽高先判，避免 w-1 无符号下溢。
    源表面一项是「源越界只跳过该像素」在矩形级的等价写法：
        [dx - sx, dx - sx + src_w - 1] x [dy - sy, dy - sy + src_h - 1]
    FILL 没有源，只交前四项。
    """
    if cmd.wd == 0 or cmd.ht == 0:
        return None

    dx, dy = cmd.dx, cmd.dy
    w, h = cmd.wd, cmd.ht
    ds = surf[cmd.dst]

    x0 = max(0, dx, clip[0])
    y0 = max(0, dy, clip[1])
    x1 = min(SCREEN_W - 1, dx + w - 1, clip[2], ds.width - 1)
    y1 = min(SCREEN_H - 1, dy + h - 1, clip[3], ds.height - 1)

    if cmd.op == OP_BLIT:
        ss = surf[cmd.src]
        sx, sy = cmd.sx, cmd.sy
        x0 = max(x0, dx - sx)
        y0 = max(y0, dy - sy)
        x1 = min(x1, dx - sx + ss.width - 1)
        y1 = min(y1, dy - sy + ss.height - 1)

    if x1 < x0 or y1 < y0:
        return None
    return (x0, y0, x1, y1)


# ---------------------------------------------------------------- 描述符拆分

def split_descriptors(addr, nbytes, max_beats=AXI_MAX_BURST_LEN):
    """把 [addr, addr+nbytes) 的 16 字节对齐覆盖区间切成描述符。

    D3 规矩 1：每个描述符 16 字节对齐、不跨 4KB、不超过 max_beats 拍。
    返回 [(段基址, 拍数)]，拍数 = 字节数 / 16。覆盖区间只有 16 字节对齐的下界与非对齐的上界，
    所以起止都会向外取整，可能带出不属本行的像素——那些由 D16 的带内标记不写。
    """
    a0 = addr & ~0xF
    a1 = (addr + nbytes + 0xF) & ~0xF
    segs = []
    a = a0
    while a < a1:
        page_end = (a & ~0xFFF) + 0x1000
        n = (a1 - a) >> 4
        n = min(n, (page_end - a) >> 4, max_beats)
        assert n > 0, "拆出的段必须有正拍数"
        segs.append((a, n))
        a += n << 4
    return segs


def covering(addr, nbytes):
    """覆盖区间的起止字节（不拆段），给统计与判据用。"""
    return (addr & ~0xF, (addr + nbytes + 0xF) & ~0xF)


# ---------------------------------------------------------------- 一行的计划

class RowPlan:
    """一行发出去的东西：三个方向的描述符加一条 I5 边带。"""

    __slots__ = ("py", "dst_segs", "src_segs", "dst_rd_segs", "lead_dst",
                 "lead_src", "npx", "beats", "sb")

    def __init__(self, py, dst_segs, src_segs, dst_rd_segs, lead_dst, lead_src,
                 npx, sb):
        self.py = py
        self.dst_segs = dst_segs
        self.src_segs = src_segs
        self.dst_rd_segs = dst_rd_segs
        self.lead_dst = lead_dst
        self.lead_src = lead_src
        self.npx = npx
        self.beats = sum(n for _a, n in dst_segs)
        self.sb = sb

    def masks(self):
        """逐步算出每个写拍的 4 个像素哪些属于本行，给逐像素判据用。

        一拍 16 字节 = 4 个像素；拍内第 j 个像素的行内下标是
            idx = ((拍基址 - 行首) >> 2) + j
        属于本行当且仅当 0 <= idx < npx。
        """
        out = []
        first_addr = self.dst_segs[0][0] if self.dst_segs else 0
        # 行首地址 = 覆盖区间基址 + 前导填充像素 × 4（lead 就是 (行首 & 15) / 4）
        start = first_addr + self.lead_dst * 4
        for a, n in self.dst_segs:
            for k in range(n):
                m = []
                for j in range(4):
                    idx = ((a + 16 * k + 4 * j) - start) >> 2
                    m.append(0 <= idx < self.npx)
                out.append((a + 16 * k, m))
        return out


class CommandPlan:
    __slots__ = ("idx", "op", "rect", "rows", "read_src", "read_dst", "empty", "sb_cmd", "clip")

    def __init__(self, idx, op, rect, rows, read_src, read_dst, empty, sb_cmd, clip=None):
        self.idx = idx
        self.op = op
        self.rect = rect
        self.rows = rows
        self.read_src = read_src
        self.read_dst = read_dst
        self.empty = empty
        self.sb_cmd = sb_cmd
        self.clip = clip          # 这条命令执行时的裁剪框（I1 带进来的当前框）

    @property
    def n_desc(self):
        return sum(len(r.dst_segs) + len(r.src_segs) + len(r.dst_rd_segs)
                   for r in self.rows)


def d7_read_dst(op, eq):
    """D7：只有 COPY 不看目的，其余五种方程都要读目的。"""
    return eq != EQ_COPY


def d7_read_src(op, eq):
    """D7：FILL 永远是常量源，不读源；BLIT 无论哪个方程都要源。"""
    return op == OP_BLIT


def plan_command(surf, cmd, clip, idx=0):
    """一条已经通过十步检查的 FILL 或 BLIT 的完整计划。"""
    rect = intersect(surf, cmd, clip)
    read_src = d7_read_src(cmd.op, cmd.eq)
    read_dst = d7_read_dst(cmd.op, cmd.eq)
    sfmt = fill_src_fmt(cmd.op, cmd.eq)
    if sfmt is None:
        sfmt = surf[cmd.src].fmt
    sb_cmd = dict(op=cmd.op, eq=cmd.eq, key_en=cmd.key_en, key=cmd.w5,
                  w4=cmd.w4, c=cmd.c, sfmt=sfmt,
                  read_dst=read_dst, read_src=read_src)
    if rect is None:
        return CommandPlan(idx, cmd.op, None, [], read_src, read_dst, True, sb_cmd)

    x0, y0, x1, y1 = rect
    ds = surf[cmd.dst]
    ss = surf[cmd.src] if cmd.op == OP_BLIT else None
    npx = x1 - x0 + 1
    nbytes = npx * 4
    rows = []
    for py in range(y0, y1 + 1):
        dst_row = ds.addr(x0, py)
        dst_segs = split_descriptors(dst_row, nbytes)
        lead_dst = (dst_row & 0xF) >> 2
        if ss is not None:
            sx0 = cmd.sx + (x0 - cmd.dx)
            sy = cmd.sy + (py - cmd.dy)
            src_row = ss.addr(sx0, sy)
            lead_src = (src_row & 0xF) >> 2
            src_segs = split_descriptors(src_row, nbytes)
        else:
            src_row = None
            lead_src = 0
            src_segs = []
        dst_rd_segs = [] if not read_dst else split_descriptors(dst_row, nbytes)
        sb = dict(sb_cmd)
        sb.update(py=py, dst_row=dst_row, src_row=src_row, npx=npx,
                  lead_dst=lead_dst, lead_src=lead_src,
                  beats=sum(n for _a, n in dst_segs))
        rows.append(RowPlan(py, dst_segs, src_segs, dst_rd_segs, lead_dst,
                            lead_src, npx, sb))
    return CommandPlan(idx, cmd.op, rect, rows, read_src, read_dst, False, sb_cmd)


# ---------------------------------------------------------------- 命令表

def set_clip_rect(c):
    """SET_CLIP：w1 是左上角、w3 是尺寸，闭区间，设置时截到游戏区（exec_sw.c 的 apply_set_clip）。"""
    x0 = c.sx
    y0 = c.sy
    x1 = x0 + c.wd - 1
    y1 = y0 + c.ht - 1
    return [max(x0, 0), max(y0, 0), min(x1, SCREEN_W - 1), min(y1, SCREEN_H - 1)]


def run_list(surf, cmds, count, clip, limit=None):
    """按 ABI v1 走一张命令表，返回 (命令计划表, 末态 clip, 统计)。

    只做几何层的事：NOP 记一笔、SET_CLIP 更新裁剪框、FILL/BLIT 出计划、END 停。
    十步检查归控制层（B5），这里假定进来的记录已经合法；出错的那一条与之后的记录
    不会到几何层，由调用方用 limit 截断（见 run_vector）。
    clip 是这张表开始时的裁剪框：它跨命令表继承（规范条款），不能每张表重置。
    """
    plans = []
    clip = list(clip)
    st = dict(nop=0, setclip=0, zero_area=0, rows=0, desc=0, beats=0,
              splits=0, max_segs_row=0, oob_stop=False)
    n = min(count, len(cmds))
    if limit is not None:
        n = min(n, limit)
    for i in range(n):
        c = cmds[i]
        if c.op == OP_END:
            break
        if c.op == OP_NOP:
            st["nop"] += 1
            continue
        if c.op == OP_SET_CLIP:
            clip = set_clip_rect(c)
            st["setclip"] += 1
            continue
        if c.op not in (OP_FILL, OP_BLIT):
            st["oob_stop"] = True
            break
        p = plan_command(surf, c, clip, idx=i)
        p.clip = list(clip)
        plans.append(p)
        if p.empty:
            st["zero_area"] += 1
            continue
        for r in p.rows:
            st["rows"] += 1
            st["beats"] += r.beats
            nseg = len(r.dst_segs) + len(r.src_segs) + len(r.dst_rd_segs)
            st["max_segs_row"] = max(st["max_segs_row"], nseg)
            for segs in (r.dst_segs, r.src_segs, r.dst_rd_segs):
                if len(segs) > 1:
                    st["splits"] += 1
                st["desc"] += len(segs)
    return plans, clip, st


GAME_CLIP = [0, 0, SCREEN_W - 1, SCREEN_H - 1]


def run_vector(v):
    """按提交顺序走一个向量里的全部命令表，返回 ([(表号, 命令表, 命令计划表)], 末态 clip)。

    与 check_cmd_semantics.py 的 run_lists() 同一口径：
      * 裁剪框从游戏区开始，跨命令表继承；
      * 向量带错误（code != 0）时，errlist 之前的表全部执行，errlist 这张表只执行到 erridx 之前，
        出错的那一条没有副作用，之后的表不提交。几何层只会见到这些命令。
    """
    t = surf_table(v["slots"])
    clip = list(GAME_CLIP)
    out = []
    code = v.get("code") or 0
    errlist = v.get("errlist", 0)
    erridx = v.get("erridx", 0)
    for li, (count, cmds) in enumerate(v["lists"]):
        if code and li > errlist:
            break
        limit = erridx if (code and li == errlist) else None
        plans, clip, _st = run_list(t, cmds, count, clip, limit)
        out.append((li, cmds, plans))
    return t, out, clip


# ---------------------------------------------------------------- 与执行器对照

def exec_window(surf, cmd, clip):
    """照 exec_sw.c 的 dst_window()：目的矩形、游戏区、裁剪框、目的表面四者求交，没有源表面一项。"""
    if cmd.wd == 0 or cmd.ht == 0:
        return None
    ds = surf[cmd.dst]
    x0 = max(0, cmd.dx, clip[0])
    y0 = max(0, cmd.dy, clip[1])
    x1 = min(SCREEN_W - 1, cmd.dx + cmd.wd - 1, clip[2], ds.width - 1)
    y1 = min(SCREEN_H - 1, cmd.dy + cmd.ht - 1, clip[3], ds.height - 1)
    if x1 < x0 or y1 < y0:
        return None
    return (x0, y0, x1, y1)


def exec_check(surf, cmd, clip, plan, bad, st):
    """按软件执行器的写法逐像素走一遍，与五者求交的计划逐行比。

    执行器先做四者求交（exec_window），再在窗口里逐像素判源越界、越界的跳过
    （exec_sw.c 的 DEF_BLIT_LOOP）。这里不用 intersect() 的 max/min，而是照那个循环
    数出每一行真正会写的像素，要求它们连续、且与计划的这一行完全重合。
    同时累计执行器口径的 pixel_total 与 src_oob（B6 的 PIXEL 对账用 pixel_total − src_oob）。
    """
    where = "cmd%d" % plan.idx
    win = exec_window(surf, cmd, clip)
    rows = dict((r.py, r) for r in plan.rows)
    if win is None:
        if not plan.empty:
            bad.append("%s: 执行器的窗口是空的，计划却有 %d 行" % (where, len(plan.rows)))
        return
    x0, y0, x1, y1 = win
    st["pixel_total"] += (x1 - x0 + 1) * (y1 - y0 + 1)
    ss = surf[cmd.src] if cmd.op == OP_BLIT else None
    for py in range(y0, y1 + 1):
        xs = []
        for px in range(x0, x1 + 1):
            if ss is not None:
                sx = cmd.sx + (px - cmd.dx)
                sy = cmd.sy + (py - cmd.dy)
                if sx < 0 or sx >= ss.width or sy < 0 or sy >= ss.height:
                    st["src_oob"] += 1
                    continue
            xs.append(px)
        r = rows.pop(py, None)
        if not xs:
            if r is not None:
                bad.append("%s row%d: 执行器这一行一个像素都不写，计划却有这一行" % (where, py))
            continue
        if xs[-1] - xs[0] + 1 != len(xs):
            bad.append("%s row%d: 执行器写的像素不连续，矩形级的第五项不成立" % (where, py))
        if r is None or (xs[0], xs[-1]) != (plan.rect[0], plan.rect[2]) or len(xs) != r.npx:
            bad.append("%s row%d: 执行器写 x=%d..%d，计划 %s"
                       % (where, py, xs[0], xs[-1],
                          None if r is None else (plan.rect[0], plan.rect[2])))
    if rows:
        bad.append("%s: 计划有 %d 行落在执行器的窗口之外" % (where, len(rows)))


def check_vector_exec(v, t, lists, bad, st):
    """对一个向量里执行器会执行的每条 FILL/BLIT 做 exec_check。

    裁剪框在这里按执行器的写法**独立重放**（apply_set_clip：w1 是左上角、w3 是尺寸，
    跨命令表继承），走到哪一条也照 check_cmd_semantics 的 run_lists() 独立截断，
    不借用 run_list / run_vector 记下的框——否则裁剪框写错了两边一起错，整帧用例又没有参考终态可比。
    """
    clip = list(GAME_CLIP)
    code = v.get("code") or 0
    for li, (count, cmds) in enumerate(v["lists"]):
        if code and li > v["errlist"]:
            break
        plans = [pl for (l2, _c, pl) in lists if l2 == li]
        byidx = dict((p.idx, p) for p in (plans[0] if plans else []))
        n = min(count, len(cmds))
        if code and li == v["errlist"]:
            n = min(n, v["erridx"])
        seen = set()
        for i in range(n):
            c = cmds[i]
            if c.op == OP_END:
                break
            if c.op == OP_NOP:
                continue
            if c.op == OP_SET_CLIP:
                x0 = s16(c.w[1])
                y0 = s16(c.w[1] >> 16)
                x1 = x0 + u16(c.w[3]) - 1
                y1 = y0 + u16(c.w[3] >> 16) - 1
                clip = [max(x0, 0), max(y0, 0), min(x1, SCREEN_W - 1), min(y1, SCREEN_H - 1)]
                continue
            if c.op not in (OP_FILL, OP_BLIT):
                break
            p = byidx.get(i)
            if p is None:
                bad.append("%s 表%d cmd%d: 执行器会执行，模型没有规划" % (v["name"], li, i))
                continue
            seen.add(i)
            exec_check(t, c, clip, p, bad, st)
            st["exec_cmds"] += 1
        for i in sorted(set(byidx) - seen):
            bad.append("%s 表%d cmd%d: 执行器不会执行，模型却规划了" % (v["name"], li, i))


# ---------------------------------------------------------------- 判据

def check_rule1(segs, where, bad):
    for a, n in segs:
        if a & 0xF:
            bad.append("%s: 地址 0x%08X 不是 16 字节对齐" % (where, a))
        if ((a & 0xFFF) + (n << 4)) > 0x1000:
            bad.append("%s: 0x%08X 起 %d 拍跨 4KB" % (where, a, n))
        if n > AXI_MAX_BURST_LEN:
            bad.append("%s: %d 拍超过 %d" % (where, n, AXI_MAX_BURST_LEN))
        if n <= 0:
            bad.append("%s: 拍数 %d 非正" % (where, n))


def check_plan(plans, bad):
    """对一批命令计划跑全部几何判据，返回统计。"""
    st = dict(desc=0, beats=0, rows=0, src_desc=0, dst_rd_desc=0, wr_desc=0)
    for p in plans:
        for r in p.rows:
            st["rows"] += 1
            st["wr_desc"] += len(r.dst_segs)
            st["src_desc"] += len(r.src_segs)
            st["dst_rd_desc"] += len(r.dst_rd_segs)
            st["desc"] += len(r.dst_segs) + len(r.src_segs) + len(r.dst_rd_segs)
            st["beats"] += r.beats
            w = "cmd%d row%d" % (p.idx, r.py)
            check_rule1(r.dst_segs, w + " 写目的", bad)
            check_rule1(r.src_segs, w + " 读源", bad)
            check_rule1(r.dst_rd_segs, w + " 读目的", bad)

            # 覆盖区间必须盖住本行的每一个真像素，且首末拍掩码与 npx、lead 自洽
            if not r.dst_segs:
                bad.append("%s: 非空行没有写描述符" % w)
                continue
            cov0 = r.dst_segs[0][0]
            cov1 = r.dst_segs[-1][0] + (r.dst_segs[-1][1] << 4)
            row0 = r.sb["dst_row"]
            if cov0 != (row0 & ~0xF) or cov1 < ((row0 + r.npx * 4 + 0xF) & ~0xF):
                bad.append("%s: 覆盖区间没有盖住整行" % w)
            ms = r.masks()
            act = sum(1 for _a, m in ms for b in m if b)
            if act != r.npx:
                bad.append("%s: 掩码有效像素 %d != 本行 %d" % (w, act, r.npx))
            for _a, m in ms[1:-1]:
                if m != [True] * 4:
                    bad.append("%s: 中间拍有无效像素" % w)

            # 读写避让规则：读源、读目的的有无
            if (len(r.src_segs) > 0) != p.read_src:
                bad.append("%s: 读源描述符与 D7 不符" % w)
            if (len(r.dst_rd_segs) > 0) != p.read_dst:
                bad.append("%s: 读目的描述符与 D7 不符" % w)
    return st


# ---------------------------------------------------------------- 交换格式输入

class Exchange:
    """check_cmd_semantics.py --emit 与 geom_cases_dump 共用的记录格式。

    只解析几何层要的四种记录： [surf] [list] [cmd] [vector]。
    """

    def __init__(self):
        self.vectors = []

    def parse(self, text):
        cur = None
        for raw in text.splitlines():
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            if line.startswith("[vector]"):
                if cur is not None:
                    self.vectors.append(cur)
                cur = dict(name="?", group="?", code=None, slots={}, lists=[],
                           lists_expected=0, errlist=0, erridx=0, clip_after=None)
                for kv in line[len("[vector]"):].split():
                    if "=" not in kv:
                        continue
                    k, v = kv.split("=", 1)
                    if k == "name":
                        cur["name"] = v
                    elif k == "group":
                        cur["group"] = v
                    elif k == "lists":
                        cur["lists_expected"] = int(v, 0)
                    elif k == "code":
                        cur["code"] = int(v, 0)
                    elif k == "errlist":
                        cur["errlist"] = int(v, 0)
                    elif k == "erridx":
                        cur["erridx"] = int(v, 0)
                    elif k == "clip":
                        # 参考实现算出的末态裁剪框（出错时是出错那一刻的框）
                        cur["clip_after"] = [int(x, 0) for x in v.split(",")]
                    elif k == "mem":
                        cur["mem"] = int(v, 0)
            elif line.startswith("[surf]"):
                if cur is None:
                    continue
                kv = dict()
                for tok in line[len("[surf]"):].split():
                    tok = tok.strip("()")
                    if "=" in tok:
                        k, v = tok.split("=", 1)
                        kv[k] = v
                # ctrl= 后面可能带 "(stride_log2=… space=… format=… valid=…)"，只取十六进制
                ctrl_raw = kv.get("ctrl", "0x0")
                ctrl = int(ctrl_raw, 16) if ctrl_raw.lower().startswith("0x") else int(ctrl_raw, 0)
                sl = int(kv.get("stride_log2", ctrl & 0x1F), 0)
                sp = int(kv.get("space", (ctrl >> 8) & 3), 0)
                fm = int(kv.get("format", (ctrl >> 12) & 0xF), 0)
                va = int(kv.get("valid", (ctrl >> 31) & 1), 0)
                s = Surf(int(kv.get("slot", 0), 0),
                         base=int(kv.get("base", "0x0"), 0),
                         width=int(kv.get("width", 0), 0),
                         height=int(kv.get("height", 0), 0),
                         stride_log2=sl, space=sp, fmt=fm, valid=va,
                         ctrl_rsvd=ctrl & 0x7FFF0CE0)
                cur["slots"][s.slot] = s
            elif line.startswith("[list]"):
                if cur is None:
                    continue
                n = 0
                for tok in line[len("[list]"):].split():
                    if tok.startswith("count="):
                        n = int(tok.split("=", 1)[1], 0)
                cur["lists"].append((n, []))
            elif line.startswith("[cmd]"):
                if cur is None or not cur["lists"]:
                    continue
                words = [int(t, 16) for t in line[len("[cmd]"):].split() if t.startswith("0x")]
                cur["lists"][-1][1].append(Cmd(words))
        if cur is not None:
            self.vectors.append(cur)
        return self.vectors


def surf_table(slots):
    t = [Surf(i) for i in range(SURF_N)]
    for i, s in slots.items():
        if 0 <= i < SURF_N:
            t[i] = s
    return t


# ---------------------------------------------------------------- 定向用例

def directed_cases():
    """几何层仿真清单第 1 条的定向用例：真实输入没有覆盖到的几何边界。

    返回 [(名字, 说明, 表面表, 命令, 期望检查点)]。
    """
    cases = []

    def add(name, desc, surf, cmd):
        cases.append((name, desc, surf, cmd))

    def mk_surf(t, slot, base, w, h, slog, fmt=FMT_XRGB8888):
        t[slot] = Surf(slot, base=base, width=w, height=h, stride_log2=slog,
                       fmt=fmt, valid=1)

    # 1 表面基址不按 4KB 对齐（但按 16 字节对齐），行首地址的拍内相位非 0
    t = [Surf(i) for i in range(SURF_N)]
    mk_surf(t, 0, 0x00100FF0, 640, 480, 12)
    mk_surf(t, 1, 0x00200FF0, 64, 64, 8)
    add("base-not-4k", "目的基址 0x100FF0：4KB 边界落在行中间，行首相位随 y 变",
        t, Cmd([(OP_FILL) | (EQ_COPY << 8) | (0 << 20), 0, coord(16, 0),
               w_h(400, 100), 0x00112233, 0, 0, 0]))

    # 2 基址只按 4 字节对齐（合法下界），拍内相位不是 x 对 4 的余数
    t = [Surf(i) for i in range(SURF_N)]
    mk_surf(t, 0, 0x00100004, 640, 480, 12)
    add("base-mod4-only", "目的基址 0x100004：base%16=4，首拍前导填充随 x0 与 base 一起变",
        t, Cmd([(OP_FILL) | (EQ_COPY << 8) | (0 << 20), 0, coord(37, 16),
               w_h(13, 6), 0x00A0B0C0, 0, 0, 0]))

    # 3 一行跨 4KB（行首地址 0x100F00 + x0*4 = 0x101000 不成立，改用错开的基址）
    t = [Surf(i) for i in range(SURF_N)]
    mk_surf(t, 0, 0x00100E00, 640, 480, 12)
    add("row-cross-4k", "目的基址 0x100E00 + x0=64：行首 0x100F00，2560 字节跨过 0x101000，拆两段",
        t, Cmd([(OP_FILL) | (EQ_COPY << 8) | (0 << 20), 0, coord(64, 0),
               w_h(640, 8), 0x00010203, 0, 0, 0]))

    # 4 源、目的 x 对 4 的余数 4x4
    for sd in range(4):
        for dd in range(4):
            t = [Surf(i) for i in range(SURF_N)]
            mk_surf(t, 0, 0x00100000, 640, 480, 12)
            mk_surf(t, 1, 0x00200000, 64, 64, 8)
            add("rem4-src%d-dst%d" % (sd, dd),
                "源 x 对 4 余 %d、目的 x 对 4 余 %d 的 COPY BLIT" % (sd, dd),
                t, Cmd([(OP_BLIT) | (EQ_COPY << 8) | (1 << 16) | (0 << 20),
                       coord(sd, 0), coord(dd, 0), w_h(31, 9), 0, 0, 0, 0]))

    # 5 宽度对 4 取余 4 种
    for wr in range(4):
        t = [Surf(i) for i in range(SURF_N)]
        mk_surf(t, 0, 0x00100000, 640, 480, 12)
        add("width-mod4-%d" % wr, "宽度 29+%d，末拍有效像素 %d 个" % (wr, (29 + wr - 1) % 4 + 1),
            t, Cmd([(OP_FILL) | (EQ_COPY << 8) | (0 << 20), 0, coord(3, 1),
                   w_h(29 + wr, 2), 0x00FFFFFF, 0, 0, 0]))

    # 6 源矩形越出源表面（五者求交的第五项）
    t = [Surf(i) for i in range(SURF_N)]
    mk_surf(t, 0, 0x00100000, 640, 480, 12)
    mk_surf(t, 1, 0x00200000, 64, 64, 8)
    add("src-oob", "源坐标 -10,-10、宽高 100：第五项把 100x100 收成 64x64",
        t, Cmd([(OP_BLIT) | (EQ_COPY << 8) | (1 << 16) | (0 << 20),
               coord(-10, -10), coord(300, 100), w_h(100, 100), 0, 0, 0, 0]))

    # 7 四种方程的读目的有无（读写避让规则）
    for eq in (EQ_COPY, EQ_ALPHA, EQ_ADD, EQ_CONST, EQ_MUL, EQ_ALPHA_STRAIGHT):
        t = [Surf(i) for i in range(SURF_N)]
        mk_surf(t, 0, 0x00100000, 640, 480, 12)
        mk_surf(t, 1, 0x00200000, 64, 64, 8, FMT_ARGB8888_PM)
        add("eq%d-readdst" % eq, "方程 %s：%s读目的" % (EQ_NAME[eq], "不" if eq == 0 else ""),
            t, Cmd([(OP_BLIT) | (eq << 8) | (1 << 16) | (0 << 20),
                   coord(1, 2), coord(5, 3), w_h(16, 4), 0x40FF0000, 0, 0, 0]))

    # 8~10 是 2026-10-01 复核补的：原来只有 FILL 的一行跨 4KB（row-cross-4k），
    # 读源与读目的两个方向的拆段（RTL 里是另外两套算式）一条都没走到，
    # 把源的第二段地址、源的页余量、读目的第二段地址改错，测试台全绿。
    # 8 ALPHA BLIT 的一行跨 4KB：源、目的都拆两段，而且拆点不同
    t = [Surf(i) for i in range(SURF_N)]
    mk_surf(t, 0, 0x00100E00, 640, 480, 12)
    mk_surf(t, 1, 0x00200C00, 640, 16, 12, FMT_ARGB8888_PM)
    add("blit-cross-4k-both", "ALPHA BLIT：目的行首 0x100F00 拆 16+128 拍，源行首 0x200C00 拆 64+80 拍",
        t, Cmd([(OP_BLIT) | (EQ_ALPHA << 8) | (1 << 16) | (0 << 20),
               coord(0, 0), coord(64, 0), w_h(576, 4), 0, 0, 0, 0]))

    # 9 只有源跨 4KB，目的不跨
    t = [Surf(i) for i in range(SURF_N)]
    mk_surf(t, 0, 0x00100000, 640, 480, 12)
    mk_surf(t, 1, 0x00200D00, 640, 8, 12)
    add("blit-src-cross-only", "COPY BLIT：源行首 0x200D10 拆 47+3 拍，目的 0x100000 一段 50 拍",
        t, Cmd([(OP_BLIT) | (EQ_COPY << 8) | (1 << 16) | (0 << 20),
               coord(4, 0), coord(0, 0), w_h(200, 3), 0, 0, 0, 0]))

    # 10 源基址只按 8 字节对齐：源相位由基址带出，源与目的的拍数不同
    t = [Surf(i) for i in range(SURF_N)]
    mk_surf(t, 0, 0x00100000, 640, 480, 12)
    mk_surf(t, 1, 0x00200008, 64, 64, 8, FMT_ARGB8888_PM)
    add("blit-src-base-mod8", "ALPHA BLIT：源行首 0x20000C 相位 3、5 拍，目的相位 0、4 拍",
        t, Cmd([(OP_BLIT) | (EQ_ALPHA << 8) | (1 << 16) | (0 << 20),
               coord(1, 0), coord(0, 5), w_h(14, 3), 0, 0, 0, 0]))

    # 11 零面积（裁剪后没有一行）
    t = [Surf(i) for i in range(SURF_N)]
    mk_surf(t, 0, 0x00100000, 640, 480, 12)
    add("zero-area-src-oob", "源表面完全在矩形之外，第五项把它交成空",
        t, Cmd([(OP_BLIT) | (EQ_COPY << 8) | (1 << 16) | (0 << 20),
               coord(0x2000, 0x2000), coord(0, 0), w_h(16, 4), 0, 0, 0, 0]))

    return cases


# ---------------------------------------------------------------- 自检

def selftest():
    ok = True

    def rep(cond, what):
        nonlocal ok
        ok = ok and cond
        print("  [%s] %s" % ("OK" if cond else "FAIL", what))

    print("拆分与对齐：")
    a0, a1 = covering(0x1000, 2560)
    rep(a0 == 0x1000 and a1 == 0x1A00, "16 字节对齐的行不额外扩边")
    a0, a1 = covering(0x1002, 2559)
    rep(a0 == 0x1000 and a1 == 0x1A10, "非对齐行向外取整到 0x1000..0x1A10，实际 %s" % ((a0, a1),))
    segs = split_descriptors(0x0F00, 2560)
    rep(len(segs) == 2 and segs[0] == (0x0F00, 16) and segs[1] == (0x1000, 144),
        "跨 4KB 拆成两段：%s" % (segs,))
    segs = split_descriptors(0x1000, 4096)
    rep(segs == [(0x1000, 192), (0x1C00, 64)], "超 192 拍也拆：%s" % (segs,))
    for a, n in split_descriptors(0x0FF8, 2575):
        rep((a & 0xF) == 0 and ((a & 0xFFF) + (n << 4)) <= 0x1000 and n <= 192,
            "段 0x%08X/%d 满足规矩 1" % (a, n))

    print("五者求交：")
    t = [Surf(i) for i in range(SURF_N)]
    t[0] = Surf(0, base=0x1000, width=640, height=480, stride_log2=12, valid=1)
    t[1] = Surf(1, base=0x2000, width=64, height=64, stride_log2=8, valid=1)
    clip = [0, 0, SCREEN_W - 1, SCREEN_H - 1]
    # 源越界：第五项把 100x100 收成 64x64
    c = Cmd([(OP_BLIT) | (0 << 8) | (1 << 16) | (0 << 20),
             coord(-10, -10), coord(300, 100), w_h(100, 100), 0, 0, 0, 0])
    rep(intersect(t, c, clip) == (310, 110, 373, 173),
        "源 -10/-10 宽高 100 -> 64x64，得到 %s" % (intersect(t, c, clip),))
    # 裁剪框比游戏区更严
    c2 = Cmd([(OP_FILL) | (0 << 8) | (0 << 20), 0, coord(0, 0), w_h(640, 480), 0, 0, 0, 0])
    rep(intersect(t, c2, [100, 100, 299, 249]) == (100, 100, 299, 249), "裁剪框生效")
    rep(intersect(t, c2, [0, 0, -1, -1]) is None, "空裁剪框整条跳过")
    # 目的表面比屏幕小
    t[2] = Surf(2, base=0x3000, width=100, height=50, stride_log2=9, valid=1)
    c3 = Cmd([(OP_FILL) | (0 << 8) | (2 << 20), 0, coord(0, 0), w_h(640, 480), 0, 0, 0, 0])
    rep(intersect(t, c3, clip) == (0, 0, 99, 49), "目的表面 100x50 截断")
    # 空宽高先判
    c3b = Cmd([(OP_FILL) | (0 << 8) | (2 << 20), 0, coord(0, 0), w_h(0, 480), 0, 0, 0, 0])
    rep(intersect(t, c3b, clip) is None, "宽 0 整条跳过")

    print("D7 与边带：")
    c4 = Cmd([(OP_FILL) | (EQ_COPY << 8) | (0 << 20), 0, coord(1, 3),
              w_h(29, 2), 0xFFFFFFFF, 0, 0, 0])
    p = plan_command(t, c4, clip)
    r = p.rows[0]
    rep(not p.read_src and not p.read_dst, "FILL COPY 两笔读都省掉")
    row0 = 0x1000 + (3 << 12) + (1 << 2)
    rep(r.lead_dst == ((row0 & 0xF) >> 2),
        "目的首拍相位由地址算出：%d == %d" % (r.lead_dst, (row0 & 0xF) >> 2))
    rep(sum(1 for _a, m in r.masks() for b in m if b) == 29, "掩码有效像素 = 本行 29")
    ms = r.masks()
    rep(len(ms) > 2 and all(b for _a, m in ms[1:-1] for b in m),
        "中间拍全是有效像素（%d 拍，中间 %d 拍）" % (len(ms), max(len(ms) - 2, 0)))
    rep(ms[0][1] == [False, True, True, True] and ms[-1][1] == [True, True, False, False],
        "首拍 lead=1 空 1 个、末拍剩 2 个：%s / %s" % (ms[0][1], ms[-1][1]))

    print("命令表与裁剪框：")
    # SET_CLIP 的左上角在 w1（exec_sw.c 的 apply_set_clip），w2 不用；这里故意让 w2 非 0
    clip_cmd = Cmd([OP_SET_CLIP, coord(100, 100), coord(7, 9), w_h(200, 150), 0, 0, 0, 0])
    full = Cmd([(OP_FILL) | (EQ_COPY << 8) | (0 << 20), 0, coord(0, 0), w_h(640, 480), 0, 0, 0, 0])
    plans, clip_end, _st = run_list(t, [clip_cmd, full], 2, GAME_CLIP)
    rep(plans[0].rect == (100, 100, 299, 249), "SET_CLIP 取 w1 为左上角：%s" % (plans[0].rect,))
    # 裁剪框跨命令表继承，不随表复位
    v = dict(slots={0: t[0]}, code=0, errlist=0, erridx=0,
             lists=[(1, [clip_cmd]), (1, [full])])
    _t, lists, clip_end = run_vector(v)
    rep(lists[1][2][0].rect == (100, 100, 299, 249) and clip_end == [100, 100, 299, 249],
        "第二张表继承第一张表的裁剪框：%s" % (lists[1][2][0].rect,))
    # 向量出错：出错那一条与之后的记录、之后的表都不到几何层
    bad_cmd = Cmd([4, 0, 0, 0, 0, 0, 0, 0xBEEF])
    v = dict(slots={0: t[0]}, code=1, errlist=0, erridx=1,
             lists=[(3, [full, bad_cmd, full]), (1, [full])])
    _t, lists, _clip = run_vector(v)
    rep(len(lists) == 1 and [p.idx for p in lists[0][2]] == [0],
        "出错在第 0 张表第 1 条：只规划第 0 条，第二张表不提交")

    c5 = Cmd([(OP_FILL) | (EQ_ALPHA << 8) | (0 << 20), 0, coord(0, 0),
              w_h(16, 2), 0x80FFFFFF, 0, 0, 0])
    p = plan_command(t, c5, clip)
    rep(not p.read_src and p.read_dst, "FILL ALPHA 只读目的")
    rep(p.sb_cmd["sfmt"] == FMT_ARGB8888_PM, "FILL ALPHA 的源格式按方程取 PREMULT")

    c6 = Cmd([(OP_BLIT) | (EQ_ALPHA << 8) | (1 << 16) | (0 << 20),
              coord(1, 2), coord(5, 3), w_h(16, 4), 0x40FF0000, 0, 0, 0])
    p = plan_command(t, c6, clip)
    rep(p.read_src and p.read_dst, "BLIT ALPHA 两笔读都要")
    rep(p.sb_cmd["sfmt"] == t[1].fmt, "BLIT 的源格式取自源表面")

    print("")
    print("SELFTEST %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


# ---------------------------------------------------------------- 主流程

def _fmt_row(r):
    return ("    y=%-4d npx=%-4d 写%s 读源%s 读目的%s lead=%d/%d"
            % (r.py, r.npx,
               "[" + ",".join("0x%08X/%d" % s for s in r.dst_segs) + "]",
               "[" + ",".join("0x%08X/%d" % s for s in r.src_segs) + "]",
               "[" + ",".join("0x%08X/%d" % s for s in r.dst_rd_segs) + "]",
               r.lead_dst, r.lead_src))


def run_vectors(path, verbose):
    text = open(path, "r", encoding="utf-8", errors="replace").read()
    ex = Exchange()
    ex.parse(text)
    bad = []
    tot = dict(vectors=0, lists=0, cmds=0, geom_cmds=0, rows=0, desc=0, beats=0,
               splits=0, src_desc=0, dst_rd_desc=0, wr_desc=0, zero=0,
               not_run=0, clip_checked=0)
    ex_st = dict(exec_cmds=0, pixel_total=0, src_oob=0)
    ex_bad = []
    maxrow_beats = 0
    maxrow_desc = 0
    leads = set()
    for v in ex.vectors:
        tot["vectors"] += 1
        t, lists, clip = run_vector(v)
        tot["lists"] += len(v["lists"])
        tot["cmds"] += sum(len(cmds) for _n, cmds in v["lists"])
        tot["not_run"] += len(v["lists"]) - len(lists)
        # 裁剪框的语义（SET_CLIP 取 w1、跨表继承、出错时停在出错那一刻）与参考实现的末态比
        if v["clip_after"] is not None:
            tot["clip_checked"] += 1
            if list(clip) != v["clip_after"]:
                bad.append("%s: 末态裁剪框 %s != 参考实现 %s" % (v["name"], clip, v["clip_after"]))
        check_vector_exec(v, t, lists, ex_bad, ex_st)
        for _li, _cmds, plans in lists:
            st2 = check_plan([p for p in plans if not p.empty], bad)
            tot["geom_cmds"] += len([p for p in plans if not p.empty])
            tot["zero"] += len([p for p in plans if p.empty])
            for k in ("rows", "desc", "beats", "src_desc", "dst_rd_desc", "wr_desc"):
                tot[k] += st2[k]
            for p in plans:
                for r in p.rows:
                    tot["splits"] += sum(1 for s in (r.dst_segs, r.src_segs, r.dst_rd_segs) if len(s) > 1)
                    maxrow_beats = max(maxrow_beats, r.beats)
                    maxrow_desc = max(maxrow_desc, len(r.dst_segs) + len(r.src_segs) + len(r.dst_rd_segs))
                    leads.add((r.lead_dst, r.lead_src))
    bad.extend(ex_bad)
    print("输入：%s" % path)
    print("  向量 %d、命令表 %d（出错后不提交 %d 张）、命令 %d（几何层见到的 FILL/BLIT：非空 %d、空 %d）"
          % (tot["vectors"], tot["lists"], tot["not_run"], tot["cmds"], tot["geom_cmds"], tot["zero"]))
    print("  行 %d、描述符 %d（写 %d / 读源 %d / 读目的 %d）、写拍 %d"
          % (tot["rows"], tot["desc"], tot["wr_desc"], tot["src_desc"], tot["dst_rd_desc"], tot["beats"]))
    print("  跨 4KB 拆成两段 %d 次、单行最多 %d 个描述符（三个方向合计）、单行最多 %d 拍"
          % (tot["splits"], maxrow_desc, maxrow_beats))
    print("  见过的 (lead_dst, lead_src) 组合 %d 种：%s"
          % (len(leads), sorted(leads)[:16]))
    print("  与执行器逐像素对照：%d 条 FILL/BLIT，违例 %d；执行器口径 pixel_total %d，其中源越界 %d"
          % (ex_st["exec_cmds"], len(ex_bad), ex_st["pixel_total"], ex_st["src_oob"]))
    if tot["clip_checked"]:
        print("  末态裁剪框与参考实现对照：%d 个向量" % tot["clip_checked"])
    if verbose:
        for v in ex.vectors[:5]:
            _t, lists, _clip = run_vector(v)
            for _li, _cmds, plans in lists:
                for p in plans:
                    print("  [%s] cmd%d op=%d eq=%s rect=%s"
                          % (v["name"], p.idx, p.op, EQ_NAME.get(p.sb_cmd["eq"], "?"), p.rect))
                    for r in p.rows[:3]:
                        print(_fmt_row(r))
                    if len(p.rows) > 3:
                        print("    ... 共 %d 行" % len(p.rows))
    if bad:
        print("  违例 %d 条：" % len(bad))
        for b in bad[:20]:
            print("    " + b)
    else:
        print("  规矩 1、D7、逐像素对照与裁剪框：违例 0")
    print("RESULT: %s" % ("PASS" if not bad else "FAIL"))
    return 0 if not bad else 1


def run_directed(verbose):
    bad = []
    total = 0
    print("定向用例：")
    for name, desc, t, cmd in directed_cases():
        clip = [0, 0, SCREEN_W - 1, SCREEN_H - 1]
        p = plan_command(t, cmd, clip)
        # 定向用例里 SET_CLIP 由 run_list 管，这里全屏
        st = check_plan([p] if not p.empty else [], bad)
        total += 1
        segs = len(p.rows[0].dst_segs) if p.rows else 0
        print("  %-22s %-58s 行 %-4d 写段 %d 读源 %d 读目的 %d"
              % (name, desc, len(p.rows),
                 sum(len(r.dst_segs) for r in p.rows),
                 sum(len(r.src_segs) for r in p.rows),
                 sum(len(r.dst_rd_segs) for r in p.rows)))
        if verbose:
            for r in p.rows[:2]:
                print(_fmt_row(r))
            for r in p.rows[:1]:
                ms = r.masks()
                print("      首拍掩码 %s  末拍掩码 %s" % (ms[0][1], ms[-1][1]))
        # 定向用例自带的检查点
        if name == "row-cross-4k":
            if not p.rows or len(p.rows[0].dst_segs) != 2:
                bad.append("row-cross-4k 没有拆成两段")
        if name.startswith("rem4-"):
            sd = int(name.split("src")[1].split("-")[0])
            dd = int(name.split("dst")[1])
            if p.rows:
                if p.rows[0].lead_dst != dd:
                    bad.append("%s: 目的相位 %d != %d" % (name, p.rows[0].lead_dst, dd))
                if p.rows[0].lead_src != sd:
                    bad.append("%s: 源相位 %d != %d" % (name, p.rows[0].lead_src, sd))
        if name.startswith("eq") and name.endswith("readdst"):
            eq = int(name[2:].split("-")[0])
            if p.read_dst != (eq != EQ_COPY):
                bad.append("%s: 读目的开关错" % name)
        if name == "blit-cross-4k-both":
            r0 = p.rows[0] if p.rows else None
            if (r0 is None or [n for _a, n in r0.dst_segs] != [16, 128]
                    or [n for _a, n in r0.dst_rd_segs] != [16, 128]
                    or [n for _a, n in r0.src_segs] != [64, 80]):
                bad.append("blit-cross-4k-both 的拆段不是 写/读目的 16+128、读源 64+80")
        if name == "blit-src-cross-only":
            r0 = p.rows[0] if p.rows else None
            if (r0 is None or [n for _a, n in r0.src_segs] != [47, 3]
                    or [n for _a, n in r0.dst_segs] != [50] or r0.dst_rd_segs):
                bad.append("blit-src-cross-only 的拆段不是 读源 47+3、写 50、不读目的")
        if name == "blit-src-base-mod8":
            r0 = p.rows[0] if p.rows else None
            if (r0 is None or r0.lead_src != 3 or r0.lead_dst != 0
                    or [n for _a, n in r0.src_segs] != [5] or [n for _a, n in r0.dst_segs] != [4]):
                bad.append("blit-src-base-mod8 不是 源相位 3、5 拍，目的相位 0、4 拍")
        if name == "zero-area-src-oob" and not p.empty:
            bad.append("zero-area-src-oob 应当是空")
        if name == "src-oob" and p.rect != (310, 110, 373, 173):
            bad.append("src-oob 矩形 %s" % (p.rect,))
    print("  用例 %d 条" % total)
    if bad:
        print("  违例 %d 条：" % len(bad))
        for b in bad:
            print("    " + b)
    print("RESULT: %s" % ("PASS" if not bad else "FAIL"))
    return 0 if not bad else 1


# 记录格式的字段顺序。测试台用 $readmemh 读成一个 32 位字的数组，
# 按类型字决定后面读几个字；这三张表是**唯一真源**，RTL 测试台照它写。
REC_CMD = ["cidx", "op", "eq", "flags", "nrows", "y0", "x0", "y1", "x1",
           "key_en", "sfmt", "c",
           "src_base", "src_w", "src_h", "src_slog2",
           "dst_base", "dst_w", "dst_h", "dst_slog2",
           "src_x", "src_y", "dst_x", "dst_y", "w", "h",
           "clip_x0", "clip_y0", "clip_x1", "clip_y1", "key", "w4"]
REC_WR = ["cidx", "row", "addr", "beats", "last"]
REC_RD = ["cidx", "row", "dir", "buf", "addr", "beats"]
REC_SB = ["cidx", "row", "py", "npx", "lead_dst", "lead_src", "beats"]


def _h(v):
    """按 32 位十六进制写；负数取补码。"""
    return "%08X" % (v & 0xFFFFFFFF)


def emit_command_records(out, p, surf, cmd, clip):
    """一条命令的 I1 输入（类型 0）与它的期望输出（类型 3/2/1），按 RTL 的发出顺序。

    顺序与 engine_geom.v 的发射顺序一致，测试台据此逐条比：
      0  命令头（I1 的全部字段 + 期望的 rect、行数、flags）
      3  每行的边带        ← 先发，B4 在数据回来之前就知道这一行的相位与掩码
      2  dir=0 读源、dir=1 读目的（每段一笔）
      1  写目的（每段一笔）
    `buf` 是读描述符的目标行缓冲号：位 1 = 0 源 / 1 目的，位 0 = 行缓冲的两半（按行号最低位翻转）。
    `last` 只打在本命令最后一行的最后一段上（I2，2026-10-01 订正）。
    """
    flags = (1 if p.read_src else 0) | (2 if p.read_dst else 0) | (4 if p.empty else 0)
    r = p.rect if p.rect else (0, 0, -1, -1)
    blit = (cmd.op == OP_BLIT)
    ss = surf[cmd.src] if blit else Surf(0)
    ds = surf[cmd.dst]
    src_x, src_y = (cmd.sx, cmd.sy) if blit else (0, 0)
    vals = [p.idx, p.op, p.sb_cmd["eq"], flags, len(p.rows),
            r[1] & 0xFFFF, r[0] & 0xFFFF, r[2] & 0xFFFF, r[3] & 0xFFFF,
            1 if cmd.key_en else 0, p.sb_cmd["sfmt"], cmd.c,
            ss.base if blit else 0, ss.width if blit else 0,
            ss.height if blit else 0, ss.stride_log2 if blit else 0,
            ds.base, ds.width, ds.height, ds.stride_log2,
            src_x & 0xFFFF, src_y & 0xFFFF, cmd.dx & 0xFFFF, cmd.dy & 0xFFFF,
            cmd.wd, cmd.ht,
            clip[0] & 0xFFFF, clip[1] & 0xFFFF, clip[2] & 0xFFFF, clip[3] & 0xFFFF,
            cmd.w5, cmd.w4]
    assert len(vals) == len(REC_CMD), "类型 0 的字段数与 REC_CMD 不符"
    out.append(" ".join(["0"] + [_h(v) for v in vals]))
    for ri, row in enumerate(p.rows):
        last_row = (ri == len(p.rows) - 1)
        out.append("3 " + " ".join(_h(v) for v in
                                   [p.idx, ri, row.sb["py"], row.npx,
                                    row.lead_dst, row.lead_src, row.beats]))
        for a, n in row.src_segs:
            out.append("2 " + " ".join(_h(v) for v in
                                       [p.idx, ri, 0, (0 << 1) | (ri & 1), a, n]))
        for a, n in row.dst_rd_segs:
            out.append("2 " + " ".join(_h(v) for v in
                                       [p.idx, ri, 1, (1 << 1) | (ri & 1), a, n]))
        for si, (a, n) in enumerate(row.dst_segs):
            out.append("1 " + " ".join(_h(v) for v in
                                       [p.idx, ri, a, n,
                                        1 if (last_row and si == len(row.dst_segs) - 1) else 0]))


def dump_records(path_in, path_out):
    """把几何计划导成定长记录，给 B3 的 Verilog 测试台 $readmemh 读。

    每行一条记录，第一列是类型字，其余全是 32 位十六进制、没有 x/z；
    字段表见 REC_CMD / REC_WR / REC_RD / REC_SB（RTL 测试台照它写）。
    `//` 开头是注释（每个向量、每张命令表开头各一行），`$readmemh` 直接读。
    只导出几何层真正见到的命令：裁剪框跨表继承，向量出错时截在出错那一条之前（run_vector）；
    `cidx` 是表内下标。
    """
    text = open(path_in, "r", encoding="utf-8", errors="replace").read()
    ex = Exchange()
    ex.parse(text)
    out = []
    for v in ex.vectors:
        t, lists, _clip = run_vector(v)
        out.append("// vector %s order=%d" % (v["name"], len(out)))
        for li, cmds, plans in lists:
            out.append("// list %d" % li)
            for p in plans:
                emit_command_records(out, p, t, cmds[p.idx], p.clip)
    out.append("F")
    with open(path_out, "w", encoding="utf-8") as f:
        f.write("\n".join(out) + "\n")
    print("写出 %s：%d 行记录（%d 条命令）"
          % (path_out, len(out), sum(1 for s in out if s.startswith("0 "))))
    return 0


def dump_directed(path_out):
    """把定向用例导成同一种记录（B3 单元仿真第 1 条判据）。

    每条用例一条命令，裁剪框是全屏（定向用例不带 SET_CLIP），表面表来自用例自己。
    """
    out = []
    for name, _desc, t, cmd in directed_cases():
        clip = [0, 0, SCREEN_W - 1, SCREEN_H - 1]
        p = plan_command(t, cmd, clip, idx=0)
        p.clip = list(clip)
        out.append("// vector %s order=%d" % (name, len(out)))
        emit_command_records(out, p, t, cmd, clip)
    out.append("F")
    with open(path_out, "w", encoding="utf-8") as f:
        f.write("\n".join(out) + "\n")
    print("写出 %s：%d 行记录（%d 条命令）"
          % (path_out, len(out), sum(1 for s in out if s.startswith("0 "))))
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--vectors")
    ap.add_argument("--cases")
    ap.add_argument("--directed", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--dump-records", nargs=2, metavar=("IN", "OUT"))
    ap.add_argument("--dump-directed", metavar="OUT")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()

    rc = 0
    if a.selftest:
        rc |= selftest()
    if a.vectors:
        rc |= run_vectors(a.vectors, a.verbose)
    if a.cases:
        rc |= run_vectors(a.cases, a.verbose)
    if a.directed:
        rc |= run_directed(a.verbose)
    if a.dump_records:
        rc |= dump_records(a.dump_records[0], a.dump_records[1])
    if a.dump_directed:
        rc |= dump_directed(a.dump_directed)
    if not (a.selftest or a.vectors or a.cases or a.directed or a.dump_records
            or a.dump_directed):
        ap.print_help()
    return rc


if __name__ == "__main__":
    sys.exit(main())
