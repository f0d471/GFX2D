#!/usr/bin/env python3
"""把二进制文件转成 Efinity Programmer 用的 hex 文本。

格式与 Efinity 生成的 .hex 相同：每字节一行，两位大写十六进制，LF 换行。
写完后把 hex 解码回字节，与输入逐字节比较，不一致时以退出码 1 退出。

用法：python3 bin2hex.py <输入.bin> <输出.hex>
"""
import sys


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    src, dst = sys.argv[1], sys.argv[2]
    data = open(src, "rb").read()
    with open(dst, "w", newline="\n") as f:
        f.write("".join("%02X\n" % b for b in data))
    back = bytes(int(tok, 16) for tok in open(dst).read().split())
    if back != data:
        print("ERROR: %s 解码后与 %s 不一致" % (dst, src))
        return 1
    print("%s -> %s: %d 字节，回环校验一致" % (src, dst, len(data)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
