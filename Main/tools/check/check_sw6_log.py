#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""check_sw6_log.py —— SW6 判据日志的外部核对

判据与日志格式的权威是 Engineering/Todo/sw/SW6-基础Demo.md 第 3 节与 2.7 节。
本脚本只做**日志侧**的判据，不碰应用内部：spec 第 01 章 4.1、4.7 两节那条教训是
「判据要能从外面核对，不能由被测程序自己判自己」。

七种模式，各打印若干行 `chk <名字> ok|FAIL`，末尾 `fail <n>`，退出码 0 表示全过：

    --compare A B           两份日志逐行比（去掉 cyc 三列），用于 D3 与 D4
    --pair LOG              同一段的两遍（p 1／p 2）逐行比（再去掉 p 列），用于 D5
    --hud LOG --font FONT   hud 列与上一帧统计对账；hcrc 与按字库文本独立重画的
                            HUD 矩形 CRC 对账，用于 D6
    --scene LOG             三个 scene 参考值两两不同；每个 B 帧的 scrc 等于本帧 s
                            那张图的参考值；k=1 段内 s 逐帧轮换，用于 D7
    --counts LOG            A 帧 off >= 4；eq A 的 copy/alpha/add 非 0；
                            eq P 的 copy/alpha/straight 非 0，用于 D8
    --quick-prefix FULL Q   快速档的逐帧行按段、按遍必须是全量档的前缀，用于 D3
    --board BOARD EXPECT    板上串口日志从 pak 行起与期望日志逐行比（去掉 cyc 三列），
                            用于第 3 节「板上对账」

