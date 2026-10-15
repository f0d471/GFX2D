#!/usr/bin/env python3
"""编排入口：关闭的机制离开编译输入（两种判据）。

probe_cut.sh 数实例化点（TEX/PB/BAND/INST），probe_strip.sh 读源码内容
（STRIP 不增减例化，只能用内容判）。两者都自建源树到临时目录。
"""
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
FEATURES = ROOT / 'Main/sim/engine/features'


def main():
    with tempfile.TemporaryDirectory(prefix='gfx-feature-probe-') as work:
        subprocess.run(['bash', str(FEATURES / 'build_presets.sh'), work],
                       cwd=ROOT, capture_output=True, text=True,
                       encoding='utf-8', errors='replace')
        env_note = f'GFX_FEATURE_ELAB={work}'
        import os
        env = dict(os.environ, GFX_FEATURE_ELAB=work)
        for script in ('probe_cut.sh', 'probe_strip.sh'):
            proc = subprocess.run(['bash', str(FEATURES / script), work],
                                  cwd=ROOT, capture_output=True, text=True,
                                  encoding='utf-8', errors='replace', env=env)
            sys.stdout.write(f'--- {script} ---\n{proc.stdout}')
            if proc.returncode:
                print(f'feature-probe FAIL（{script}，{env_note}）', file=sys.stderr)
                return 1
    print('FEATURE PROBE PASS：七个代表档的关闭机制都不在编译输入里')
    return 0


if __name__ == '__main__':
    sys.exit(main())
