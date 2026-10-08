#!/usr/bin/env python3
# 引擎寄存器表的单一真源（B5 定案 D51）。
#
# 偏移、位域、复位值与读写属性只在本文件的 REGS 表里写一遍，一条命令生成两份产物：
#   Main/rtl/engine/engine_regs_offsets.vh   Verilog `localparam`，RTL `include
#   Main/sw/bsp/include/gfx_regs.h           C `#define`，驱动（B6）include
# `--check` 重新生成并与仓库内两份产物逐行比，不一致即退出码 1（挂提交前检查，
# 与 localize_paths.py --check 同一位置）。语义（每一位干什么）的权威是
# Main/doc/赛题二总体设计.md 3.4 节与 Engineering/TechReports/spec/02-命令语义.md；
# 本文件只锁「数字与位号」，改语义先改那两处再回来同步表。
#
# 用法（在 Main/ 下）：
#   python3 tools/check/regs_gen.py                 生成两份产物
#   python3 tools/check/regs_gen.py --check         只校验，不写
import argparse
import sys
from pathlib import Path

# ---- 真源表 -----------------------------------------------------------------
# (名字, 偏移, 属性, 位域列表)。属性：RO/RW/WO；位域 = (字段, 高位, 低位, 复位值)，
# 位域只进 .h 的宏名；.vh 另给每个寄存器的偏移与整字位宽。
ID      = 0x000
CAPS    = 0x004
CTRL    = 0x008
STATUS  = 0x00C
IRQ_EN  = 0x010
IRQ_ST  = 0x014
ERR_IDX = 0x018
ERR_TAG = 0x01C
LBASE   = 0x020
LCNT    = 0x024
DOORBELL= 0x028
LSTRIPS = 0x02C
STRIP_H = 0x030
BAND_SURF = 0x034
SURF0   = 0x040            # SURF[s] 字 w：SURF0 + 12*s + 4*w，s=0..15，w=0..2
PERF0   = 0x100            # PERF[i]：PERF0 + 4*i，i 见 PERF_NAMES

PERF_NAMES = ["CMD", "PIXEL", "KEY_SKIP", "RD_BURST", "WR_BURST",
              "RD_BEAT", "WR_BEAT", "WAIT_DDR", "BUSY"]

# ID = ASCII "GFX" (0x474658) 左移 8 位 | 版本号 1
ID_VALUE   = 0x47465801
ID_VERSION = 0x01

# CAPS 位（spec 第 02 章 §1 与 3.8 节：v1 支持色键、预乘/直通 ALPHA、MUL；
# v1-TEX profile（引擎 P3 的 G1，2026-10-07 定案 D57/D63）把 TEX 置 1）
# (名字, 最低位, 位宽, 复位值)。[11:8] = 每拍像素通路数，v1 为 4。
CAPS_FIELDS = [("KEY", 0, 1, 1), ("ALPHA_PM", 1, 1, 1),
               ("ALPHA_ST", 2, 1, 1), ("MUL", 3, 1, 1),
               ("TINT", 4, 1, 0), ("BAND", 5, 1, 1),
               ("TEX", 6, 1, 1), ("INDEX8", 7, 1, 0),
               ("LANES", 8, 4, 4)]
CAPS_VALUE = sum(value << lo for _, lo, _, value in CAPS_FIELDS)

# TEX 装载窗口（v1-TEX profile，G1 的 D57）：APB Slave0 的第 4~7 页，
# 页内偏移 0x4000 起 16 KiB，CPU 地址 0xF810_4000~0xF810_7FFF。
TEX_WINDOW = 0x4000
TEX_BYTES = 16384

# CTRL 位
CTRL_BITS = [("EN", 0), ("SOFT_RST", 1), ("CNT_CLR", 2), ("BAND", 3)]   # 后两位置 1 自清，读恒 0

# STATUS 位
STATUS_BITS = [("BUSY", 0), ("DRAINING", 1), ("DONE", 2), ("ERR", 3)]
STATUS_CODE_HI, STATUS_CODE_LO = 7, 4                     # [7:4] 错误码

# 中断位（IRQ_EN 读写；IRQ_STAT 写 1 清）
IRQ_BITS = [("LIST_DONE", 0), ("ERROR", 1)]

# 错误码（spec 第 02 章 §5 的 0~9 + D19 的 10~12）
ERR_CODES = [("OK", 0), ("BAD_OPCODE", 1), ("BAD_FIELD", 2), ("BAD_EQUATION", 3),
             ("BAD_SURFACE", 4), ("BAD_SPACE", 5), ("BAD_FORMAT", 6), ("OVERLAP", 7),
             ("BAD_PREMULT", 8), ("FETCH", 9), ("BUS_READ", 10), ("BUS_WRITE", 11),
             ("TIMEOUT", 12)]

