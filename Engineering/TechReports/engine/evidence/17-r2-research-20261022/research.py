#!/usr/bin/env python3
"""R2 前期研究算例；不执行或证明光栅 RTL。默认从仓库根运行。"""
import argparse
import json
from pathlib import Path
import subprocess
import struct
import sys
import zlib


def plan(address, length):
    first = address & ~15
    end = (address + length + 15) & ~15
    result = []
    while first < end:
        size = min(1024, 4096 - (first & 4095), end - first)
        assert first % 16 == size % 16 == 0
        assert size <= 1024 and first // 4096 == (first + size - 1) // 4096
        result.append((first, size))
        first += size
    return result


def memory(address, length):
    return bytes((((a * 131) ^ (a >> 7) ^ (a >> 15)) & 255) for a in range(address, address + length))


def transport():
    cases = 0
    for address in range(0, 4096, 2):
        for length in (2, 4, 6, 126, 128, 130, 1022, 1024, 1026, 4094):
            descriptors = plan(address, length)
            raw = b''.join(memory(a, n) for a, n in descriptors)
            prefix = address - descriptors[0][0]
            assert raw[prefix:prefix + length] == memory(address, length)
            cases += 1
    splits = set()
    main_cases = 0
    for base in range(0, 4096, 32):
        for index in range(64):
            descriptors = plan(base + index * 128, 128)
            if len(descriptors) > 1:
                splits.add(tuple(n for _, n in descriptors))
            assert sum(n for _, n in descriptors) == 128
            main_cases += 1
    assert splits == {(32, 96), (64, 64), (96, 32)}
    # count=1 的 6B 段、count=2 的 8B 段后，下个 u32 头位于偏移 14。
    headers = [0, 6, 14]
    assert plan(headers[2], 4) == [(0, 32)]
    assert memory(14, 4) != memory(14 & ~15, 4)
    # 64 个 u16 从偏移 14 开始需 9 拍，不是固定 8 拍。
    assert sum(n for _, n in plan(14, 128)) == 144
    free, requested = 4, 64
    assert not (requested <= free)  # 必须先预留整个索引请求的 FIFO 空间。
    return dict(aligned_envelope_cases=cases, main_record_cases=main_cases,
                page_splits=sorted(splits), header_offsets=headers,
                header_at_14_physical_bytes=32, indices64_at_14_physical_bytes=144,
                fifo_issue_requires_full_credit=True)


def addresses():
    addresses = [y * 640 + x for y in range(16) for x in range(640)]
    assert len(set(addresses)) == 10240
    assert max(addresses).bit_length() == 14
    assert len({a & 2047 for a in addresses}) == 2048
    clear = [4 * word + lane for word in range(2560) for lane in range(4)]
    assert clear == addresses
    for a in addresses:
        bank, word = a & 3, a >> 2
        segment = int(word >= 2048)
        index = word - segment * 2048
        assert bank + 4 * (index + segment * 2048) == a
        assert index < (512 if segment else 2048)
    return dict(pixels=10240, pixel_address_bits=14, old_11bit_distinct=2048,
                banks=4, bank_depth=2560, bank_segments=[2048, 512],
                clear_cycles_per_strip=2560, clear_cycles_per_frame=76800,
                clear_ms_at_100MHz=0.768)


def pipeline(gap):
    depth, color = 65535, 0
    pending = {}
    trace = []
    for cycle in range(gap + 4):
        if cycle in (0, gap):
            z, rgb = (100, 0x112233) if cycle == 0 else (200, 0x445566)
            trace.append(dict(cycle=cycle, action='read', depth=depth, candidate=z))
            if z < depth:
                pending[cycle + 3] = z, rgb
        if cycle in pending:  # READ_FIRST：同沿的读看到写入前数据。
            depth, color = pending[cycle]
            trace.append(dict(cycle=cycle, action='atomic_commit', depth=depth, color=color))
    return dict(gap=gap, final_depth=depth, final_color=color, trace=trace)


def hazards():
    result = [pipeline(gap) for gap in (1, 3, 4)]
    assert [r['final_depth'] for r in result] == [200, 200, 100]
    assert result[2]['final_color'] == 0x112233
    return dict(scope='abstract READ_FIRST pipeline, commit latency=3; not RTL',
                traces=result, conclusion='atomic color/z alone does not remove inter-triangle RAW hazard')


def arithmetic():
    state = 0x52323026
    def random():
        nonlocal state
        state ^= state << 13 & 0xffffffff
        state ^= state >> 17
        state ^= state << 5 & 0xffffffff
        return state
    def multiply(g, delta):
        high, low = g // 65536, g & 65535
        assert -(1 << 14) <= high < (1 << 14)
        assert 0 <= low < (1 << 16)
        return (high * delta) * 65536 + low * delta
    crc, largest = 0, 0
    for i in range(100000):
        gx = random() % 2147483647 - 1073741823
        gy = random() % 2147483647 - 1073741823
        dx, dy = random() % 28656 - 14335, random() % 23536 - 11775
        for g, d in ((gx, dx), (gy, dy)):
            assert multiply(g, d) == g * d
        base = (random() & 65535) * 65536
        acc = base + (multiply(gx, dx) + multiply(gy, dy) + 8) // 16
        assert acc == base + (gx * dx + gy * dy + 8) // 16
        assert abs(acc) < 1 << 41
        largest = max(largest, abs(acc))
        crc = zlib.crc32(struct.pack('<q', acc), crc)
    for g in (-(1 << 30) + 1, -65537, -65536, -1, 0, 1, 65535, 65536, (1 << 30) - 1):
        for delta in (-14335, -11775, -1, 0, 1, 11760, 14320):
            assert multiply(g, delta) == g * delta
    return dict(seed='52323026', cases=100000, split='signed high15 + unsigned low16',
                crc32=f'{crc:08x}', max_abs_acc_observed=largest,
                proposed_init_dsp=4, partial_products_per_triangle=30,
                minimum_issue_cycles=8, scope='integer identity, not DSP mapping/timing')


def atlas_and_caps(root):
    sys.path.insert(0, str(root / 'Main/tools/check'))
    import feature_config as fc
    b3 = fc.caps(fc.parse_name('B3'))
    b7 = b3 | (1 << 14) | (1 << 15)
    assert b3 == 0x146f and b7 == 0xd46f and b7 & (1 << 12)
    mapped = set()
    for v in range(64):
        for u in range(64):
            word = v * 64 + u
            assert 0 <= word < 4096
            assert 4 * (word >> 2) + (word & 3) == word
            mapped.add(word)
    assert len(mapped) == 4096
    return dict(B3=f'{b3:08x}', proposed_B7=f'{b7:08x}', physical_texel_words=4096,
                proposed_atlas=[64, 64], tile_origins=[[0, 0], [32, 0], [0, 32], [32, 32]])


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out', type=Path, required=True)
    args = ap.parse_args()
    root = Path.cwd()
    result = dict(scope='R2 preliminary research, not RTL acceptance',
                  revision=subprocess.check_output(['git', '-c', 'safe.directory=' + root.as_posix(),
                       'rev-parse', 'HEAD'], text=True).strip(),
                  transport=transport(), addresses=addresses(),
                  hazards=hazards(), arithmetic=arithmetic(), atlas_caps=atlas_and_caps(root))
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({k: v for k, v in result.items() if k != 'hazards'}, indent=2))
    print('HAZARD EXPECTED_RED gaps=1,3 final_z=200; drain gap=4 final_z=100')
    print('R2 RESEARCH PASS; no RTL/synthesis/board claim')


if __name__ == '__main__':
    main()
