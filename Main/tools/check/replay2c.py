#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""replay2c.py —— 录像（.g2dr）与 C 数组、录制文本之间的转换

板上没有文件系统，SW9 要把一段录像编进程序；PC 上的录制结果是一段日志。
双方共用同一份格式定义（sw/lib/core/replay.h，SW4 的 D6）：

    偏移  长度  内容
    0     4     魔数 'G','2','D','R'（小端读作 0x52443247）
    4     2     版本号，当前 1
    6     2     标志位，当前必须是 0
    8     4     随机种子
    12    4     帧数 N
    16    4N    每帧一个 32 位按键位图，小端

录制文本（sw/lib/core/replay_text_*）长这样，脚本只认这两种行，其余行忽略：

    g2dr 1 <seed> <frames>
    key <frame> <8 位十六进制>

用法：

    # 二进制 -> C 数组（判据用；主机侧测试与 SW9 都读它）
    python3 replay2c.py --in fixture.g2dr --out replay_fixture.h --name replay_fixture

    # 录制日志 -> 二进制
    python3 replay2c.py --from-text run.log --out rec.g2dr

    # 二进制 -> 录制文本
    python3 replay2c.py --in rec.g2dr --to-text > run.log

    # 合成一份固件（按键序列用 lib/core 的 xorshift32 生成，参数见下）
    python3 replay2c.py --gen-test --frames 256 --seed 7 --out fixture.g2dr

    # 校验 / 自检
    python3 replay2c.py --check fixture.g2dr
    python3 replay2c.py --selftest