SURF_SLOTS = 16
PERF_COUNT = len(PERF_NAMES)

# ---- 生成 -------------------------------------------------------------------
def gen_vh():
    L = []
    a = L.append
    a("// 由 tools/check/regs_gen.py 生成（D51 的单一真源），不要手改；")
    a("// 语义见 Main/doc/赛题二总体设计.md 3.4 节与 spec 第 02 章。")
    a("`ifndef GFX_ENGINE_REGS_OFFSETS_VH")
    a("`define GFX_ENGINE_REGS_OFFSETS_VH")
    a("")
    a("// 寄存器偏移（APB 第 0 页内 12 位地址）")
    for n, o in [("REG_ID", ID), ("REG_CAPS", CAPS), ("REG_CTRL", CTRL),
                 ("REG_STATUS", STATUS), ("REG_IRQ_EN", IRQ_EN), ("REG_IRQ_STAT", IRQ_ST),
                 ("REG_ERROR_INDEX", ERR_IDX), ("REG_ERROR_TAG", ERR_TAG),
                 ("REG_LIST_BASE", LBASE), ("REG_LIST_COUNT", LCNT),
                 ("REG_DOORBELL", DOORBELL), ("REG_LSTRIPS", LSTRIPS),
                 ("REG_STRIP_H", STRIP_H), ("REG_BAND_SURF", BAND_SURF), ("REG_SURF0", SURF0), ("REG_PERF0", PERF0)]:
        a(f"localparam [11:0] EC_{n} = 12'h{o:03X};")
    a("")
    a("// 表面表与性能计数器的下标")
    a(f"localparam [4:0]  EC_SURF_SLOTS = 5'd{SURF_SLOTS};")
    a(f"localparam [3:0]  EC_PERF_COUNT = 4'd{PERF_COUNT};")
    a("")
    a("// 性能计数器下标（0x100 + 4*i）")
    for i, n in enumerate(PERF_NAMES):
        a(f"localparam [3:0] EC_PERF_{n} = 4'd{i};")
    a("")
    a("// ID 与版本")
    a(f"localparam [31:0] EC_ID_VALUE = 32'h{ID_VALUE:08X};")
    a(f"localparam [7:0]  EC_ID_VERSION = 8'h{ID_VERSION:02X};")
    a("")
    a("// CAPS 位位号")
    for n, lo, width, value in CAPS_FIELDS:
        a(f"localparam EC_CAPS_{n}_LO = {lo};")
        a(f"localparam EC_CAPS_{n}_W  = {width};")
        a(f"localparam EC_CAPS_{n}_VALUE = {value};")
    a(f"localparam [31:0] EC_CAPS_VALUE = 32'h{CAPS_VALUE:08X};")
    a("")
    a("// CTRL 位位号（SOFT_RST/CNT_CLR 写 1 自清）")
    for n, b in CTRL_BITS:
        a(f"localparam EC_CTRL_{n}_BIT = {b};")
    a("")
    a("// STATUS 位位号与错误码字段")
    for n, b in STATUS_BITS:
        a(f"localparam EC_STATUS_{n}_BIT = {b};")
    a(f"localparam EC_STATUS_CODE_HI = {STATUS_CODE_HI};")
    a(f"localparam EC_STATUS_CODE_LO = {STATUS_CODE_LO};")
    a("")
    a("// 中断位位号（IRQ_STAT 写 1 清）")
    for n, b in IRQ_BITS:
        a(f"localparam EC_IRQ_{n}_BIT = {b};")
    a("")
    a("// 错误码值（错误码字段 [STATUS_CODE_HI:LO] 的取值）")
    for n, v in ERR_CODES:
        a(f"localparam [3:0] EC_ERR_{n} = 4'd{v};")
    a("")
    a("// TEX 装载窗口（v1-TEX profile，G1 的 D57）：APB Slave0 页内偏移，")
    a("// 不是第 0 页寄存器表的 12 位偏移；CPU 地址 = 0xF810_0000 + EC_TEX_WINDOW。")
    a(f"localparam [15:0] EC_TEX_WINDOW = 16'h{TEX_WINDOW:04X};")
    a(f"localparam [31:0] EC_TEX_BYTES  = 32'd{TEX_BYTES};")
    a("")
    a("// 超时门限 T 的复位值（D30；本表不设 T 寄存器，B6 若要可写再扩 CTRL）")
    a("localparam [12:0] EC_TMO_RESET = 13'd4096;")
    a("")
    a("`endif")
    return "\n".join(L) + "\n"

