#!/usr/bin/env python3
"""复核 SW9 入库串口日志；只检查已采集产物，不连接板卡。"""

import subprocess
import sys
from pathlib import Path


HERE = Path(__file__).resolve().parent
MAIN = HERE.parent.parent
LOG = MAIN / "sw/apps/game/gfx2dGame/logs"


def run(script, *args):
    subprocess.run([sys.executable, str(HERE / script), *map(str, args)], check=True)


def body(path):
    return path.read_text(encoding="utf-8", errors="replace")


def need(ok, what):
    if not ok:
        raise SystemExit("FAIL: " + what)


def main():
    pc = LOG / "sw9_pc_reference.log"
    board = "check_sw9_board.py"
    run(board, "--reference", pc, "--board", LOG / "sw9_replay_sw600.log",
        "--frames", 600)
    run(board, "--reference", pc, "--board", LOG / "sw9_replay_hw9208.log",
        "--frames", 9208, "--full")
    run(board, "--reference", pc, "--board", LOG / "sw9_compare_normal.log",
        "--frames", 10, "--compare-equal")
    run(board, "--reference", pc, "--board", LOG / "sw9_compare_skip_invalidate.log",
        "--frames", 10, "--compare-different")
    run(board, "--reference", LOG / "sw9_pc_eq_copy.log",
        "--board", LOG / "sw9_eq_copy.log", "--frames", 10)
    clean = body(pc).splitlines()
    inject = body(LOG / "sw9_pc_eq_copy.log").splitlines()
    need(next(s for s in clean if s.startswith("g 0 ")) !=
         next(s for s in inject if s.startswith("g 0 ")),
         "COPY 方程注错未改变第一帧")

    perf = "check_sw9_perf.py"
    for name in ("sw9_perf_hw_demo.log", "sw9_perf_hw_game.log"):
        run(perf, LOG / name, "--warmup-rows", 1, "--steady-drops-max", 0)
    for name in ("sw9_perf_sw_game.log", "sw9_perf_hw_s1_60s.log",
                 "sw9_perf_sw_s1_60s.log", "sw9_perf_sw_s1_hot.log",
                 "sw9_switch_board.log"):
        run(perf, LOG / name)
    for name in ("sw9_perf_hw_k896_60s.log", "sw9_perf_hw_k864_60s.log"):
        run(perf, LOG / name, "--fields-min", 3600,
            "--warmup-rows", 1, "--steady-drops-max", 0)
    run(perf, LOG / "sw9_perf_hw_nmax.log")
    run(perf, LOG / "sw9_perf_sw_nmax.log")
    run("check_sw9_nmax.py", LOG / "sw9_perf_hw_nmax.log", "--status", "OK")
    run("check_sw9_nmax.py", LOG / "sw9_perf_sw_nmax.log",
        "--status", "BELOW_N0")
    run("check_sw9_flash.py", LOG / "sw9_flash_boot.log")

    keys = body(MAIN / "sw/apps/test/sw9InputTest/sw9_input.log")
    for marker in ("keys frame 11 value 00000080 pressed 00000080",
                   "keys frame 12 value 00000000 pressed 00000000",
                   "keys frame 33 value 00000040 pressed 00000040",
                   "keys frame 51 value 00000000 pressed 00000000"):
        need(marker in keys, "串口短按或超时: " + marker)
    switch = body(LOG / "sw9_switch_board.log")
    seq = ("mode 2 enter", "mode 3 enter", "mode 1 enter",
           "backend sw frame", "backend hw frame")
    need([switch.index(s) for s in seq] == sorted(switch.index(s) for s in seq),
         "三模式与两后端切换顺序")

    no_a = body(LOG / "sw9_inject_no_irq_a.log")
    no_b = body(LOG / "sw9_inject_no_irq_b.log")
    no_clear = body(LOG / "sw9_inject_skip_clear_b.log")
    need("err present v 0" in no_a and "irqA 0" in no_a and "fail 2" in no_a,
         "禁用场中断没有让呈现超时")
    need("engine err code 4294967295" in no_b and "irqB 0" in no_b and
         "fail 2" in no_b, "禁用引擎中断没有让执行超时")
    need("csvh," in no_clear and "done" not in no_clear,
         "漏清引擎中断没有使程序停住")
    print("PASS: SW9 入库日志全部复核通过")


if __name__ == "__main__":
    main()
