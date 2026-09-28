#!/usr/bin/env python3
"""把比特流、程序与素材包合成一个 Flash 镜像（sw 第 04 章 3.10 节）。

    python3 Main/tools/board/make_flash_image.py --app <程序.bin>
    python3 Main/tools/board/make_flash_image.py --app <程序.bin> --full
    python3 Main/tools/board/make_flash_image.py --app <程序.bin> --max-asset 2097152
    python3 Main/tools/board/make_flash_image.py --bit <hex> --app <bin> --asset <pak> --out <hex>

分区（设计文档 2.2 节，四个起点都是 4 KiB 扇区边界）：

    0x000000  2 MiB    比特流    输入是 Efinity 出来的 .hex（**不是 .bit**），
                                  解码后按字节写在 0x0，含它自己的 ASCII 头
    0x200000  1.5 MiB  预留      不计入素材容量，本工具填 0xFF
    0x380000  512 KiB  程序      bootloader 从这里拷到 DDR 0x1000
    0x400000  4 MiB    素材包    SW5 的 assetpak 包

两条容易做错的地方：

1. **比特流要的是 .hex，不是 .bit。** 同一编译的两份输出解码后差 20 字节，
   差异全在 ASCII 头里（.bit 写 `Mode: passive`、.hex 写 `Mode: active`），
   写 Flash 必须用 active 那一份。
2. **输出范围按「分区起点 + 输入实际字节数」算，不按最后一个非 0xFF 字节算。**
   有效输入本身可以以 0xFF 结尾，按内容截断会把它的尾巴丢掉。

本工具只生成与校验镜像，**不烧录**。写 Flash 见 Main/doc/开发环境.md 第 8 节，
写之前先做整片备份。
"""

import argparse
import hashlib
import os
import re
import sys

SECTOR = 4096

# (名字, 起点, 容量)。起点必须扇区对齐，容量是分区上限。
PARTITIONS = [
    ("bitstream", 0x000000, 2 * 1024 * 1024),
    ("reserved",  0x200000, 3 * 512 * 1024),
    ("app",       0x380000, 512 * 1024),
    ("assets",    0x400000, 4 * 1024 * 1024),
]
FLASH_BYTES = 8 * 1024 * 1024
ASSET_PARTITION_MAX = 4 * 1024 * 1024


class ImageError(Exception):
    pass


def need(cond, msg):
    if not cond:
        raise ImageError(msg)


def read_hex_text(path):
    """Efinity 的 .hex：每字节一行、两位十六进制。读成字节。"""
    need(os.path.isfile(path), "找不到比特流 hex：%s" % path)
    with open(path, "rb") as f:
        lines = f.read().splitlines()
    need(lines, "%s 是空文件" % path)
    need(all(re.fullmatch(rb"[0-9A-Fa-f]{2}", line) for line in lines),
         "%s 里有不是两位十六进制的行" % path)
    return bytes(int(line, 16) for line in lines)


def read_bin(path):
    need(os.path.isfile(path), "找不到文件：%s" % path)
    with open(path, "rb") as f:
        return f.read()


def write_hex_text(path, data):
    """Programmer 接受的格式：每字节一行、两位大写十六进制、LF 换行。"""
    with open(path, "w", encoding="ascii", newline="\n") as f:
        f.write("".join("%02X\n" % b for b in data))