def gen_h():
    L = []
    a = L.append
    a("/* 由 tools/check/regs_gen.py 生成（D51 的单一真源），不要手改；")
    a(" * 语义见 Main/doc/赛题二总体设计.md 3.4 节与 spec 第 02 章。")
    a(" * 引擎寄存器页：APB 第 0 页，基址 0xF810_0000（soc.h 的 IO_APB_SLAVE_0_INPUT）。 */")
    a("#ifndef GFX_REGS_H")
    a("#define GFX_REGS_H")
    a("")
    a("/* 寄存器偏移（相对页基址的字节偏移） */")
    for n, o in [("ID", ID), ("CAPS", CAPS), ("CTRL", CTRL), ("STATUS", STATUS),
                 ("IRQ_EN", IRQ_EN), ("IRQ_STAT", IRQ_ST), ("ERROR_INDEX", ERR_IDX),
                 ("ERROR_TAG", ERR_TAG), ("LIST_BASE", LBASE), ("LIST_COUNT", LCNT),
                 ("DOORBELL", DOORBELL), ("LSTRIPS", LSTRIPS),
                 ("STRIP_H", STRIP_H), ("BAND_SURF", BAND_SURF)]:
        a(f"#define GFX_{n}          0x{o:03X}")
    a(f"#define GFX_SURF(s, w)   (0x{SURF0:03X} + ((s) * 12) + ((w) * 4))  /* s=0..15, w=0..2 */")
    a(f"#define GFX_PERF(i)      (0x{PERF0:03X} + (i) * 4)")
    a("")
    a("/* 性能计数器下标 */")
    for i, n in enumerate(PERF_NAMES):
        a(f"#define GFX_PERF_{n}     {i}")
    a(f"#define GFX_PERF_COUNT   {PERF_COUNT}")
    a("")
    a("/* ID 与版本 */")
    a(f"#define GFX_ID_VALUE     0x{ID_VALUE:08X}u  /* \"GFX\" << 8 | 版本 */")
    a(f"#define GFX_ID_VERSION   0x{ID_VERSION:02X}u")
    a("")
    a("/* CAPS 位掩码 */")
    for n, lo, width, _ in CAPS_FIELDS:
        mask = ((1 << width) - 1) << lo
        a(f"#define GFX_CAPS_{n}_M   0x{mask:08X}u")
        a(f"#define GFX_CAPS_{n}_S   {lo}")
    a(f"#define GFX_CAPS_VALUE   0x{CAPS_VALUE:08X}u")
    a("")
    a("/* CTRL 位（SOFT_RST/CNT_CLR 写 1 自清，读恒 0） */")
    for n, b in CTRL_BITS:
        a(f"#define GFX_CTRL_{n}     (1u << {b})")
    a("")
    a("/* STATUS 位与错误码字段 */")
    for n, b in STATUS_BITS:
        a(f"#define GFX_STATUS_{n}   (1u << {b})")
    a(f"#define GFX_STATUS_CODE_M 0x000000F0u  /* [7:4] */")
    a(f"#define GFX_STATUS_CODE_S 4")
    a("")
    a("/* 中断位（IRQ_STAT 写 1 清） */")
    for n, b in IRQ_BITS:
        a(f"#define GFX_IRQ_{n}      (1u << {b})")
    a("")
    a("/* 错误码值 */")
    for n, v in ERR_CODES:
        a(f"#define GFX_ERR_{n}      0x{v:X}u")
    a("")
    a("/* TEX 装载窗口（v1-TEX profile，G1 的 D57）：APB Slave0 页内偏移，")
    a(" * 不是第 0 页寄存器表的偏移；CPU 地址 = 0xF810_0000 + GFX_TEX_WINDOW。 */")
    a(f"#define GFX_TEX_WINDOW   0x{TEX_WINDOW:04X}u")
    a(f"#define GFX_TEX_BYTES    {TEX_BYTES}u")
    a("")
    a("/* 超时门限 T 的复位值（D30） */")
    a("#define GFX_TMO_RESET    4096u")
    a("")
    a("#endif")
    return "\n".join(L) + "\n"

VH_PATH = Path("rtl/engine/engine_regs_offsets.vh")
H_PATH = Path("sw/bsp/include/gfx_regs.h")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="只校验产物与真源一致")
    args = ap.parse_args()

    vh, h = gen_vh(), gen_h()
    if args.check:
        bad = []
        for p, txt in [(VH_PATH, vh), (H_PATH, h)]:
            cur = p.read_text(encoding="utf-8") if p.exists() else None
            if cur != txt:
                bad.append(str(p))
        if bad:
            print("regs_gen --check: 产物与真源不一致：", ", ".join(bad))
            print("在 Main/ 下跑 python3 tools/check/regs_gen.py 重新生成。")
            return 1
        print("regs_gen --check: 通过。")
        return 0

    VH_PATH.write_text(vh, encoding="utf-8", newline="\n")
    H_PATH.write_text(h, encoding="utf-8", newline="\n")
    print(f"生成 {VH_PATH} 与 {H_PATH}。")
    return 0

if __name__ == "__main__":
    sys.exit(main())
