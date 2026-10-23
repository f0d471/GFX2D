#!/usr/bin/env python3
"""本轮自建七代表档源树，完成四步、全时钟时序与网表结构检查。"""
import argparse
import os
import re
from pathlib import Path
import shutil
import sys
import xml.etree.ElementTree as ET

import feature_build as fb
import feature_config as fc
import feature_synthesis as synthesis
import feature_netlist
from check_run import IncompleteCheck, ROOT, session, write_json


def compiler_inputs(tree):
    """只比较编译输入；不把工作目录、报告与 GUI 流程状态当成源码。"""
    project = tree / 'fpga/gfx2d_ti60'
    files = [* (tree / 'rtl').rglob('*.v'), * (tree / 'rtl').rglob('*.vh'),
             * (project / 'sdc').glob('*.sdc'), project / 'gfx2d_ti60.xml',
             project / 'gfx2d_ti60.peri.xml']
    files += [p for p in (project / 'ip').rglob('*') if p.is_file()
              and not any(part.startswith(('work_', 'build', '__pycache__')) or part == 'outflow'
                          for part in p.relative_to(project / 'ip').parts)
              and p.suffix not in ('.log', '.pyc')]
    return {p.relative_to(tree).as_posix(): p for p in files}


def comparable(path):
    data = path.read_bytes()
    if path.name == 'gfx2d_ti60.xml':
        root = ET.fromstring(data)
        for key in ('last_change', 'last_run_state', 'last_run_flow', 'config_result_in_sync',
                    'design_ood', 'place_ood', 'route_ood'):
            root.attrib.pop(key, None)
        for node in root.iter():
            if node.text is not None and not node.text.strip(): node.text = None
            if node.tail is not None and not node.tail.strip(): node.tail = None
        return ET.tostring(root)
    return data


def matching_inputs(previous, expected):
    files = compiler_inputs(expected)
    for name, path in files.items():
        old = previous / name
        if not old.is_file() or comparable(old) != comparable(path):
            return False, name
    return True, sorted(files)


def completed(tree):
    flow = (tree / 'flow.log').read_text(encoding='utf-8', errors='replace')
    if not all(re.search(r'\b' + step + r'\s*:\s*PASS', flow) for step in synthesis.STEPS):
        raise RuntimeError('缺少本次四步通过输出')
    result = synthesis.collect(tree)
    bit = tree / 'fpga/gfx2d_ti60/outflow/gfx2d_ti60.bit'
    sources = [p for p in compiler_inputs(tree).values() if p.suffix in ('.v', '.vh', '.sdc')]
    if not bit.exists() or bit.stat().st_mtime_ns < max(p.stat().st_mtime_ns for p in sources):
        raise RuntimeError('位流早于构型源码')
    return result


def copy_tree(source, target):
    allowed = (ROOT / 'Main/sim/build').resolve()
    if not source.resolve().is_relative_to(allowed) or not target.resolve().is_relative_to(allowed):
        raise ValueError('复用源树与目标必须位于 sim/build 内')
    # 不穿过重解析点；复制保留原时间戳，不制造源码早于位流的假象。
    for folder, directories, files in os.walk(source):
        directories[:] = [d for d in directories if not d.startswith(('work_', '__pycache__'))]
        for name in directories + files:
            entry = Path(folder) / name
            if entry.is_symlink() or getattr(entry.lstat(), 'st_file_attributes', 0) & 0x400:
                raise ValueError('源树含重解析点: ' + str(entry))
    shutil.copytree(source, target,
                    ignore=shutil.ignore_patterns('work_*', '__pycache__'))


