#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""gen_sin_tab.py —— 生成 SW7 数值基础的正弦表 `Main/sw/lib/core/sin_tab.c`

表的定义如下：

    1024 项一周，加 256 项尾巴（前 256 项的重复），共 1280 项 int16；
    sin_tab[i] = 16384 * sin(2*pi*i / 1024)，幅度 Q1.14（sin 90 度 = 16384）；
    余弦取 sin_tab[i + 256]。

**取整按绝对值四舍五入、再补回符号**：`sign(v) * floor(|v| + 0.5)`。
这样表严格奇对称（sin_tab[1024 - i] == -sin_tab[i]），负半周与正半周逐项互为相反数。
不用 Python 的 `round()`：它是银行家舍入（恰在 .5 时取偶），与 C 一侧与 B 批独立实现的规则不同。
本表的 1280 个值里没有恰在 .5 上的项，两种舍入在这里给出同一张表，
但规则写成 floor 形式，谁照着重写都得到同一个答案。

入库的 `sin_tab.c` 是本脚本的产物，不手改。`check_sw7_game.sh` 每次重新生成一遍、与入库的逐字节比；
`--inject` 把第 1 项的最低位取反，给那条比对当注错用（它必须因此见红）。

用法：

    python3 Main/tools/check/gen_sin_tab.py              写进 Main/sw/lib/core/sin_tab.c（入库的那份）
    python3 Main/tools/check/gen_sin_tab.py --out <文件>  写到别处（判据用）
    python3 Main/tools/check/gen_sin_tab.py --inject --out <文件>   注错：第 1 项最低位取反

只用标准库，不依赖板子、编译器与 SDL。退出码 0 表示成功。
"""

import argparse
import math
import os
import sys

N = 1024            # 一周的项数
TAIL = 256          # 尾巴：余弦取 sin_tab[i + 256]
ONE = 16384         # Q1.14 的 1.0
PER_LINE = 8

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_OUT = os.path.normpath(os.path.join(HERE, "..", "..", "sw", "lib", "core", "sin_tab.c"))


def round_half_away(v):
    """按绝对值四舍五入再补回符号。spec 2.1 节的规则，不用 round()。"""
    r = int(math.floor(abs(v) + 0.5))
    return -r if v < 0 else r


def table():
    vals = [round_half_away(ONE * math.sin(2.0 * math.pi * i / N)) for i in range(N)]
    vals += vals[:TAIL]
    return vals


def render(vals):
    lines = []
    lines.append("/* 由 Main/tools/check/gen_sin_tab.py 生成，不手改。")
    lines.append("   口径见 Main/tools/check/gen_sin_tab.py 的 table()/render()：1024 项一周加 256 项尾巴，")
    lines.append("   Q1.14，按绝对值四舍五入再补回符号。check_sw7_game.sh 重新生成并逐字节比对。 */")
    lines.append("")
    lines.append('#include "core/fixmath.h"')
    lines.append("")
    lines.append("/* 不写长度：与 fixmath.h 里带长度的 extern 声明对不上时编译报类型冲突，")
    lines.append("   项数写错不会被零填充悄悄补齐。 */")
    lines.append("const int16_t fx_sin_tab[] = {")
    for k in range(0, len(vals), PER_LINE):
        chunk = vals[k:k + PER_LINE]
        lines.append("    " + " ".join("%6d," % v for v in chunk) + "   /* %4d */" % k)
    lines.append("};")
    lines.append("")
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description="生成 SW7 的 1280 项 Q1.14 正弦表")
    ap.add_argument("--out", default=DEFAULT_OUT, help="输出文件，默认写进 lib/core/sin_tab.c")
    ap.add_argument("--inject", action="store_true", help="注错：第 1 项最低位取反")
    args = ap.parse_args()

    vals = table()
    if len(vals) != N + TAIL:
        print("gen_sin_tab: 项数 %d 不是 %d" % (len(vals), N + TAIL), file=sys.stderr)
        return 1
    if args.inject:
        vals[1] ^= 1

    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        f.write(render(vals))
    print("gen_sin_tab: %d 项 -> %s%s" % (len(vals), args.out, "（注错）" if args.inject else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
