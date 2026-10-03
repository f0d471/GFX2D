#!/usr/bin/env python3
"""核对 SW9 的 Flash 自启动交互采集与三模式、两后端切换。"""

import argparse
import csv
from pathlib import Path


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("log", type=Path)
    args = ap.parse_args()
    lines = [line.strip() for line in args.log.read_text(
        encoding="utf-8", errors="replace").splitlines()]
    banners = [i for i, line in enumerate(lines) if line == "gfx2dGame MODES"]
    if not banners:
        raise SystemExit("FAIL: 缺本次程序横幅")
    lines = lines[banners[-1]:]
    if "pak 2840320 b370ba0a" not in lines:
        raise SystemExit("FAIL: Flash 素材包身份不符")
    if any(line.startswith(("err ", "engine err ", "fail ")) for line in lines):
        raise SystemExit("FAIL: 游戏报错")

    order = ("mode 2 enter", "mode 3 enter", "mode 1 enter",
             "backend sw frame", "backend hw frame")
    positions = []
    for prefix in order:
        found = [i for i, line in enumerate(lines) if line.startswith(prefix)]
        if len(found) != 1:
            raise SystemExit("FAIL: %s 出现 %d 次" % (prefix, len(found)))
        positions.append(found[0])
    if positions != sorted(positions):
        raise SystemExit("FAIL: 模式与后端切换顺序不符")

    heads = [next(csv.reader([line])) for line in lines if line.startswith("csvh,")]
    if len(heads) != 1:
        raise SystemExit("FAIL: CSV 表头数量不符")
    rows = [dict(zip(heads[0], next(csv.reader([line]))))
            for line in lines[positions[-1] + 1:] if line.startswith("csv,")]
    steady = [row for row in rows if row.get("mode") == "demo" and
              row.get("exec") == "hw" and row.get("fps") == "60" and
              row.get("drop") == "0"]
    if len(steady) < 3:
        raise SystemExit("FAIL: 回到硬件 Demo 后缺至少三个 60/60 窗口")
    if not any(int(row["hw_cmd_cum"]) > 0 for row in steady):
        raise SystemExit("FAIL: 硬件计数器没有增长")
    print("PASS: Flash 素材匹配，三模式与两后端依次切换，硬件 Demo %d 个 60/60 窗口" %
          len(steady))


if __name__ == "__main__":
    main()