def crc32_of(data):
    import zlib
    return zlib.crc32(data) & 0xFFFFFFFF


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    main_dir = os.path.dirname(os.path.dirname(here))       # Main/

    ap = argparse.ArgumentParser(description="gfx2d Flash 镜像合成（SW5）")
    ap.add_argument("--bit", default=os.path.join(main_dir, "fpga", "gfx2d_ti60",
                                                  "outflow", "gfx2d_ti60.hex"),
                    help="比特流 hex（Efinity 的 .hex，不是 .bit）")
    ap.add_argument("--app", required=True, help="程序 bin（必填，不能输出空程序区）")
    ap.add_argument("--asset", default=os.path.join(main_dir, "assets", "build",
                                                    "assets.pak"),
                    help="素材包")
    ap.add_argument("--out", default=None, help="输出 hex，默认 <素材包同目录>/flash.hex")
    ap.add_argument("--full", action="store_true", help="输出整片 8 MiB，默认截到最后一个分区的末端")
    ap.add_argument("--max-asset", type=int, default=ASSET_PARTITION_MAX,
                    help="素材运行预算（字节），只能收紧，不能超过 4 MiB 分区")
    args = ap.parse_args()

    out_path = os.path.abspath(args.out or
                               os.path.join(os.path.dirname(os.path.abspath(args.asset)), "flash.hex"))

    try:
        need(out_path not in {os.path.abspath(args.bit), os.path.abspath(args.app),
                             os.path.abspath(args.asset)},
             "输出路径不能与任一输入文件相同")
        bit = read_hex_text(args.bit)
        need(any(line.strip() == b"Mode: active" for line in bit[:512].splitlines()),
             "比特流不是 active 模式的 .hex；passive .bit 不能写 Flash")
        asset = read_bin(args.asset)
        app = read_bin(args.app)
        need(app, "程序 bin 是空文件")
        need(asset, "素材包是空文件")

        need(args.max_asset <= ASSET_PARTITION_MAX,
             "--max-asset 只能收紧运行预算，Flash 素材分区上限是 %d 字节" % ASSET_PARTITION_MAX)

        # --- 分区与容量 ---
        for name, start, cap in PARTITIONS:
            need(start % SECTOR == 0,
                 "%s 的起点 0x%06X 不在 %d 字节扇区边界上" % (name, start, SECTOR))
        for i in range(len(PARTITIONS) - 1):
            need(PARTITIONS[i][1] + PARTITIONS[i][2] == PARTITIONS[i + 1][1],
                 "分区 %s 与 %s 之间不连续" % (PARTITIONS[i][0], PARTITIONS[i + 1][0]))
        need(PARTITIONS[-1][1] + PARTITIONS[-1][2] == FLASH_BYTES, "分区没有铺满整片 Flash")

        need(len(bit) <= 2 * 1024 * 1024,
             "比特流 %d 字节超过 2 MiB 分区" % len(bit))
        need(len(app) <= 512 * 1024,
             "程序 %d 字节超过 512 KiB 分区" % len(app))
        need(len(asset) <= args.max_asset,
             "素材包 %d 字节超过运行预算 %d 字节（分区上限 %d）"
             % (len(asset), args.max_asset, ASSET_PARTITION_MAX))
        need(len(asset) <= ASSET_PARTITION_MAX,
             "素材包 %d 字节超过 4 MiB 分区" % len(asset))

        # --- 合成 ---
        placed = [("bitstream", 0x000000, bit), ("app", 0x380000, app),
                  ("assets", 0x400000, asset)]
        end = max(start + len(data) for _, start, data in placed if data) if any(
            data for _, _, data in placed) else 0
        # 按「分区起点 + 输入实际字节数」向上取整到扇区；**不**按最后一个非 0xFF 字节截断：
        # 有效输入本身可以以 0xFF 结尾，按内容截断会把那个字节丢掉。
        total = FLASH_BYTES if args.full else (end + SECTOR - 1) // SECTOR * SECTOR
        need(total > 0, "没有任何输入，不生成镜像")
        need(total <= FLASH_BYTES, "输出 %d 字节超过整片 Flash" % total)

        img = bytearray(b"\xFF" * total)        # 空洞与预留区都是 0xFF
        for _, start, data in placed:
            img[start:start + len(data)] = data

        write_hex_text(out_path, bytes(img))

        # --- 回环校验：解码回来与三份输入逐字节比 ---
        back = read_hex_text(out_path)
        need(len(back) == total,
             "回读长度 %d 与写出长度 %d 不一致" % (len(back), total))
        for name, start, data in placed:
            got = back[start:start + len(data)]
            need(got == data, "%s 分区回读不一致（起点 0x%06X，%d 字节）" % (name, start, len(data)))
        # 分区之外（含预留区）必须是 0xFF
        marks = [(start, start + len(data)) for _, start, data in placed if data]
        for i in range(total):
            covered = any(a <= i < b for a, b in marks)
            if not covered:
                need(back[i] == 0xFF, "偏移 0x%06X 在分区之外却不是 0xFF" % i)
    except ImageError as exc:
        print("ERROR: %s" % exc, file=sys.stderr)
        return 1

    def sha16(data):
        return hashlib.sha256(data).hexdigest()[:16]

    print("make_flash_image: 输出 %s" % os.path.relpath(out_path, main_dir))
    print("  镜像 %d 字节 = %.3f MiB（%s）"
          % (total, total / 1048576.0,
             "整片" if args.full else "截到最后一个分区末端并向上取整到 %d 字节扇区" % SECTOR))
    for name, start, data in placed:
        if not data:
            continue
        print("  %-10s 0x%06X..0x%06X  %8d 字节  crc32=%08X  sha256-16=%s"
              % (name, start, start + len(data) - 1, len(data),
                 crc32_of(data), sha16(data)))
    print("  预留 0x200000..0x37FFFF 填 0xFF；回环校验：三个分区逐字节一致，区外全 0xFF")
    print("  本工具不烧录。写 Flash 前先按 Main/doc/开发环境.md 第 8 节整片备份。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
