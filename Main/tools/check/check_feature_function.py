#!/usr/bin/env python3
"""合法构型的真实引擎绘制、关闭态错误与定向变异。"""
import argparse
import re
import shutil
from pathlib import Path

import feature_build as fb
import feature_config as fc
import regs_gen
from check_run import ROOT, command, session, write_json

MAIN = ROOT / 'Main'


def configure(tree, config):
    engine = tree / 'engine'
    fb.write_feature_files(config, fc.name_of(config), engine)
    (engine / 'engine_regs_offsets.vh').write_text(regs_gen.gen_vh(fc.caps(config)), encoding='utf-8')


def compile_and_run(tree, config, tag, build, logs, expect_failure=False):
    configure(tree, config)
    excluded = {Path(p).name for flag, paths in fc.FEATURE_SOURCES.items()
                if not config[flag] for p in paths}
    sources = [p for p in sorted((tree / 'engine').glob('**/*.v')) if p.name not in excluded]
    sources += [tree / 'vendor/verilog-axi' / name for name in ('axi_dma_rd.v', 'axi_dma_wr.v')]
    sources += [MAIN / 'sim/axi_probe/axi_ram_model.v']
    binary = build / (tag + '.vvp')
    body = command(['iverilog', '-g2012', '-Wportbind', '-s', 'tb_feature_function',
                    '-I', tree / 'engine', '-I', MAIN / 'sim/axi_probe', '-o', binary,
                    MAIN / 'sim/engine/features/tb_feature_function.v', *sources],
                   logs / (tag + '.compile.log'))
    floating = re.findall(r'^.*dangling input port.*$', body, re.M)
    if floating:
        if expect_failure:
            return {'detected': 'dangling input port', 'count': len(floating)}
        raise RuntimeError(tag + ': ' + '\n'.join(floating))
    body = command(['vvp', '-n', binary], logs / (tag + '.log'), timeout=120)
    passed = 'FEATURE-FUNCTION-PASS' in body and 'FAIL' not in body
    if expect_failure:
        if passed or 'FEATURE-FUNCTION-FAIL' not in body:
            raise RuntimeError(tag + ': 变异没有被判据检出')
        return {'detected': 'functional mismatch'}
    if not passed:
        raise RuntimeError(tag + ': ' + body[-3500:])
    return {'status': 'PASS', 'checks': int(re.search(r'PASS checks=(\d+)', body)[1])}


def mutations(tree, build, logs):
    top = tree / 'engine/engine_top.v'
    regs = tree / 'engine/core/engine_regs.v'
    cmd = tree / 'engine/core/engine_cmd.v'
    original = {p: p.read_text(encoding='utf-8') for p in (top, regs, cmd)}
    jobs = [
        ('tex-floating', 'B0', top,
         original[top].replace(".tx_busy(1'b0)", '.tx_busy()')),
        ('ctrl-side-effect', 'B0', regs,
         original[regs].replace(' && !ctrl_unsupported;', ';')),
        ('band-gate', 'B6', regs,
         original[regs].replace("wire ctrl_band_bad = pwdata[EC_CTRL_BAND_BIT];",
                                "wire ctrl_band_bad = 1'b0;")),
        ('key-ignored', '11111-110', cmd,
         original[cmd].replace(' || disabled_key_bad', '')),
        ('soft-reset-clip', 'B0', top,
         original[top].replace(".preserve_clip(1'b0)", ".preserve_clip(1'b1)")),
        ('backend-transition', 'B5', top,
         original[top].replace('.band_mode(band_mode && !bin_mode)',
                               '.band_mode(band_mode && !pb_active)')),
    ]
    rows = []
    for name, preset, path, changed in jobs:
        if changed == original[path]:
            raise RuntimeError('变异没有改变源码: ' + name)
        try:
            path.write_text(changed, encoding='utf-8')
            result = compile_and_run(tree, fc.parse_name(preset), 'mutation-' + name,
                                     build, logs, expect_failure=True)
            rows.append({'mutation': name, **result})
            print('FEATURE MUTATION detected ' + name, flush=True)
        finally:
            path.write_text(original[path], encoding='utf-8')
    return rows


def main():
    current = session()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--only', nargs='+', help='诊断构型；正式范围默认全部 220 个合法构型')
    parser.add_argument('--no-mutations', action='store_true')
    args = parser.parse_args()
    build = current.build('feature-function')
    logs = current.logs('feature-function')
    tree = build / 'rtl'
    if not tree.exists():
        shutil.copytree(MAIN / 'rtl', tree)
    configs = [fc.parse_name(name) for name in args.only] if args.only else fc.legal_configurations()
    for malformed in ('00000-111-x', '00000-111-', '00000-111-d-extra', '00000-112'):
        try:
            fc.parse_name(malformed)
        except ValueError:
            pass
        else:
            raise RuntimeError('错误配置名被接受: ' + malformed)
    rows = []
    for config in configs:
        reasons = fc.check(config)
        if reasons:
            raise RuntimeError('非法构型: ' + '; '.join(reasons))
        name = fc.name_of(config)
        result = compile_and_run(tree, config, name, build, logs)
        rows.append({'config': name, **result})
        write_json(logs / 'result.json', {'variants': rows, 'complete': False})
        print('FEATURE FUNCTION PASS ' + name, flush=True)
    mutation_rows = [] if args.no_mutations else mutations(tree, build, logs)
    current.verify()
    write_json(logs / 'result.json', {'variants': rows, 'mutations': mutation_rows,
                                    'complete': True, 'profile': current.profile,
                                    'scope': 'directed configuration functions; not all mechanism suites'})
    print('FEATURE FUNCTION PASS configurations=' + str(len(rows))
          + ' mutations=' + str(len(mutation_rows)), flush=True)


if __name__ == '__main__':
    main()