def main():
    current = session()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reuse-from', help='经输入逐项比较后复用既有构型产物')
    parser.add_argument('--reuse-b5', help='B5 的独立构建树；仍须与当前生成输入一致')
    parser.add_argument('--only',nargs='+',choices=tuple(fc.PRESETS),help='按工作点选择实际受影响构型')
    args = parser.parse_args()
    if os.name != 'nt':
        raise IncompleteCheck('构型综合使用 Windows Efinity Python')
    trees = current.build('feature-presets')
    logs = current.logs('feature-synthesis')
    summary, structures, reuse = {}, {}, []
    env = dict(os.environ)
    for key in ('EFINITY_HOME', 'PYTHONHOME', 'EFXPT_HOME', 'EFXPGM_HOME', 'EFXDBG_HOME',
                'EFXIPM_HOME', 'EFXSVF_HOME', 'EFXSERDESDBG_HOME', 'EFINITY_USER_DIR_INI'):
        env.pop(key, None)
    for preset in args.only or tuple(fc.PRESETS):
        expected = current.build('feature-expected') / preset
        record = fb.build(preset, expected)
        tree = trees / preset
        candidate = Path(args.reuse_b5).resolve() if preset == 'B5' and args.reuse_b5 else \
                    Path(args.reuse_from).resolve() / preset if args.reuse_from else None
        result = None
        if candidate is not None:
            same, detail = matching_inputs(candidate, expected)
            if same:
                result = completed(candidate)
                source_bit = candidate / 'fpga/gfx2d_ti60/outflow/gfx2d_ti60.bit'
                source_key = current.artifact(source_bit)
                copy_tree(candidate, tree)
                target_key = current.artifact(tree / 'fpga/gfx2d_ti60/outflow/gfx2d_ti60.bit')
                artifacts = current.read()['artifacts']
                if artifacts[source_key]['sha256'] != artifacts[target_key]['sha256']:
                    raise RuntimeError('复制位流身份不符: ' + preset)
                copied, _ = matching_inputs(tree, expected)
                if not copied: raise RuntimeError('复制后的源码不一致: ' + preset)
                reuse.append({'preset': preset, 'source_tree': candidate.relative_to(ROOT).as_posix(),
                              'verified_inputs': detail, 'source_bit': source_key, 'target_bit': target_key})
                print('FEATURE SYNTHESIS REUSE ' + preset + ' (verified compiler inputs)', flush=True)
            else:
                print('FEATURE SYNTHESIS REBUILD ' + preset + ' changed=' + detail, flush=True)
        if result is None:
            copy_tree(expected, tree)
            synthesis.run_flow(tree, env)
            result = completed(tree)
        result.update({'config': record['config'], 'caps': record['caps'],
                       'synthesis_mode': record['synthesis_mode'], 'sources': len(record['sources'])})
        summary[preset] = result
        shutil.copyfile(expected / 'config.json', tree / 'config.json')
        present = feature_netlist.probe(feature_netlist.netlist(tree))
        wanted = {name: bool(record['flags'][name]) for name in feature_netlist.PATTERNS}
        if present != wanted: raise RuntimeError('网表结构不符: ' + preset)
        structures[preset] = {'present': present, 'expected': wanted}
        write_json(logs / 'synthesis.json', summary)
        write_json(logs / 'netlist.json', {'variants': structures, 'failures': []})
        write_json(logs / 'reuse.json', reuse)
        destination = logs / preset
        destination.mkdir(exist_ok=True)
        shutil.copyfile(tree / 'config.json', destination / 'config.json')
        shutil.copyfile(tree / 'flow.log', destination / 'flow.log')
        outflow = tree / 'fpga/gfx2d_ti60/outflow'
        for suffix in ('place.rpt', 'timing.rpt', 'warn.log'):
            shutil.copyfile(outflow / ('gfx2d_ti60.' + suffix), destination / suffix)
        current.artifact(outflow / 'gfx2d_ti60.bit')
        print('FEATURE SYNTHESIS PASS ' + preset + ' xlr=' + str(result['xlr']), flush=True)
    current.verify()
    print('FEATURE SYNTHESIS PASS presets='+str(len(summary))+'; netlist and all clock relationships', flush=True)


if __name__ == '__main__':
    try:
        main()
    except IncompleteCheck as error:
        print('INCOMPLETE ' + str(error), file=sys.stderr)
        sys.exit(3)
