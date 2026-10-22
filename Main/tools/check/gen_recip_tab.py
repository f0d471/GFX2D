#!/usr/bin/env python3
"""生成 Q1.30 倒数种子，不使用浮点运算。默认只核对。"""
import argparse
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--write', action='store_true')
    args = parser.parse_args()
    path = Path(__file__).resolve().parents[2] / 'sw/lib/core/recip_tab.inc'
    rows = ['/* gen_recip_tab.py: floor(2^41 / (2048 + 2*i + 1)), Q1.30。 */']
    values = [(1 << 41) // (2048 + 2 * i + 1) for i in range(1024)]
    rows += ['    ' + ' '.join(f'{v}u,' for v in values[i:i + 8]) for i in range(0, 1024, 8)]
    content = '\n'.join(rows) + '\n'
    if args.write:
        path.write_text(content, encoding='utf-8')
    else:
        assert path.read_text(encoding='utf-8') == content, 'recip table differs'
    print('RECIP TABLE PASS 1024 entries')


if __name__ == '__main__':
    main()
