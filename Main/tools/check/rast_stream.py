#!/usr/bin/env python3
"""R2 流式向量导出器：按 R1 同一种子与随机数调用顺序重建光栅记录流，
产出 RTL 单元仿真直接消费的二进制包（主表/子表/逐条带折叠期望）。

与 check_rast.py raster_checks 的关系：随机流逐位同源（self-check 对照
R1 归档 raster-vectors.json 的前 16 条记录 hex）；期望不照抄 R1 的逐条
隔离初值，而是按共享 BAND/z 状态的提交顺序折叠（RTL 回放就是共享状态），
纹理放 64×64 atlas 左上 2×2，CLAMP 界为整 atlas。

输出（--out 前缀，三件）：
  <prefix>.main.bin   N×128B 主表（L2 原样；L1 用清掉 TEX_EN 的副本）
  <prefix>.frame.bin  头部+子表+逐条带期望（见 header_pack 文档）
打印 RECORD-DIGEST 行（记录流 CRC），与 R1 的 3716d055 口径不同（这里是
记录字节本身，不含像素结果），只用于跨轮确认记录流未漂移。
"""
import argparse
import json
import struct
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import rast_model as model

SEED = 0x72312026
TEX = [0xff000000, 0xff80ff40, 0xff4080ff, 0xffffffff]
KEY = TEX[0]
BG = 0x12345678
STRIP_H = 16
STRIPS = 30
WIDTH = 640
ATLAS_SLOG2 = 6            # v1 atlas 64×64，行距 64 texel 词
ATLAS_WORDS = 1 << (ATLAS_SLOG2 + ATLAS_SLOG2)
MAGIC = 0x52325354         # "R2ST"


def atlas_texture():
    """R1 的 2×2 纹理摆进 atlas 左上角：word = v*64+u。"""
    t = [0] * ATLAS_WORDS
    t[0] = TEX[0]; t[1] = TEX[1]
    t[64] = TEX[2]; t[65] = TEX[3]
    return t


def texel_at(atlas, u, v):
    return atlas[(v << ATLAS_SLOG2) + u]


def fold_pixel(dec, x, y, color, z, atlas):
    """与 engine_rast RTL 同语义的单像素求值；返回 None 表示不写。"""
    flags, edges, bbox, origin, gradients, starts = dec
    for a, b, c in edges:
        e = a * x + b * y + c
        if e < 0 or e == 0 and not (a > 0 or a == 0 and b > 0):
            return None
    values = [model.rnd(start + model.rnd(gx * (16 * x - origin[0]) + gy * (16 * y - origin[1]), 16))
              for (gx, gy), start in zip(gradients, starts)]
    zq = min(65535, max(0, values[0]))
    if zq >= z:
        return None
    texel = 0xffffff
    if flags & 1:
        u = min(63, max(0, values[1]))
        v = min(63, max(0, values[2]))
        texel = texel_at(atlas, u, v)
        if (flags & 4) and texel == KEY:
            return None
    out = 0
    for k, shift in enumerate((16, 8, 0)):
        c = min(255, max(0, values[k + 3])) if flags & 2 else 255
        if flags & 1:
            c = model.rnd(((texel >> shift) & 255) * c, 255)
        out |= c << shift
    return out, zq


