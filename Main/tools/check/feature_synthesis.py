#!/usr/bin/env python3
"""按构型跑整机四步综合并收集同阶段资源与全时钟时序。

对七代表档各跑一次 Efinity 四步（map/interface/pnr/pgm）到该构型自己的源树，
登记 XLR/RAM/DSP 与全时钟 setup/hold，写入汇总 JSON。

用法（Windows，先设置 EFINITY_HOME 与 PYTHONHOME）：
    python feature_synthesis.py --tree-root <构型源树根> --out <汇总 JSON>
    python feature_synthesis.py --tree-root <构型源树根> --out <汇总 JSON> --only B5
"""
import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path

STEPS = ('map', 'interface', 'pnr', 'pgm')
PRESETS = ('B0', 'B1', 'B2', 'B3', 'B4', 'B5', 'B6')


def timing_slacks(timing: str):
    begin = timing.index('Setup (Max) Clock Relationship')
    section = timing[begin:timing.index('NOTE: Values', begin)]
    hold = section.index('Hold (Min) Clock Relationship')
    rows = []
    for kind, body in (('setup', section[:hold]), ('hold', section[hold:])):
        for source, destination, value in re.findall(
                r'^\s*(\S+)\s+(\S+)\s+[-\d.]+\s+([-\d.]+)\s+\(', body, re.M):
            rows.append({'clock': source, 'to_clock': destination,
                         'kind': kind, 'slack_ns': float(value)})
    if not rows:
        raise RuntimeError('timing report has no clock relationships')
    return rows


def run_flow(tree: Path, env) -> str:
    project = tree / 'fpga' / 'gfx2d_ti60'
    xml = project / 'gfx2d_ti60.xml'
    if not xml.is_file():
        raise RuntimeError('缺少工程 XML: ' + str(xml))
    install = Path(os.environ['EFINITY_HOME'])
    proc = subprocess.run([str(install / 'bin' / 'efx_run.bat'), xml.name],
                          cwd=project, env=env, capture_output=True, text=True,
                          encoding='utf-8', errors='replace')
    body = proc.stdout + proc.stderr
    (tree / 'flow.log').write_text(body, encoding='utf-8')
    failed = [s for s in STEPS if not re.search(r'\b' + s + r'\s*:\s*PASS', body)]
    if proc.returncode or failed:
        raise RuntimeError(f'四步未全过，缺: {failed} (returncode={proc.returncode})')
    return body


def collect(tree: Path) -> dict:
    outflow = tree / 'fpga' / 'gfx2d_ti60' / 'outflow'
    place = (outflow / 'gfx2d_ti60.place.rpt').read_text(encoding='utf-8', errors='replace')
    timing = (outflow / 'gfx2d_ti60.timing.rpt').read_text(encoding='utf-8', errors='replace')
    result = {key: int(re.search(pattern, place).group(1)) for key, pattern in
              (('xlr', r'XLRs:\s*(\d+)'), ('ram', r'Memory Blocks:\s*(\d+)'),
               ('dsp', r'DSP Blocks:\s*(\d+)'))}
    result['slacks'] = timing_slacks(timing)
    # 自写 RTL 的位宽截断告警必须为 0（0.4 节第 5 项）。
    warn = (outflow / 'gfx2d_ti60.warn.log').read_text(encoding='utf-8', errors='replace')
    start = warn.rfind('// Efinity Synthesis Started')
    if start >= 0:
        warn = warn[start:]
    own = [s for s in warn.splitlines()
           if re.search(r'VERI-(1209|1330)', s) and '/rtl/engine/' in s.replace('\\', '/')]
    result['own_width_warnings'] = len(own)
    return result


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--tree-root', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--only', nargs='+', choices=PRESETS)
    args = ap.parse_args()
    if os.name != 'nt':
        print('INCOMPLETE feature_synthesis 使用 Windows Efinity 命令行', file=sys.stderr)
        return 3
    root = Path(args.tree_root).resolve()
    env = dict(os.environ)
    # 让厂商 bat 自行完整初始化，避免只传安装位置导致缺 DLL/PATH。
    for key in ('EFINITY_HOME', 'PYTHONHOME', 'EFXPT_HOME', 'EFXPGM_HOME', 'EFXDBG_HOME',
                'EFXIPM_HOME', 'EFXSVF_HOME', 'EFXSERDESDBG_HOME', 'EFINITY_USER_DIR_INI'):
        env.pop(key, None)
    out_path = Path(args.out).resolve()
    summary = json.loads(out_path.read_text(encoding='utf-8')) if out_path.is_file() else {}
    failures = []
    for preset in (args.only or PRESETS):
        tree = root / preset
        record = json.loads((tree / 'config.json').read_text(encoding='utf-8'))
        print(f'[{preset}] 四步开始 CAPS={record["caps"]} sources={len(record["sources"])}',
              flush=True)
        try:
            run_flow(tree, env)
            result = collect(tree)
        except Exception as error:  # 超器件上限等失败按档记下并继续，便于一次跑全表
            failures.append({'preset': preset, 'error': str(error)})
            print(f'[{preset}] FAIL {error}', flush=True)
            continue
        result.update({'config': record['config'], 'caps': record['caps'],
                       'sources': len(record['sources'])})
        summary[preset] = result
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text(json.dumps(summary, ensure_ascii=False, indent=2) + '\n',
                            encoding='utf-8')
        print(f'[{preset}] PASS xlr={result["xlr"]} ram={result["ram"]} dsp={result["dsp"]} '
              f'own_warn={result["own_width_warnings"]}', flush=True)
    if failures:
        summary['_failures'] = failures
        out_path.write_text(json.dumps(summary, ensure_ascii=False, indent=2) + '\n',
                            encoding='utf-8')
        print('FAILED: ' + ', '.join(f['preset'] for f in failures), flush=True)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
