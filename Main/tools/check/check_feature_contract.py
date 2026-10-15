#!/usr/bin/env python3
"""编排入口：按构型的禁用契约与依赖守卫反例（自建源树，跑完删）。

contract_all.sh 验七代表档的 CAPS 位、CTRL 不支持位、保留寄存器读写与无实例哨兵；
guard_negative.sh 验六条非法组合被构建入口拒绝、研究构型被接受。
"""
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
FEATURES = ROOT / 'Main/sim/engine/features'


def main():
    failed = []
    with tempfile.TemporaryDirectory(prefix='gfx-feature-contract-') as work:
        build = subprocess.run(['bash', str(FEATURES / 'build_presets.sh'), work],
                               cwd=ROOT, capture_output=True, text=True,
                               encoding='utf-8', errors='replace')
        if build.returncode:
            sys.stdout.write(build.stdout)
            print('feature-contract FAIL：构型源树没建起来', file=sys.stderr)
            return 1
        for script in ('guard_negative.sh', 'contract_all.sh'):
            proc = subprocess.run(['bash', str(FEATURES / script), work],
                                  cwd=ROOT, capture_output=True, text=True,
                                  encoding='utf-8', errors='replace')
            sys.stdout.write(f'--- {script} ---\n{proc.stdout}')
            if proc.returncode:
                failed.append(script)
    if failed:
        print('feature-contract FAIL：' + ', '.join(failed), file=sys.stderr)
        return 1
    print('FEATURE CONTRACT PASS：七档禁用契约与守卫反例都符合预期')
    return 0


if __name__ == '__main__':
    sys.exit(main())
