#!/usr/bin/env python3
"""netlist 结构检查：断言被关闭的机制在综合网表里不存在。

守卫给不了结构保证（见 spec 2.4 节：对侧声明齐全时守卫恒成功），
真正的保证是这条：厂商 map 的输出网表里，关闭的机制不得留下任何信号。
判据按层次路径匹配，例如 TEX 关闭时 `u_engine/u_tex` 一次都不出现。

读的是各构型自己的 outflow/gfx2d_ti60.map.v；该文件在 map 步之后即存在，
所以 pnr 因超器件上限失败的档（B5）同样可判。

用法：
    python feature_netlist.py --tree-root <构型源树根> --out <结果 JSON>
"""
import argparse
import json
import sys
from pathlib import Path

# 机制 -> 判定该机制存在于网表的层次路径模式
PATTERNS = {
    'TEX': 'u_engine/u_tex',
    'BAND_HW': 'g_band',
    'INST': 'u_engine/u_cmd/u_inst',
    'PB': 'u_engine/u_pb',
    'RAST': 'u_engine/u_r3/u_rast',
}
PRESETS = ('B0', 'B1', 'B2', 'B3', 'B4', 'B5', 'B6', 'B7')


def netlist(tree: Path) -> Path:
    return tree / 'fpga' / 'gfx2d_ti60' / 'outflow' / 'gfx2d_ti60.map.v'


def probe(path: Path) -> dict:
    text = path.read_text(encoding='utf-8', errors='replace')
    return {name: (pattern in text) for name, pattern in PATTERNS.items()}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--tree-root', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--only', nargs='+', choices=PRESETS)
    args = ap.parse_args()
    root = Path(args.tree_root).resolve()
    results, failures = {}, []
    for preset in (args.only or PRESETS):
        tree = root / preset
        path = netlist(tree)
        if not path.is_file():
            failures.append({'preset': preset, 'error': '缺 map 网表，先跑 feature_synthesis'})
            print(f'[{preset}] SKIP 缺 map 网表')
            continue
        flags = json.loads((tree / 'config.json').read_text(encoding='utf-8'))['flags']
        present = probe(path)
        mismatches = [name for name in PATTERNS if present[name] != bool(flags.get(name,False))]
        record = {'present': present, 'expected': {n: bool(flags.get(n,False)) for n in PATTERNS},
                  'source_count': json.loads((tree / 'config.json').read_text(encoding='utf-8'))['sources'].__len__()}
        results[preset] = record
        verdict = 'PASS' if not mismatches else 'FAIL ' + ','.join(mismatches)
        print(f'[{preset}] {verdict} ' +
              ' '.join(f'{n}={int(present[n])}/{int(bool(flags.get(n,False)))}' for n in PATTERNS))
        if mismatches:
            failures.append({'preset': preset, 'mismatches': mismatches})
    out = Path(args.out).resolve()
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps({'variants': results, 'failures': failures},
                              ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    if failures:
        print('NETLIST STRUCTURE FAIL: ' + json.dumps(failures, ensure_ascii=False))
        return 1
    print('NETLIST STRUCTURE PASS')
    return 0


if __name__ == '__main__':
    sys.exit(main())