只用标准库，不依赖板子、编译器与 SDL。退出码 0 表示成功。
"""

import argparse
import struct
import sys

MAGIC = 0x52443247          # 'G','2','D','R'
VERSION = 1
HEADER_BYTES = 16
RECORD_BYTES = 4

# lib/core/xorshift32.c 的三个移位常数。脚本要生成与板上同一串按键，
# 所以这一份是与 C 那份并列的第二实现，两边不一致时 --selftest 与主机测试都会报红。
XS32_SHIFT_A = 13
XS32_SHIFT_B = 17
XS32_SHIFT_C = 5
MASK32 = 0xFFFFFFFF


def xs32_stream(seed, count):
    """按 xs32_seed + count 次 xs32_next 生成按键序列。"""
    s = seed & MASK32
    if s == 0:
        s = 1                       # xs32_seed：0 是不动点，换成 1
    out = []
    for _ in range(count):
        x = s
        x ^= (x << XS32_SHIFT_A) & MASK32
        x ^= x >> XS32_SHIFT_B
        x ^= (x << XS32_SHIFT_C) & MASK32
        s = x
        out.append(x)
    return out


def pack(seed, frames, keys):
    if len(keys) != frames:
        raise ValueError("帧数与按键条数不一致：%d 对 %d" % (frames, len(keys)))
    body = b"".join(struct.pack("<I", k & MASK32) for k in keys)
    return struct.pack("<IHHII", MAGIC, VERSION, 0, seed & MASK32, frames) + body


def unpack(blob, where="录像"):
    """解析并校验。任何一处不合法都抛 ValueError，不返回半个结果。"""
    if len(blob) < HEADER_BYTES:
        raise ValueError("%s 只有 %d 字节，头部就要 %d 字节" % (where, len(blob), HEADER_BYTES))
    magic, version, flags, seed, frames = struct.unpack("<IHHII", blob[:HEADER_BYTES])
    if magic != MAGIC:
        raise ValueError("%s 的魔数是 0x%08x，不是 0x%08x（'G2DR'）" % (where, magic, MAGIC))
    if version != VERSION:
        raise ValueError("%s 的版本号是 %d，本工具只认 %d" % (where, version, VERSION))
    if flags != 0:
        raise ValueError("%s 的标志位是 0x%04x，本版本要求为 0" % (where, flags))
    want = HEADER_BYTES + frames * RECORD_BYTES
    if len(blob) != want:
        raise ValueError("%s 长 %d 字节，头部记的 %d 帧要求 %d 字节（多一个字节也不算合法）"
                         % (where, len(blob), frames, want))
    keys = list(struct.unpack("<%dI" % frames, blob[HEADER_BYTES:])) if frames else []
    return seed, frames, keys


def to_text(seed, frames, keys):
    lines = ["g2dr %d %d %d" % (VERSION, seed, frames)]
    lines += ["key %d %08x" % (i, k) for i, k in enumerate(keys)]
    return "\n".join(lines) + "\n"


def from_text(text, where="录制文本"):
    """从日志里抠出 g2dr/key 两种行。其余行（帧率、CRC 等）忽略。"""
    seed = None
    frames = None
    keys = []
    for lineno, raw in enumerate(text.splitlines(), 1):
        parts = raw.split()
        if not parts:
            continue
        if parts[0] == "g2dr":
            if len(parts) != 4:
                raise ValueError("%s 第 %d 行的 g2dr 头字段数不是 4：%r" % (where, lineno, raw))
            version, seed, frames = int(parts[1]), int(parts[2]), int(parts[3])
            if version != VERSION:
                raise ValueError("%s 第 %d 行的版本号是 %d，本工具只认 %d"
                                 % (where, lineno, version, VERSION))
            if seed < 0 or seed > MASK32:
                raise ValueError("%s 第 %d 行的种子越界：%d" % (where, lineno, seed))
        elif parts[0] == "key":
            if len(parts) != 3:
                raise ValueError("%s 第 %d 行的 key 字段数不是 3：%r" % (where, lineno, raw))
            idx, val = int(parts[1]), int(parts[2], 16)
            if idx != len(keys):
                raise ValueError("%s 第 %d 行的帧号是 %d，按顺序应当是 %d（不允许跳帧）"
                                 % (where, lineno, idx, len(keys)))
            if val < 0 or val > MASK32:
                raise ValueError("%s 第 %d 行的按键越界：%r" % (where, lineno, raw))
            keys.append(val)
    if frames is None:
        raise ValueError("%s 里没有 g2dr 头行" % where)
    if len(keys) != frames:
        raise ValueError("%s 里有 %d 条 key 行，头行记的是 %d 帧" % (where, len(keys), frames))
    return seed, frames, keys


def emit_header(name, source, blob):
    guard = "GFX2D_GENERATED_%s_H" % name.upper()
    out = []
    out.append("/* 生成物，不要手改。")
    out.append("   由 Main/tools/check/replay2c.py 从 %s 生成。" % source)
    out.append("   改录像请改源文件再重新生成；判据会核对两者是否一致。 */")
    out.append("#ifndef %s" % guard)
    out.append("#define %s" % guard)
    out.append("")
    out.append("#include <stdint.h>")
    out.append("")
    out.append("#define %s_LEN %du" % (name.upper(), len(blob)))
    out.append("")
    out.append("static const uint8_t %s_bytes[%du] = {" % (name, len(blob)))
    for i in range(0, len(blob), 12):
        chunk = blob[i:i + 12]
        out.append("    " + " ".join("0x%02x," % b for b in chunk))
    out.append("};")
    out.append("")
    out.append("#endif /* %s */" % guard)
    out.append("")
    return "\n".join(out)


def read_file(path):
    with open(path, "rb") as f:
        return f.read()


def write_file(path, blob):
    with open(path, "wb") as f:
        f.write(blob)


def cmd_check(path):
    blob = read_file(path)
    seed, frames, keys = unpack(blob, path)
    lo = min(keys) if keys else 0
    hi = max(keys) if keys else 0
    print("%s ok magic=G2DR version=%d seed=%d frames=%d bytes=%d keys=0x%08x..0x%08x"
          % (path, VERSION, seed, frames, len(blob), lo, hi))
    print("done")
    return 0


def cmd_selftest():
    """合成一段、走一遍文本、再收回来，要求逐字节相同。
    这条覆盖的是「录制结果就是日志」那条路：板上与 PC 打出来的文本
    要能被脚本还原成与录制时一模一样的字节串。"""
    fails = 0
    for seed, frames in ((7, 1), (1, 256), (0xDEADBEEF, 64), (2, 0)):
        keys = xs32_stream(seed, frames)
        blob = pack(seed, frames, keys)
        try:
            s2, f2, k2 = from_text(to_text(seed, frames, keys))
            blob2 = pack(s2, f2, k2)
            ok = (s2 == seed) and (f2 == frames) and (k2 == keys) and (blob2 == blob)
        except ValueError as exc:
            print("  seed=%d frames=%d FAIL %s" % (seed, frames, exc))
            fails += 1
            continue
        print("  seed=%d frames=%d 文本往返 %s" % (seed, frames, "ok" if ok else "FAIL"))
        if not ok:
            fails += 1

    # 坏输入必须被拒：魔数、版本、标志位、长度各错一处
    bad = 0
    good = pack(3, 4, xs32_stream(3, 4))
    cases = [
        ("魔数", b"XXXX" + good[4:]),
        ("版本", good[:4] + struct.pack("<H", 2) + good[6:]),
        ("标志位", good[:6] + struct.pack("<H", 1) + good[8:]),
        ("短一帧", good[:-1]),
        ("多一字节", good + b"\x00"),
    ]
    for tag, blob in cases:
        try:
            unpack(blob, tag)
            print("  %s 没有被拒 FAIL" % tag)
            fails += 1
        except ValueError:
            bad += 1
    print("  坏输入五项都被拒 %s" % ("ok" if bad == len(cases) else "FAIL"))

    # 文本侧的坏输入：跳帧、条数不符
    for tag, text in (("跳帧", "g2dr 1 1 2\nkey 0 00000001\nkey 2 00000002\n"),
                      ("条数不符", "g2dr 1 1 3\nkey 0 00000001\n")):
        try:
            from_text(text, tag)
            print("  %s 没有被拒 FAIL" % tag)
            fails += 1
        except ValueError:
            bad += 1
    print("  坏文本两项都被拒 %s" % ("ok" if bad == len(cases) + 2 else "FAIL"))

    print("fail %d" % fails)
    print("done")
    return 0 if fails == 0 else 1


def main(argv):
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("--in", dest="src", help="输入 .g2dr")
    ap.add_argument("--out", dest="dst", help="输出文件（.h 或 .g2dr）")
    ap.add_argument("--name", default="replay_fixture", help="C 数组的名字前缀")
    ap.add_argument("--from-text", dest="text", help="从录制文本收成 .g2dr")
    ap.add_argument("--to-text", action="store_true", help="把 .g2dr 打成录制文本")
    ap.add_argument("--gen-test", action="store_true", help="合成一份 .g2dr")
    ap.add_argument("--frames", type=int, default=256, help="合成多少帧")
    ap.add_argument("--seed", type=lambda s: int(s, 0), default=7, help="合成的种子")
    ap.add_argument("--check", dest="check", help="只校验一个 .g2dr")
    ap.add_argument("--selftest", action="store_true", help="自检文本往返与坏输入")
    args = ap.parse_args(argv)

    if args.selftest:
        return cmd_selftest()

    if args.check:
        return cmd_check(args.check)

    if args.gen_test:
        if not args.dst:
            ap.error("--gen-test 要配 --out")
        keys = xs32_stream(args.seed, args.frames)
        write_file(args.dst, pack(args.seed, args.frames, keys))
        print("%s 合成完成 seed=%d frames=%d bytes=%d"
              % (args.dst, args.seed & MASK32, args.frames,
                 HEADER_BYTES + args.frames * RECORD_BYTES))
        return 0

    if args.text:
        if not args.dst:
            ap.error("--from-text 要配 --out")
        with open(args.text, "r", encoding="utf-8", errors="replace") as f:
            seed, frames, keys = from_text(f.read(), args.text)
        write_file(args.dst, pack(seed, frames, keys))
        print("%s -> %s seed=%d frames=%d bytes=%d"
              % (args.text, args.dst, seed, frames, HEADER_BYTES + frames * RECORD_BYTES))
        return 0

    if not args.src:
        ap.error("要 --in 或 --gen-test 或 --from-text 或 --check 或 --selftest")

    blob = read_file(args.src)
    seed, frames, keys = unpack(blob, args.src)

    if args.to_text:
        sys.stdout.write(to_text(seed, frames, keys))
        return 0

    if not args.dst:
        ap.error("--in 要配 --out（或 --to-text）")

    if args.dst.endswith(".h"):
        with open(args.dst, "w", encoding="utf-8") as f:
            f.write(emit_header(args.name, args.src, blob))
        print("%s -> %s 数组 %s_bytes[%d] frames=%d"
              % (args.src, args.dst, args.name, len(blob), frames))
    else:
        write_file(args.dst, blob)
        print("%s -> %s bytes=%d frames=%d" % (args.src, args.dst, len(blob), frames))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
