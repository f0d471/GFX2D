#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""gen_sw6_replay.py —— 生成 SW6 基础 Demo 的两段确定性录像

按键序列的形状是 spec 定死的（Engineering/TechReports/sw/05-基础Demo.md 3.10 节）：

    A   停 10 帧；上键 49 次（每次按 1 帧、松 1 帧）把 N 从 100 升到 5000；
        在 5000 停 30 帧；下键 49 次回到 100；停 10 帧。共 246 帧。
    B   k=30 停 31 帧（至少切一次）；下键 29 次把 k 降到 1；在 k=1 停 30 帧；
        上键 119 次升到 120；停 10 帧。共 367 帧。

「按下沿」在应用里判：按 1 帧、松 1 帧正好给一个沿，所以两帧一步。

格式的权威是 `Main/sw/lib/core/replay.h`（16 字节头 + 每帧 4 字节小端）。本脚本是
**独立于 `replay2c.py` 的第二份写法**：只按格式写字节，不解析、不复用它的代码。
写完由 `check_sw6_demo.sh` 做两件事验它——用 `replay2c.py --check` 独立解析一遍，
再把本脚本重新生成的字节与入库的那份逐字节比。

用法：

    python3 Main/tools/check/gen_sw6_replay.py              写进应用的 replay/（入库的那两份）
    python3 Main/tools/check/gen_sw6_replay.py --out <目录>  写到别处（判据的 D1 用）
    python3 Main/tools/check/gen_sw6_replay.py --to-text     打成录制文本打到 stdout

只用标准库，不依赖板子、编译器与 SDL。退出码 0 表示成功。
"""

import argparse
import os
import struct
import sys

MAGIC = 0x52443247          # 文件头前四字节是 'G','2','D','R'
VERSION = 1
HEADER_BYTES = 16
RECORD_BYTES = 4

# board.h 的 BOARD_KEY_* 位分配（这里只用上下两个）
KEY_UP = 1 << 0
KEY_DOWN = 1 << 1

# 两个种子都非零：0 是 xorshift32 的不动点。
SEED_A = 0x5E6A0001
SEED_B = 0x5E6B0002

# spec 4.11 第 8 条的帧数，用来断言生成器的形状没跑偏
FRAMES_A = 246
FRAMES_B = 367


def seq_a():
    keys = []
    keys += [0] * 10                    # 起步：N=100
    for _ in range(49):                 # 100 -> 5000
        keys += [KEY_UP, 0]
    keys += [0] * 30                    # 在 5000 停留，给容量判据留出帧
    for _ in range(49):                 # 5000 -> 100
        keys += [KEY_DOWN, 0]
    keys += [0] * 10
    return keys


def seq_b():
    keys = []
    keys += [0] * 31                    # k=30，停 31 帧（至少切一次）
    for _ in range(29):                 # 30 -> 1
        keys += [KEY_DOWN, 0]
    keys += [0] * 30                    # 在 k=1 停留，给逐帧轮换留出约 10 轮
    for _ in range(119):                # 1 -> 120
        keys += [KEY_UP, 0]
    keys += [0] * 10
    return keys


def pack(seed, keys):
    body = b"".join(struct.pack("<I", k & 0xFFFFFFFF) for k in keys)
    return struct.pack("<IHHII", MAGIC, VERSION, 0, seed & 0xFFFFFFFF, len(keys)) + body


def to_text(name, seed, keys):
    lines = ["# %s: g2dr 1 %d %d（%d 字节）" % (name, seed, len(keys), HEADER_BYTES + RECORD_BYTES * len(keys))]
    lines.append("g2dr %d %d %d" % (VERSION, seed, len(keys)))
    lines += ["key %d %08x" % (i, k) for i, k in enumerate(keys)]
    return "\n".join(lines) + "\n"


def build():
    """返回 [(名字, 种子, 按键表, 期望帧数)]，形状不符就抛 AssertionError。"""
    a, b = seq_a(), seq_b()
    assert len(a) == FRAMES_A, "A 段 %d 帧，spec 写的是 %d" % (len(a), FRAMES_A)
    assert len(b) == FRAMES_B, "B 段 %d 帧，spec 写的是 %d" % (len(b), FRAMES_B)
    return [("sw6_a", SEED_A, a), ("sw6_b", SEED_B, b)]


def default_dir():
    here = os.path.dirname(os.path.abspath(__file__))
    return os.path.normpath(os.path.join(here, "..", "..", "sw", "apps", "game", "gfx2dGame", "replay"))


def main(argv):
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("--out", help="输出目录，默认是应用的 replay/")
    ap.add_argument("--to-text", action="store_true", help="打到 stdout（录制文本），不写文件")
    args = ap.parse_args(argv)

    out = args.out or default_dir()
    if not args.to_text:
        os.makedirs(out, exist_ok=True)

    total = 0
    for name, seed, keys in build():
        blob = pack(seed, keys)
        total += len(blob)
        if args.to_text:
            sys.stdout.write(to_text(name, seed, keys))
            continue
        path = os.path.join(out, name + ".g2dr")
        with open(path, "wb") as f:
            f.write(blob)
        print("%s seed=%d frames=%d bytes=%d" % (path, seed, len(keys), len(blob)))
    if not args.to_text:
        print("done %d 字节" % total)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
