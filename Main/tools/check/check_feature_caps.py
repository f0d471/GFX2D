#!/usr/bin/env python3
"""判据：按构型生成的三份产物里的 CAPS 必须与特性清单一致。

三者必须同源：
  1. feature_config.caps(config)           —— 特性清单算出的期望值
  2. regs_gen.gen_vh(caps) 里的 EC_CAPS_VALUE —— RTL 消费的硬件常量
  3. gfx_feature.h 里的 GFX_FEATURE_CAPS   —— 软件侧的编译时预期能力

任一不一致都说明构建入口某处走了独立的算法，软件会按错的能力位去选后端。

用法：
    python check_feature_caps.py [--out <结果 JSON>]
"""
import argparse
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import feature_build as fb
import feature_config as fc
import regs_gen


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out')
    args = ap.parse_args()
    rows, failures = [], []
    configs = fc.legal_configurations()
    seen = set()
    for config in configs:
        name = fc.name_of(config)
        if name in seen:
            continue
        seen.add(name)
        want = fc.caps(config)
        vh = regs_gen.gen_vh(want)
        m = re.search(r"localparam \[31:0\] EC_CAPS_VALUE = 32'h([0-9A-F]{8});", vh)
        h = fb.software_header(name, config)
        m2 = re.search(r'#define GFX_FEATURE_CAPS\s+0x([0-9A-F]{8})u', h)
        got_vh = int(m.group(1), 16) if m else None
        got_h = int(m2.group(1), 16) if m2 else None
        row = {'config': name, 'expected': f'0x{want:08X}',
               'regs_vh': None if got_vh is None else f'0x{got_vh:08X}',
               'feature_h': None if got_h is None else f'0x{got_h:08X}'}
        rows.append(row)
        if got_vh != want or got_h != want:
            failures.append(row)
        print(f"{name:16s} expect=0x{want:08X} vh={row['regs_vh']} h={row['feature_h']} "
              + ('OK' if got_vh == want and got_h == want else 'MISMATCH'))
    if args.out:
        Path(args.out).resolve().write_text(
            json.dumps({'variants': rows, 'failures': failures}, ensure_ascii=False, indent=2) + '\n',
            encoding='utf-8')
    if failures:
        print('FEATURE CAPS MISMATCH: ' + json.dumps(failures, ensure_ascii=False))
        return 1
    print(f'FEATURE CAPS CONSISTENT （{len(rows)} 个构型）')
    return 0


if __name__ == '__main__':
    sys.exit(main())
