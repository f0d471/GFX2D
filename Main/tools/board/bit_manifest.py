#!/usr/bin/env python3
"""从 feature 构建树生成交付快照的 manifest.json。

用法（WSL 或任意 python3，仓库根运行）：
  python3 Main/tools/board/bit_manifest.py --preset B7 \
      --tree Main/sim/build/check-runs/<run>/build/feature-presets/B7 \
      --run ppa-after [--note "..."]

读取树内 config.json（构型/CAPS/综合模式，由 check_feature_synthesis 写入）与
outflow 报告（资源、时序），登记源 commit 与位流摘要，写 manifest.json 到
Main/bit/snapshots/<preset>/。字段语义见 Main/bit/snapshots/README.md。
"""
import argparse
import hashlib
import json
import re
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'Main' / 'tools' / 'check'))
import feature_config as fc  # noqa: E402


def git(*args):
    return subprocess.run(['git', *args], cwd=ROOT, capture_output=True,
                          text=True, check=True).stdout.strip()


def read_place(path):
    body = path.read_text(encoding='utf-8', errors='replace')
    found = {k: int(re.search(p, body).group(1)) for k, p in
             [('xlr', r'XLRs:\s*(\d+)'), ('ram', r'Memory Blocks:\s*(\d+)'),
              ('dsp', r'DSP Blocks:\s*(\d+)')]}
    return found


def read_timing(path, clock):
    body = path.read_text(encoding='utf-8', errors='replace')
    begin = body.index('Setup (Max) Clock Relationship')
    section = body[begin:body.index('NOTE: Values', begin)]
    hold = section.index('Hold (Min) Clock Relationship')
    out = {}
    for kind, part in [('setup', section[:hold]), ('hold', section[hold:])]:
        rows = re.findall(r'^\s*(\S+)\s+(\S+)\s+[-\d.]+\s+([-\d.]+)\s+\(', part, re.M)
        for source, destination, value in rows:
            if source == destination == clock:
                out[kind + '_' + clock + '_ns'] = float(value)
    if not out:
        raise RuntimeError('timing 报告缺 ' + clock + ' 关系')
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--preset', required=True)
    parser.add_argument('--tree', required=True, type=Path)
    parser.add_argument('--run', required=True, help='验证运行号（check-runs 目录名）')
    parser.add_argument('--clock', default='core_clk')
    parser.add_argument('--note', default='')
    args = parser.parse_args()

    tree = args.tree.resolve()
    outflow = tree / 'fpga' / 'gfx2d_ti60' / 'outflow'
    bit = outflow / 'gfx2d_ti60.bit'
    config = json.loads((tree / 'config.json').read_text(encoding='utf-8'))
    for key in ('config', 'caps', 'synthesis_mode'):
        if key not in config:
            raise RuntimeError('config.json 缺 ' + key)
    if fc.normalize(config['config']) != fc.normalize(args.preset):
        raise RuntimeError('构型不符: ' + config['config'] + ' vs ' + args.preset)

    resources = read_place(outflow / 'gfx2d_ti60.place.rpt')
    timing = read_timing(outflow / 'gfx2d_ti60.timing.rpt', args.clock)
    payload = bit.read_bytes()
    manifest = {
        'config': args.preset,
        'caps': config['caps'],
        'synthesis_mode': config['synthesis_mode'],
        'source_commit': git('rev-parse', 'HEAD'),
        'source_dirty': git('status', '--porcelain', '--', 'Main/rtl') != '',
        'validated_by': ['check-runs/' + args.run],
        **resources,
        **timing,
        'bit_sha256': hashlib.sha256(payload).hexdigest(),
        'bit_bytes': len(payload),
        'date': datetime.now(timezone.utc).astimezone().isoformat(timespec='seconds'),
    }
    if args.note:
        manifest['note'] = args.note
    target = ROOT / 'Main' / 'bit' / 'snapshots' / args.preset
    target.mkdir(parents=True, exist_ok=True)
    previous=target/'manifest.json'
    if previous.is_file():
        old=json.loads(previous.read_text(encoding='utf-8'))
        # The hardware generator never silently deletes the separately bound
        # firmware identity. A changed bitstream invalidates its board evidence.
        for key in ('bin','bin_sha256','bin_bytes','content','firmware','verification','assets'):
            if key in old:manifest[key]=old[key]
        if old.get('bit_sha256')==manifest['bit_sha256'] and old.get('caps')==manifest['caps']:
            if 'bin_verified_by' in old:manifest['bin_verified_by']=old['bin_verified_by']
        elif 'bin' in old:
            manifest['bin_verified_by']='INCOMPLETE: bitstream changed; selected firmware requires fresh board verification'
    (target / 'gfx2d_ti60.bit').write_bytes(payload)
    (target / 'manifest.json').write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print('BIT MANIFEST ' + args.preset + ' xlr=' + str(resources['xlr']) +
          ' ram=' + str(resources['ram']) + ' dsp=' + str(resources['dsp']) +
          ' -> ' + str(target.relative_to(ROOT)))
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except RuntimeError as error:
        print('FAIL ' + str(error), file=sys.stderr)
        sys.exit(1)
