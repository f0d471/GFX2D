#!/usr/bin/env python3
"""SW10 console（FreeRTOS 游戏机前端）的板上判据。

构建 console 并在板上走完整前端链：启动 -> 菜单 -> 进弹幕游戏 -> HOME 回菜单
-> 进三维 -> HOME 回菜单 -> 菜单驻留（收 monitor 的 rtos tick 行与 menu fps 行）。
输入用 board_run.py 的 --input-script 注入（时间起点 = `console enter menu`），
按键位图与 bsp/include/board.h 一致（MODE1=128 MODE2=256 MODE3=512，
HOME 和弦 = 896，需按住 >=8 场）。

判据（全部满足才 PASS）：
  1. 行序：console boot -> pak len -> console enter menu -> menu crc
           -> console enter stg -> console leave stg -> console enter menu
           -> console enter r4 -> console leave r4 -> console enter menu
  2. 引擎渲染的菜单帧率：menu fps 行里至少一行 fps >= 45
  3. RTOS 存活：至少一行 `rtos tick`（monitor 在菜单驻留时说话）
  4. 不出现 err 行（console boot err / enter err / assert / overflow）

用法（Efinity 自带 python）：
  set PYTHONHOME=<EFINITY>\\python311
  <EFINITY>\\python311\\bin\\python.exe tools\\check\\check_console_board.py
  可选：--bit <位流>（缺省 board_release_game_current_20261024/board/B7.bit，
  JTAG 易失烧写，不动 Flash）、--seconds、--keep-log、--log、--no-run。
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MAIN = REPO

B7_DEFAULT = os.path.join(MAIN, "bit", "board_release_game_current_20261024",
                          "board", "B7.bit")
# console 按 -perf 档构建（PERF=1：SW9_PERF 打开引擎流水，见其 makefile 注释）
CONSOLE_BIN = os.path.join(MAIN, "sw", "apps", "console", "build", "console-O2-perf.bin")

# 输入脚本时间轴（毫秒:位图），起点是 `console enter menu`：
#   1.0s 按 MODE2 进弹幕游戏，1.1s 松开
#   3.2s 起按住 HOME 和弦（三模式键），每 250ms 重发一次，4.95s 松开 -> 回菜单
#   5.5s 按 MODE3 进三维，5.6s 松开
#   8.0s 起按住 HOME 和弦（同上重发），9.75s 松开 -> 回菜单，之后驻留收 tick/fps 行
# 按住必须重发：board_keys() 对串口键包有 300ms 无包自动清零（为真实手柄的
# 持续转发设计），一次性发一拍 896 在板侧只等效按住 300ms——按住窗口里的
# runner 帧数不足 8（应用帧率低或抖动时）和弦就永远凑不满。真实手柄经
# input_forward 的持续发包不经过这个坑；判据脚本必须模拟同样的按住形态。
INPUT_SCRIPT = ("1000:256,1100:0,"
                "3200:896,3450:896,3700:896,3950:896,4200:896,4450:896,4700:896,4950:0,"
                "5500:512,5600:0,"
                "8000:896,8250:896,8500:896,8750:896,9000:896,9250:896,9500:896,9750:0")

LINE_ORDER = [
    "console boot",
    "pak len",
    "console enter menu",
    "menu crc",
    "console enter stg",
    "console leave stg",
    "console enter menu",
    "console enter r4",
    "console leave r4",
    "console enter menu",
]

BAD_PATTERNS = [
    "console boot err",
    "console enter err",
    "console assert",
    "console stack overflow",
    "console malloc failed",
    "rt boot err",
]


def build_console() -> int:
    """WSL 里编 console（xPack 工具链），产物缺失或编译失败返回非 0。"""
    if os.path.isfile(CONSOLE_BIN):
        return 0
    # 仓库路径经 wslpath 换算成 WSL 侧路径，不写死挂载点
    r = subprocess.run(["wsl", "-e", "wslpath", "-a", MAIN],
                       capture_output=True, text=True, errors="replace")
    repo_wsl = r.stdout.strip().replace("\\", "/")
    if not repo_wsl.startswith("/"):
        sys.stderr.write("wslpath 换算失败：%r\n" % r.stdout)
        return 1
    r = subprocess.run(["wsl", "-e", "bash", "-c",
                        'export PATH="$HOME/opt/xpack-riscv-none-elf-gcc-13.4.0-1/bin:$PATH"; '
                        'cd "%s/sw/apps/console" && make' % repo_wsl],
                       capture_output=True, text=True, errors="replace")
    if r.returncode != 0:
        sys.stderr.write(r.stdout[-4000:] + r.stderr[-2000:])
        return 1
    return 0 if os.path.isfile(CONSOLE_BIN) else 1


def run_board(bit: str, seconds: int, log_path: str) -> int:
    """board_run.py 走 JTAG 加载 + 脚本化按键，返回其退出码。"""
    cmd = [
        sys.executable, os.path.join(MAIN, "tools", "board", "board_run.py"),
        "--bin", CONSOLE_BIN,
        "--bit", bit,
        "--seconds", str(seconds),
        "--input-script", INPUT_SCRIPT,
        "--input-script-after", "console enter menu",
        "--log", log_path,
    ]
    r = subprocess.run(cmd)
    return r.returncode


def judge(log_path: str) -> bool:
    with open(log_path, "r", encoding="utf-8", errors="replace") as f:
        lines = f.readlines()
    text = "".join(lines)

    ok = True

    # 1. 行序
    pos = 0
    for marker in LINE_ORDER:
        found = text.find(marker, pos)
        if found < 0:
            print("FAIL 行序缺失：%s（已到偏移 %d）" % (marker, pos))
            ok = False
            break
        pos = found + len(marker)
    else:
        print("PASS 行序：%d 个标记按序出现" % len(LINE_ORDER))

    # 2. 菜单帧率（引擎渲染）
    fps_hits = [int(m) for m in re.findall(r"menu fps (\d+)", text)]
    if any(v >= 45 for v in fps_hits):
        print("PASS 菜单帧率：menu fps 样本 %s" % fps_hits[:8])
    else:
        print("FAIL 菜单帧率：样本 %s 无一 >= 45" % fps_hits[:8])
        ok = False

    # 3. RTOS 存活
    if re.search(r"rtos tick \d+", text):
        print("PASS RTOS 存活：rtos tick 行出现")
    else:
        print("FAIL RTOS 存活：没有 rtos tick 行")
        ok = False

    # 4. 错误行
    bad = [p for p in BAD_PATTERNS if p in text]
    if bad:
        print("FAIL 出现错误行：%s" % bad)
        ok = False
    else:
        print("PASS 无错误行")

    return ok


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bit", default=B7_DEFAULT, help="位流（缺省 B7）")
    ap.add_argument("--seconds", type=int, default=40, help="串口采集秒数")
    ap.add_argument("--no-run", action="store_true", help="只判据已有日志（配 --log）")
    ap.add_argument("--log", help="串口日志路径（缺省临时文件）")
    ap.add_argument("--keep-log", action="store_true", help="结束时打印日志路径")
    args = ap.parse_args()

    if not os.path.isfile(args.bit):
        print("FAIL 找不到位流：%s" % args.bit)
        return 1

    log_path = args.log or os.path.join(tempfile.gettempdir(), "check_console_board.log")

    if not args.no_run:
        rc = build_console()
        if rc != 0:
            print("FAIL console 构建失败")
            return 1
        rc = run_board(args.bit, args.seconds, log_path)
        if rc != 0:
            print("FAIL board_run 退出码 %d（烧写/加载/串口失败），日志 %s" % (rc, log_path))
            return 1

    ok = judge(log_path)
    if args.keep_log or not ok:
        print("日志：%s" % log_path)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
