#!/usr/bin/env python3
"""核对 SW9 板上 N_max 的两种弹型与 3600 场确认结果。"""

import argparse
import re
from pathlib import Path


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("log", type=Path)
    ap.add_argument("--status", choices=("OK", "BELOW_N0"), required=True)
    args = ap.parse_args()

    lines = [line.strip() for line in args.log.read_text(
        encoding="utf-8", errors="replace").splitlines()]
    starts = [i for i, line in enumerate(lines) if line == "gfx2dGame MODES"]
    if not starts:
        raise SystemExit("FAIL: 缺本次程序横幅")
    lines = lines[starts[-1]:]
    if lines.count("nmax load 16x16") != 1 or lines.count("nmax load 8x8") != 1:
        raise SystemExit("FAIL: 两种弹型没有各搜索一次")
    if "nmax done" not in lines or "fail 0" not in lines or "done" not in lines:
        raise SystemExit("FAIL: 搜索没有正常结束")
    results = [line for line in lines if line.startswith("nmax result ")]
    if len(results) != 2:
        raise SystemExit("FAIL: 结果数量不是 2")
    for i, line in enumerate(results):
        if not line.startswith("nmax result " + args.status + " "):
            raise SystemExit("FAIL: 第 %d 种弹型状态不符: %s" % (i, line))
        if args.status == "OK":
            fields = re.search(r"n_max (\d+) knob (\d+)", line)
            if not fields:
                raise SystemExit("FAIL: 结果缺 N_max 或旋钮")
            before = lines[:lines.index(line)]
            confirms = [s for s in before if " confirm knob " in s and s.startswith("nmax probe ")]
            if not confirms:
                raise SystemExit("FAIL: 缺确认探针")
            probe = confirms[-1]
            want = ("knob %s" % fields.group(2), "fields 3600", "drops 0",
                    "act_min %s" % fields.group(1), " pass")
            if any(part not in probe for part in want):
                raise SystemExit("FAIL: 长时确认与结果不符: %s / %s" % (probe, line))
        elif "n_max - knob -" not in line:
            raise SystemExit("FAIL: BELOW_N0 应无正数 N_max: %s" % line)
    print("PASS: 16x16 与 8x8 均为 %s；%s；%s" % (args.status, *results))


if __name__ == "__main__":
    main()
