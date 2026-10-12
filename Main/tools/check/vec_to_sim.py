#!/usr/bin/env python3
# 把 check_cmd_semantics.py --emit 的标签文本展开成 engine_top 测试台能稳定解析的
# 定型记录流。
#
# 输入：emit 的文本（--emit 导出的向量集格式）
# 输出：一行一条记录、字段空格分隔、数字十六进制无前缀的 vec_stream.txt：
#   V lists reads code errlist erridx errtag mem clipx0 clipy0 clipx1 clipy1
#   S slot base width height ctrl
#   L count
#   C w0 w1 w2 w3 w4 w5 w6 w7
#   M kind addr word        kind 0 = 初始内容，1 = 期望内容
#   E                       一条向量结束
# clip 是该向量期望的**最终**裁剪框（闭区间）；errtag 保持 8 位十六进制。
# 测试台按 V 行提交 lists 次门铃，比较 code/erridx/errtag 与 mem=1 时的 [exp] 字。
import re
import sys

def main():
    src, dst = sys.argv[1], sys.argv[2]
    out = []
    vec = None
    with open(src, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line.startswith("[vector]"):
                m = re.match(r"\[vector\] group=(\S+) name=(\S+) lists=(\d+) reads=(\d+) "
                             r"code=(\d+) errlist=(\d+) erridx=(\d+) errtag=0x([0-9a-fA-F]+) "
                             r"clip=(-?\d+),(-?\d+),(-?\d+),(-?\d+) new=(\d+) mem=(\d+)", line)
                assert m, line
                g, n, lists, reads, code, errlist, erridx, errtag = m.group(1, 2, 3, 4, 5, 6, 7, 8)
                cx0, cy0, cx1, cy1, new, mem = m.group(9, 10, 11, 12, 13, 14)
                vec = {"name": f"{g}/{n}", "lists": int(lists)}
                out.append(f"V {lists} {reads} {code} {errlist} {erridx} {int(errtag, 16):08x} "
                           f"{mem} {int(cx0):04x} {int(cy0):04x} {int(cx1):04x} {int(cy1):04x}")
            elif line.startswith("[surf]"):
                m = re.match(r"\[surf\] slot=(\d+) base=0x([0-9a-fA-F]+) width=(\d+) height=(\d+) "
                             r"ctrl=0x([0-9a-fA-F]+)", line)
                assert m, line
                slot, base, w, h, ctrl = m.group(1, 2, 3, 4, 5)
                out.append(f"S {int(slot):02d} {int(base, 16):08x} {int(w):04x} {int(h):04x} {int(ctrl, 16):08x}")
            elif line.startswith("[list]"):
                m = re.match(r"\[list\] count=(\d+)", line)
                assert m, line
                out.append(f"L {int(m.group(1)):04x}")
            elif line.startswith("[cmd]"):
                ws = line.split()[1:]
                assert len(ws) == 8, line
                out.append("C " + " ".join(f"{int(w, 16):08x}" for w in ws))
            elif line.startswith("[mem]"):
                m = re.match(r"\[mem\]\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)", line)
                assert m, line
                out.append(f"M 0 {int(m.group(1), 16):08x} {int(m.group(2), 16):08x}")
            elif line.startswith("[exp]"):
                m = re.match(r"\[exp\]\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)", line)
                assert m, line
                out.append(f"M 1 {int(m.group(1), 16):08x} {int(m.group(2), 16):08x}")
            elif line.startswith("E"):
                pass
    with open(dst, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out) + "\n")
    nv = sum(1 for r in out if r.startswith("V "))
    print(f"vec_to_sim: {nv} 条向量，{len(out)} 行记录 -> {dst}")

if __name__ == "__main__":
    main()
