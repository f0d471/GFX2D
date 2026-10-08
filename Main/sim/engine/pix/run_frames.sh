#!/usr/bin/env bash
# 导出 SW3 十一个整帧金标准并核对 framebuffer CRC，供 B6 仿真重用。
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build/frames
gcc -O2 -std=c11 -Wall -Wextra -Werror -I../../../sw/lib \
    -o build/pix_cases_dump ../../../tools/check/pix_cases_dump.c \
    ../../../sw/lib/gfx/list.c ../../../sw/lib/gfx/strip.c ../../../sw/lib/gfx/check.c \
    ../../../sw/lib/gfx/exec_sw.c ../../../sw/lib/gfx/cases.c \
    ../../../sw/lib/core/format.c
for i in {0..10}; do
    build/pix_cases_dump --frame "$i" "build/frames/frame$i"
done
python3 - <<'PY'
from pathlib import Path
import zlib

base = Path('build/frames')
golden = dict(line.split() for line in Path('../../../sw/lib/test/golden.txt').read_text().splitlines())
for i in range(11):
    prefix = base / f'frame{i}'
    meta = dict(line.split('=', 1) for line in Path(f'{prefix}.meta.txt').read_text().splitlines())
    raw = bytearray()
    for word in Path(f'{prefix}.exp.hex').read_text().splitlines():
        raw.extend(bytes.fromhex(word)[::-1])
    crc = 0
    for y in range(480):
        offset = 0x1000 + y * 4096
        crc = zlib.crc32(raw[offset:offset+640*4], crc)
    actual = f'{crc:08x}'
    expected = golden[meta['name']]
    print(f'CRC {i:02d} {meta["name"]}: {actual} {"PASS" if actual == expected else "FAIL"}')
    if actual != expected:
        raise SystemExit(1)
print('FRAME EXPORT OK: 11/11 CRCs')
PY
