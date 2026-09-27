#!/usr/bin/env python3
"""按原始 CRC 复核 E7 注错日志；旧日志的 CHANGED/same 与 FAIL 标签不作证据。

默认复核六个旧方程注错日志和一个错配日志。可另给本次两份 VERIFY_ONLY
板上日志，要求完整的程序结束、正确的判分与没有性能扫描。
"""
import argparse
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[3]
LOGS = ROOT / "Main/sw/apps/probe/execSwTest/logs"
GOLDEN = ROOT / "Main/sw/lib/test/golden.txt"
CHANGES = {
    "ALPHA": {3}, "ADD": {5}, "CONST": {6}, "MUL": {7},
    "ALPHA_STRAIGHT": {4}, "COPY": {0, 1, 2, 5, 8, 9, 10},
    "self": {0, 1, 2, 5, 8, 9, 10},
}
ROW = re.compile(r"^(\d+) ([\w-]+) crc 0x([0-9a-fA-F]{8})\b", re.M)


def check(log, kind, golden, live=False):
    text = log.read_text(encoding="utf-8")
    rows = ROW.findall(text)
    indices = [int(r[0]) for r in rows]
    if len(rows) != 11 or sorted(indices) != list(range(11)):
        raise ValueError(f"{log.name}: VERIFY 必须恰好有 0~10 各一行")
    changed = set()
    for idx, name, crc in rows:
        i = int(idx)
        if name != golden[i][0]:
            raise ValueError(f"{log.name}: 用例 {i} 名称不一致")
        if int(crc, 16) != golden[i][1]:
            changed.add(i)
    expected_observation = {3} if kind == "self" else CHANGES[kind]
    if changed != expected_observation:
        raise ValueError(f"{log.name}: CRC 变化集合 {sorted(changed)} 不符")
    mismatches = len(changed ^ CHANGES[kind])
    expected_fail = 8 if kind == "self" else 0
    if mismatches != expected_fail:
        raise ValueError(f"{log.name}: 独立判分 {mismatches} 不符")
    done = bool(re.search(r"^done\s*$", text, re.M))
    if live:
        fails = re.findall(r"^fail (\d+)\s*$", text, re.M)
        if fails != [str(expected_fail)] or not done:
            raise ValueError(f"{log.name}: 缺少唯一 fail {expected_fail} 或 done")
        if "execSwTest E7" not in text or re.search(r"^THR |^OVH ", text, re.M):
            raise ValueError(f"{log.name}: 不是完整的 VERIFY_ONLY 输出")
    print(f"{log.name}: verify=11 changed={','.join(map(str, sorted(changed)))} "
          f"judge_fail={mismatches} done={'yes' if done else 'no'} "
          f"scope={'complete-verify-only' if live else 'raw-verify-evidence'} OK")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--logs", type=Path, default=LOGS)
    ap.add_argument("--golden", type=Path, default=GOLDEN)
    ap.add_argument("--board-alpha", type=Path)
    ap.add_argument("--board-self", type=Path)
    args = ap.parse_args()
    golden = [(parts[0], int(parts[1], 16)) for line in
              args.golden.read_text(encoding="utf-8").splitlines()
              if (parts := line.split())]
    if len(golden) != 11:
        raise ValueError("golden 必须恰好有 11 项")
    for kind in CHANGES:
        check(args.logs / f"board_inj_{kind}.log", kind, golden)
    for kind, log in (("ALPHA", args.board_alpha), ("self", args.board_self)):
        if log:
            check(log, kind, golden, live=True)
    print("E7 injection evidence PASS")


if __name__ == "__main__":
    main()
