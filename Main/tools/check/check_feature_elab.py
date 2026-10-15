#!/usr/bin/env python3
"""编排入口：七代表档配置穷举 elaboration（自建源树，跑完删）。

覆盖 elab_all.sh（七代表档）与 elab_legal.sh 的合法/非法结构组合两层：
自建源树到临时目录，逐档 elaboration，非法结构组合逐个验证被构建入口拒绝。
"""
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
FEATURES = ROOT / 'Main/sim/engine/features'


def run(script, *args):
    return subprocess.run(['bash', str(FEATURES / script), *map(str, args)],
                          cwd=ROOT, capture_output=True, text=True,
                          encoding='utf-8', errors='replace')


def main():
    with tempfile.TemporaryDirectory(prefix='gfx-feature-elab-') as work:
        presets = run('elab_all.sh', work)
        sys.stdout.write(presets.stdout)
        legal = run('elab_legal.sh', work)
        sys.stdout.write(legal.stdout)
        if presets.returncode or legal.returncode:
            print('feature-elab FAIL', file=sys.stderr)
            return 1
    print('FEATURE ELAB PASS：七代表档 + 合法/非法结构组合两层都符合预期')
    return 0


if __name__ == '__main__':
    sys.exit(main())