def generate_records(count):
    """逐位复刻 check_rast.raster_checks 的随机流（种子与调用顺序不变）。"""
    rng = model.XorShift(SEED)
    records = []
    for i in range(count):
        x, y = rng.next() % 634, rng.next() % 474
        w, h = 1 + rng.next() % 5, 1 + rng.next() % 5
        sub = rng.next() & 15
        vs = [(x * 16 - sub, y * 16, rng.next() & 65535, rng.next() & 63, rng.next() & 63, rng.next()),
              ((x + w) * 16, y * 16 + sub, rng.next() & 65535, rng.next() & 63, rng.next() & 63, rng.next()),
              (x * 16, (y + h) * 16, rng.next() & 65535, rng.next() & 63, rng.next() & 63, rng.next())]
        records_buf = model.setup(vs, rng.next() & 7)
        if not records_buf:
            vs = [(x * 16, y * 16, 100, 0, 0, 0xff804020),
                  ((x + 4) * 16, y * 16, 120, 16, 0, 0xffff8040),
                  (x * 16, (y + 4) * 16, 110, 0, 16, 0xff4080ff)]
            records_buf = model.setup(vs, i & 7)
        r = records_buf[0]
        if i % 4 == 0:
            raw = bytearray(r)
            for off in model.GRADS:
                struct.pack_into('<ii', raw, off, int(rng.next() % (1 << 29)) - (1 << 28),
                                 int(rng.next() % (1 << 29)) - (1 << 28))
            r = bytes(raw)
        # R1 原循环在 bbox 逐像素的双重循环里每点固定消耗两次随机数
        # （隔离测试的初始 z/颜色）；这里不使用其值，但必须按相同次数消耗，
        # 保证后续记录与 R1 流逐位同源。
        dec0 = model.unpack(r)
        bx0, bx1, by0, by1 = dec0[2]
        for _ in range(by0, by1 + 1):
            for _ in range(bx0, bx1 + 1):
                _ = (65535, 0, dec0[5][0] // 65536, rng.next() & 65535)[i % 4]
                _ = rng.next()
        records.append(r)
    return records


def check_against_r1(records):
    """前 16 条记录与 R1 归档 raster-vectors.json 的 fixtures 逐字节比对。"""
    fixture = Path('Engineering/TechReports/engine/evidence/16-r1-20261022/logs/rast/raster-vectors.json')
    if not fixture.exists():
        return 'R1-FIXTURE-MISSING'
    vectors = json.loads(fixture.read_text(encoding='utf-8'))['vectors']
    for case, rec in enumerate(r['record'] for r in vectors):
        if bytes.fromhex(rec) != records[case]:
            return f'R1-FIXTURE-DIFFERS at case {case}'
    return 'R1-FIXTURE-MATCH'


def build_frame(records, l1, index_offset=0):
    """主表/子表/逐条带折叠期望。l1=True 时 flags 清 TEX_EN 并按无纹理折叠。

    index_offset：本帧第 0 条记录在整条 R1 流里的全局序号；子表索引用帧内
    序号（u16 域 ≤65535），RTL 侧把 TRI_BASE 加上 offset×128 即可寻址。"""
    atlas = atlas_texture()
    strips = [[] for _ in range(STRIPS)]
    for idx, r in enumerate(records):
        if l1:
            r = r[:0] + struct.pack('<I', struct.unpack_from('<I', r)[0] & ~1) + r[4:]
        y0 = struct.unpack_from('<h', r, 76)[0]
        y1 = struct.unpack_from('<h', r, 78)[0]
        for s in range(max(0, y0 // STRIP_H), min(STRIPS - 1, y1 // STRIP_H) + 1):
            strips[s].append(idx)
    sub = bytearray()
    sub_offsets = []
    for s in range(STRIPS):
        sub_offsets.append(len(sub))
        sub += struct.pack('<HH', len(strips[s]), 0)
        for idx in strips[s]:
            sub += struct.pack('<H', idx)
    states = [[(BG, 65535)] * (WIDTH * STRIP_H) for _ in range(STRIPS)]
    commits = 0
    for idx, r0 in enumerate(records):
        r = r0 if not l1 else r0[:0] + struct.pack('<I', struct.unpack_from('<I', r0)[0] & ~1) + r0[4:]
        dec = model.unpack(r)
        x0, x1, y0, y1 = dec[2]
        for s in range(max(0, y0 // STRIP_H), min(STRIPS - 1, y1 // STRIP_H) + 1):
            sy = s * STRIP_H
            state = states[s]
            for py in range(max(y0, sy), min(y1, sy + STRIP_H - 1) + 1):
                row = (py - sy) * WIDTH
                for px in range(x0, x1 + 1):
                    cur = state[row + px]
                    got = fold_pixel(dec, px, py, cur[0], cur[1], atlas)
                    if got is not None:
                        state[row + px] = got
                        commits += 1
    return bytes(sub), sub_offsets, states, commits


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--count', type=int, default=100000)
    parser.add_argument('--out', required=True, help='输出前缀（三件：<p>.main.bin <p>.frame.bin）')
    parser.add_argument('--skip-r1-check', action='store_true')
    args = parser.parse_args()

    records = generate_records(args.count)
    if not args.skip_r1_check and args.count >= 16:
        print('SELF-CHECK', check_against_r1(records))
    digest = zlib.crc32(b''.join(records)) & 0xffffffff
    print(f'RECORD-DIGEST crc32={digest:08x} count={args.count}')

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    main_blob = b''.join(records)
    out.with_suffix('.main.bin').write_bytes(main_blob)
    l1_blob = b''.join(
        r[:0] + struct.pack('<I', struct.unpack_from('<I', r)[0] & ~1) + r[4:] for r in records)
    out.with_name(out.stem + '_l1.main.bin').write_bytes(l1_blob)

    # 子表索引是 u16（ABI 上限 65535 条/帧），十万条拆两帧回放；
    # 帧首清 z，期望按帧从清零态折叠，RTL 消费总数仍为 count。
    # frame.bin：帧头(8 字) + 每帧 [offset, sub_bytes, 30×段偏移] + 子表；
    # exp.bin：每帧每条带 10240×(u32 color, u32 z)（定宽，供 $fread 直接索引）。
    half = (args.count + 1) // 2
    halves = [(0, records[:half]), (half, records[half:])] if args.count > 65535 else [(0, records)]
    with open(out.with_suffix('.frame.bin'), 'wb') as fa, \
         open(out.with_name(out.stem + '_l1.frame.bin'), 'wb') as fb, \
         open(out.with_name(out.stem + '.exp.bin'), 'w+b') as fe, \
         open(out.with_name(out.stem + '_l1.exp.bin'), 'w+b') as fe1:
        for fx in (fa, fb):
            fx.write(struct.pack('<IIIIIIII', MAGIC, args.count, len(halves), BG, KEY, *TEX[1:]))
        total_commits = 0
        for li, (offset, chunk) in enumerate(halves):
            sub, sub_offsets, states, commits = build_frame(chunk, False, offset)
            sub1, _, states1, commits1 = build_frame(chunk, True, offset)
            total_commits += commits
            for fx, sx in ((fa, states), (fb, states1)):
                fx.write(struct.pack('<II', offset, len(sub)))
                fx.write(struct.pack('<%dI' % STRIPS, *sub_offsets))
                fx.write(sub)
            for s in range(STRIPS):
                for color, z in states[s]:
                    fe.write(struct.pack('<II', color, z))
            for s in range(STRIPS):
                for color, z in states1[s]:
                    fe1.write(struct.pack('<II', color, z))
        print(f'EXPORT PASS records={args.count} frames={len(halves)} commits={total_commits}')


if __name__ == '__main__':
    main()