只用标准库（zlib.crc32 与 lib/core/crc32 同参，SW5 4.10 节对过账）。
"""

import argparse
import sys
import zlib

# f 行的字段位置：行里不出现字符本身，按空白切分就能解析（spec 2.7）
F_LABELS = ["f", None, None, "p", None, "s", None, "keys", None, "n", None,
            "k", None, "cmd", None, "pix", None, "ksk", None, "hud", None,
            None, "off", None, "cyc", None, None, None, "crc", None,
            "scrc", None, "hcrc", None]
F_TOKENS = len(F_LABELS)
CYC_IDX = (25, 26, 27)
P_IDX = 4

HUD_X = 400
HUD_Y = 0
HUD_W = 240
HUD_H = 160
CELL_W = 8
CELL_H = 16
FONT_COLS = 16
FONT_FIRST = 0x20

FAILED = []


def ok(name):
    print("chk %s ok" % name)


def bad(name, why=""):
    print("chk %s FAIL %s" % (name, why))
    FAILED.append(name)


def norm(path):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return [ln.rstrip("\r\n") for ln in f]


def parse_f(line):
    """解析一条 f 行；不合法返回 None。"""
    t = line.split()
    if len(t) != F_TOKENS:
        return None
    for i, lab in enumerate(F_LABELS):
        if lab is not None and t[i] != lab:
            return None
    return t


def strip_cyc(tokens):
    out = list(tokens)
    for i in CYC_IDX:
        out[i] = "x"
    return out


def groups(lines):
    """把 f 行按 (stage, pass) 分组，值是该组的行列表（保持顺序）。"""
    g = {}
    for ln in lines:
        t = parse_f(ln)
        if t is None:
            continue
        g.setdefault((t[2], t[4]), []).append(t)
    return g


# ---------------------------------------------------------------- D3 / D4

def compare_lines(a, b, tag):
    if len(a) != len(b):
        bad("%s 行数" % tag, "%d 对 %d" % (len(a), len(b)))
        return
    for i, (la, lb) in enumerate(zip(a, b), 1):
        ta, tb = parse_f(la), parse_f(lb)
        if ta is not None and tb is not None:
            la = " ".join(strip_cyc(ta))
            lb = " ".join(strip_cyc(tb))
        if la != lb:
            # 行很长、差别常在行尾：报第一个不同的字段，而不是截断的整行
            wa, wb = la.split(), lb.split()
            j = next((k for k in range(min(len(wa), len(wb))) if wa[k] != wb[k]),
                     min(len(wa), len(wb)))
            bad("%s 第 %d 行" % (tag, i), "第 %d 个字段起不同：%r != %r（行首 %r）"
                % (j, " ".join(wa[j:j + 2]), " ".join(wb[j:j + 2]), " ".join(wa[:3])))
            return
    ok("%s 全部 %d 行（去掉 cyc 三列）逐行相同" % (tag, len(a)))


def mode_compare(pa, pb):
    compare_lines(norm(pa), norm(pb), "compare")


# ---------------------------------------------------------------- 板上对账

BANNER = "gfx2dGame SW6"


def from_pak(lines):
    for i, ln in enumerate(lines):
        if ln.startswith("pak "):
            return lines[i:]
    return None


def board_body(lines):
    """板上串口日志里属于本程序的那一段。

    board_run.py 的日志里有三样别的东西：它自己的 `#` 标记行；加载之前 Flash 里旧程序
    在 `reset halt` 之前打的几行；本程序的横幅常常抢在「程序已加载」标记之前出现
    （程序一恢复就打印，标记写得晚）。所以去掉 `#` 行与空行，取**最后一个**横幅之后、
    从 `pak` 行起的部分，与期望日志从 `pak` 行起比（spec 第 3 节「板上对账」）。"""
    body = [ln for ln in lines if ln.strip() != "" and not ln.startswith("#")]
    last = None
    for i, ln in enumerate(body):
        if ln == BANNER:
            last = i
    if last is None:
        return None
    return from_pak(body[last + 1:])


def mode_board(pb, pe):
    b = board_body(norm(pb))
    e = from_pak(norm(pe))
    if b is None:
        bad("board 串口日志", "找不到横幅 %r 之后的 pak 行" % BANNER)
        return
    if e is None:
        bad("board 期望日志", "没有 pak 行")
        return
    compare_lines(e, b, "board 从 pak 行起")


# ---------------------------------------------------------------- D5

def mode_pair(path):
    lines = norm(path)
    g = groups(lines)
    if not g:
        bad("pair 没有逐帧行")
        return
    for stage in ("A", "B"):
        p1 = g.get((stage, "1"), [])
        p2 = g.get((stage, "2"), [])
        if not p1 or not p2:
            bad("pair %s 段两遍不全" % stage, "%d 对 %d" % (len(p1), len(p2)))
            continue
        if len(p1) != len(p2):
            bad("pair %s 段帧数" % stage, "%d 对 %d" % (len(p1), len(p2)))
            continue
        diff = 0
        for i, (x, y) in enumerate(zip(p1, p2)):
            u = list(strip_cyc(x)); v = list(strip_cyc(y))
            u[P_IDX] = "x"; v[P_IDX] = "x"
            if u != v:
                if diff == 0:
                    bad("pair %s 段第 %d 帧" % (stage, i), "两遍不同" % ())
                diff += 1
        if diff == 0:
            ok("pair %s 段 %d 帧两遍逐行相同" % (stage, len(p1)))


# ---------------------------------------------------------------- D6

def load_font(path):
    glyphs = []
    rows = []
    with open(path, "r", encoding="utf-8") as f:
        for line in f.read().splitlines():
            s = line.strip()
            if s == "" or s == "#" or (s.startswith("#") and len(s) > 1 and s[1].isspace()):
                continue
            if len(s) != CELL_W or any(c not in ".#" for c in s):
                raise ValueError("字库里有不合法的像素行：%r" % line)
            rows.append(s)
    if len(rows) != 95 * CELL_H:
        raise ValueError("字库有 %d 行数据，应为 %d" % (len(rows), 95 * CELL_H))
    for g in range(95):
        glyphs.append(rows[g * CELL_H:(g + 1) * CELL_H])
    return glyphs


def hud_text(row):
    """按 spec 2.3 的行表与本行字段拼 HUD 的九行，判据模式下实时量都是 `--`。

    返回 [(行号, 字符串)]，行号 0~8。"""
    n, k = row[10], row[12]
    hcmd, hpix = row[20], row[21]
    return [
        (0, "SW FPS --"),
        (1, "HW FPS --"),
        (2, "N " + n),
        (3, "K " + k),
        (4, "CMD " + ("--" if hcmd == "-" else hcmd)),
        (5, "PIX " + ("--" if hpix == "-" else hpix)),
        (6, "LOG --"),
        (7, "BLD --"),
        (8, "EXE --"),
    ]


def hud_crc(row, glyphs):
    """按字库文本与本行字段独立重画 HUD 矩形，再按 hcrc 的口径算 CRC。

    底色纯黑、字形前景 0x00FFFFFF（spec 2.3），所以每个像素只有两种取值；
    每像素 4 字节按存储序 B,G,R,X 排（xrgb 的末字节是 0）。"""
    bg = bytes((0x00, 0x00, 0x00, 0x00))
    fg = bytes((0xFF, 0xFF, 0xFF, 0x00))
    pix = [[bg] * HUD_W for _ in range(HUD_H)]

    for line_no, text in hud_text(row):
        y0 = line_no * CELL_H
        for col, ch in enumerate(text):
            gi = ord(ch) - FONT_FIRST
            if gi < 0 or gi >= len(glyphs):
                continue
            x0 = col * CELL_W
            for r in range(CELL_H):
                bits = glyphs[gi][r]
                for c in range(CELL_W):
                    if bits[c] == "#" and x0 + c < HUD_W and y0 + r < HUD_H:
                        pix[y0 + r][x0 + c] = fg

    crc = 0
    for y in range(HUD_H):
        crc = zlib.crc32(b"".join(pix[y]), crc)
    return crc & 0xFFFFFFFF


def mode_hud(path, font):
    lines = norm(path)
    try:
        glyphs = load_font(font)
    except (OSError, ValueError) as exc:
        bad("hud 字库", str(exc))
        return

    n_check = n_bad = 0
    for stage in ("A", "B"):
        for p in ("1", "2"):
            seq = [parse_f(ln) for ln in lines]
            seq = [t for t in seq if t is not None and t[2] == stage and t[4] == p]
            if not seq:
                bad("hud %s p%s 没有帧" % (stage, p))
                continue
            prev = None
            for i, t in enumerate(seq):
                n_check += 1
                # (a) hud 列 == 上一帧的 cmd/pix，每遍首帧是 - -
                want = ("-", "-") if prev is None else (prev[14], prev[16])
                got = (t[20], t[21])
                if i == 0:
                    if got != ("-", "-"):
                        n_bad += 1
                        bad("hud %s p%s 首帧" % (stage, p), "%r 应为 - -" % (got,))
                elif got != want:
                    n_bad += 1
                    bad("hud %s p%s 第 %d 帧" % (stage, p, i),
                        "画的是 %r，上一帧统计是 %r" % (got, want))
                # (b) hcrc == 独立重画的 CRC
                want_crc = hud_crc(t, glyphs)
                if int(t[33], 16) != want_crc:
                    n_bad += 1
                    bad("hcrc %s p%s 第 %d 帧" % (stage, p, i),
                        "日志 %s，重画 %08x" % (t[33], want_crc))
                prev = t
    if n_bad == 0:
        ok("hud／hcrc %d 帧全部对账" % n_check)


# ---------------------------------------------------------------- D7

def mode_scene(path):
    lines = norm(path)
    refs = {}
    for ln in lines:
        t = ln.split()
        if len(t) == 4 and t[0] == "scene" and t[2] == "scrc":
            refs[int(t[1])] = int(t[3], 16)
    if len(refs) != 3:
        bad("scene 参考值", "只解析到 %d 个" % len(refs))
        return
    if len(set(refs.values())) != 3:
        bad("scene 参考值两两不同", "三个值是 %r" % (refs,))
    else:
        ok("scene 三个参考值两两不同")

    n_check = n_bad = 0
    k1_by_pass = {}
    for ln in lines:
        t = parse_f(ln)
        if t is None or t[2] != "B":
            continue
        n_check += 1
        s = int(t[6])
        if int(t[31], 16) != refs.get(s, -1):
            n_bad += 1
            bad("scene B 第 %s 帧" % t[1], "scrc %s 不等于 scene %d 的参考值" % (t[31], s))
        if t[12] == "1":
            k1_by_pass.setdefault(t[4], []).append(s)
    if n_bad == 0:
        ok("scene %d 个 B 帧的 scrc 都等于本帧 s 的参考值" % n_check)

    if not k1_by_pass:
        bad("scene k=1 停留段", "一帧都没有")
    for p in sorted(k1_by_pass):
        k1 = k1_by_pass[p]
        if len(k1) < 3:
            bad("scene k=1 停留段 p%s" % p, "只有 %d 帧" % len(k1))
            continue
        bad_at = [i for i in range(1, len(k1)) if k1[i] != (k1[i - 1] + 1) % 3]
        if bad_at:
            bad("scene k=1 逐帧轮换 p%s" % p, "第 %d 帧起不符：%r" % (bad_at[0], k1[:8]))
        elif len(set(k1)) != 3:
            bad("scene k=1 停留段 p%s" % p, "三张图没有都轮到：%r" % sorted(set(k1)))
        else:
            ok("scene k=1 段内 s 逐帧轮换且三张都轮到（p%s，%d 帧）" % (p, len(k1)))


# ---------------------------------------------------------------- D8

def mode_counts(path):
    lines = norm(path)
    n_a = n_off = 0
    for ln in lines:
        t = parse_f(ln)
        if t is None or t[2] != "A":
            continue
        n_a += 1
        if int(t[23]) < 4:
            n_off += 1
    if n_a == 0:
        bad("counts 没有 A 帧")
    elif n_off == 0:
        ok("counts %d 个 A 帧的 off 都 >= 4" % n_a)
    else:
        bad("counts off", "%d 个 A 帧的 off < 4" % n_off)

    # eq 行一行都没有时下面的循环什么也不查，判据就空转了：先数一遍
    n_eq = {"A": 0, "P": 0}
    for ln in lines:
        t = ln.split()
        if len(t) == 6 and t[0] == "eq" and t[1] in n_eq:
            n_eq[t[1]] += 1
    if n_eq["P"] != 1:
        bad("counts eq P", "应恰好 1 行，实际 %d 行" % n_eq["P"])
    if n_eq["A"] == 0:
        bad("counts eq A", "一行都没有")

    for ln in lines:
        t = ln.split()
        if len(t) == 6 and t[0] == "eq":
            kind = t[1]
            copy, alpha, add, straight = (int(x) for x in t[2:6])
            if kind == "A":
                if copy > 0 and alpha > 0 and add > 0:
                    ok("counts eq A 三个方程都非 0（%d %d %d %d）" % (copy, alpha, add, straight))
                else:
                    bad("counts eq A", "copy=%d alpha=%d add=%d" % (copy, alpha, add))
            elif kind == "P":
                if copy > 0 and alpha > 0 and straight > 0:
                    ok("counts eq P copy/alpha/straight 都非 0（%d %d %d %d）"
                       % (copy, alpha, add, straight))
                else:
                    bad("counts eq P", "copy=%d alpha=%d straight=%d" % (copy, alpha, straight))


# ---------------------------------------------------------------- D3 后半

def mode_quick_prefix(full, quick):
    fl, ql = norm(full), norm(quick)
    fg, qg = groups(fl), groups(ql)
    for key in sorted(qg.keys()):
        if key not in fg:
            bad("quick %s 段 p%s 不在全量档里" % key, "")
            return
        f, q = fg[key], qg[key]
        if len(q) > len(f):
            bad("quick %s p%s 比全量档还长" % key, "%d > %d" % (len(q), len(f)))
            return
        for i, (x, y) in enumerate(zip(f, q)):
            if strip_cyc(x) != strip_cyc(y):
                bad("quick %s p%s 第 %d 帧" % (key[0], key[1], i), "与全量档不是前缀")
                return
    # 非逐帧行：文件头那一段（banner／pak／scene／eq P）与末尾的 fail／done 必须一致。
    # 唯一的例外是 `eq A`：它是**这一遍实际跑过的帧**的累计，快速档截短之后必然不同，
    # 所以不参与前缀核对（它自己的判据在 --counts 里）。
    def others(lines):
        out = []
        for ln in lines:
            if parse_f(ln) is not None:
                continue                    # f 行由前缀核对管
            if ln.split()[:2] == ["eq", "A"]:
                continue                    # 这一遍的累计，截短后必然不同
            out.append(ln)
        return out
    if others(fl) != others(ql):
        bad("quick 非逐帧行", "与全量档不一致")
        return
    ok("quick 快速档逐帧行按段按遍是全量档的前缀（%d 组）" % len(qg))


def main(argv):
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("--compare", nargs=2, metavar=("A", "B"))
    ap.add_argument("--pair")
    ap.add_argument("--hud")
    ap.add_argument("--font")
    ap.add_argument("--scene")
    ap.add_argument("--counts")
    ap.add_argument("--quick-prefix", nargs=2, metavar=("FULL", "QUICK"))
    ap.add_argument("--board", nargs=2, metavar=("BOARD", "EXPECT"))
    args = ap.parse_args(argv)

    did = False
    if args.compare:
        did = True
        mode_compare(args.compare[0], args.compare[1])
    if args.pair:
        did = True
        mode_pair(args.pair)
    if args.hud:
        did = True
        if not args.font:
            ap.error("--hud 要配 --font")
        mode_hud(args.hud, args.font)
    if args.scene:
        did = True
        mode_scene(args.scene)
    if args.counts:
        did = True
        mode_counts(args.counts)
    if args.quick_prefix:
        did = True
        mode_quick_prefix(args.quick_prefix[0], args.quick_prefix[1])
    if args.board:
        did = True
        mode_board(args.board[0], args.board[1])
    if not did:
        ap.error("至少要给一种模式")

    print("fail %d" % len(FAILED))
    return 0 if not FAILED else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
