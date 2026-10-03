#!/usr/bin/env python3
"""SW9 板上回放：按程序横幅截取本次日志，逐帧核对 PC 的 g 行。"""

import argparse
import hashlib
from pathlib import Path


def body(path, board):
    lines = [line.strip() for line in Path(path).read_text(
        encoding="utf-8", errors="replace").splitlines()]
    if board:
        banners = [i for i, line in enumerate(lines) if line == "gfx2dGame STG"]
        if not banners:
            raise ValueError("板上日志没有本次程序横幅")
        lines = lines[banners[-1]:]
    return [line for line in lines if line and not line.startswith("#")]


def frames(lines, expected):
    got = [line for line in lines if line.startswith("g ")]
    if len(got) != expected:
        raise ValueError("g 行数量 %d，预期 %d" % (len(got), expected))
    for i, line in enumerate(got):
        if line.split()[1] != str(i):
            raise ValueError("g 行帧号不连续：行 %d 是 %s" % (i, line))
    return got


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--reference", required=True, type=Path)
    ap.add_argument("--board", required=True, type=Path)
    ap.add_argument("--frames", required=True, type=int)
    ap.add_argument("--full", action="store_true", help="还核对 CLEAR 与末尾 end 行")
    group = ap.add_mutually_exclusive_group()
    group.add_argument("--compare-equal", action="store_true", help="同帧软硬 CRC 全相同")
    group.add_argument("--compare-different", action="store_true", help="注错后同帧 CRC 全不同")
    args = ap.parse_args()

    pc = body(args.reference, False)
    hw = body(args.board, True)
    if "fail 0" not in hw or "done" not in hw:
        raise SystemExit("FAIL: 板上日志缺 fail 0 / done")
    try:
        expected = [line for line in pc if line.startswith("g ")][:args.frames]
        actual = frames(hw, args.frames)
        if len(expected) != args.frames:
            raise ValueError("PC 参考日志不足 %d 帧" % args.frames)
        for i, (p, b) in enumerate(zip(expected, actual)):
            if p != b:
                raise ValueError("第 %d 帧不同\nPC: %s\n板: %s" % (i, p, b))
        if args.full:
            for prefix in ("clear ", "end frames "):
                p = [line for line in pc if line.startswith(prefix)]
                b = [line for line in hw if line.startswith(prefix)]
                if p != b:
                    raise ValueError("%s 行不同: PC=%s 板=%s" % (prefix, p, b))
        if args.compare_equal or args.compare_different:
            compares = [line.split() for line in hw if line.startswith("compare ")]
            if len(compares) != args.frames:
                raise ValueError("compare 行数量 %d，预期 %d" % (len(compares), args.frames))
            for i, parts in enumerate(compares):
                if len(parts) != 6 or parts[1] != str(i) or parts[2] != "sw" or parts[4] != "hw":
                    raise ValueError("compare 第 %d 行格式或帧号错误" % i)
                if (parts[3] == parts[5]) != args.compare_equal:
                    raise ValueError("compare 第 %d 帧 CRC 关系错误：%s" % (i, parts))
    except ValueError as exc:
        raise SystemExit("FAIL: %s" % exc)
    digest = hashlib.sha256(args.board.read_bytes()).hexdigest()
    print("PASS: %d 帧逐行相同，fail 0，done，board SHA256 %s" % (args.frames, digest))


if __name__ == "__main__":
    main()
