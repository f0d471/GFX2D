#!/usr/bin/env python3
"""独立复核 SW9 性能日志的 CSV 算术、场数和扫描欠载。"""

import argparse
from pathlib import Path


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("log", type=Path)
    ap.add_argument("--fields-min", type=int, default=0)
    ap.add_argument("--drops-max", type=int)
    ap.add_argument("--warmup-rows", type=int, default=0)
    ap.add_argument("--steady-drops-max", type=int)
    args = ap.parse_args()

    lines = [line.strip() for line in args.log.read_text(
        encoding="utf-8", errors="replace").splitlines()]
    banners = [i for i, line in enumerate(lines) if line == "gfx2dGame MODES"]
    if not banners:
        raise SystemExit("FAIL: 缺本次程序横幅")
    lines = lines[banners[-1]:]
    if "fail 0" not in lines or "done" not in lines or any(
            line.startswith("err ") or line.startswith("engine err ") for line in lines):
        raise SystemExit("FAIL: 程序报错或缺 fail 0 / done")
    heads = [line.split(",") for line in lines if line.startswith("csvh,")]
    sums = [line.split(",") for line in lines if line.startswith("csvsum,")]
    rows = [line.split(",") for line in lines if line.startswith("csv,")]
    if len(heads) != 1 or len(sums) != 1 or not rows:
        raise SystemExit("FAIL: CSV 表头、汇总或数据行数量错误")
    header = heads[0]
    if len(set(header)) != len(header):
        raise SystemExit("FAIL: CSV 表头列名重复")
    for i, cells in enumerate(rows):
        if len(cells) != len(header) or cells[1] != str(i):
            raise SystemExit("FAIL: CSV 第 %d 行列数或序号错误" % i)
        d = dict(zip(header, cells))
        fields = int(d["window_fields"])
        fps = int(d["fps"])
        if int(d["drop"]) != max(fields - fps, 0):
            raise SystemExit("FAIL: CSV 第 %d 行 drop 算术不符" % i)
        if int(d["fps_milli"]) != fps * 60000 // fields:
            raise SystemExit("FAIL: CSV 第 %d 行 fps_milli 算术不符" % i)
    if args.steady_drops_max is not None:
        if args.warmup_rows >= len(rows):
            raise SystemExit("FAIL: 没有稳态 CSV 窗口")
        steady = sum(int(dict(zip(header, r))["drop"])
                     for r in rows[args.warmup_rows:])
        if steady > args.steady_drops_max:
            raise SystemExit("FAIL: 稳态掉帧 %d 超过上限 %d" % (
                steady, args.steady_drops_max))
    summary = sums[0]
    if len(summary) != 8 or int(summary[6]) != len(rows):
        raise SystemExit("FAIL: csvsum 列数或 rows 不符")
    frames, fields, drops = map(int, summary[1:4])
    if fields < args.fields_min or (args.drops_max is not None and drops > args.drops_max):
        raise SystemExit("FAIL: fields=%d drops=%d 超出要求" % (fields, drops))
    diag = [line for line in lines if line.startswith("diag ")]
    if len(diag) != 1:
        raise SystemExit("FAIL: 缺扫描诊断行")
    parts = diag[0].split()
    values = dict(zip(parts[1::2], parts[2::2]))
    if int(values["zero"]) != 0 or int(values["under"]) != 0 or int(values["faults"]) != 0:
        raise SystemExit("FAIL: 扫描欠载或中断故障：%s" % diag[0])
    rates = [int(dict(zip(header, r))["fps_milli"]) for r in rows]
    print("PASS: frames=%d fields=%d drops=%d rows=%d fps_milli=[%d,%d] %s" % (
        frames, fields, drops, len(rows), min(rates), max(rates), diag[0]))


if __name__ == "__main__":
    main()
