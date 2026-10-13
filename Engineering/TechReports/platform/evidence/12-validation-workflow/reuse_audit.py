#!/usr/bin/env python3
"""复核本次局部工具修订不改变既有功能检查；不代替仿真或板测。"""
import ast
import hashlib
import json
from pathlib import Path
import sys

EVIDENCE=Path(__file__).resolve().parent
ROOT=EVIDENCE.parents[4]
sys.path.insert(0,str(ROOT/'Main/tools/check'))
from check_run import cached_identities


def main():
    prior=json.loads((EVIDENCE/'host-rtl/run.json').read_text(encoding='utf-8'))
    current=cached_identities(list(prior['inputs']),'inputs')
    changed=[name for name in prior['inputs'] if prior['inputs'][name]['sha256']!=current[name]['sha256']]
    if any(not name.startswith('Main/tools/') for name in changed):
        raise RuntimeError('生产输入已改变，原复用关系不适用于当前源树')
    functions={
        'Main/tools/check/check_f4_instances.py':('f4-prior.py.txt',['models','compile_rtl','rtl_cases','rtl','mutations','protocols','lease']),
        'Main/tools/check/check_f5_bin.py':('f5-prior.py.txt',['run','models','compile_rtl','bin','frame','mutations','protocols','lease'])}
    result={'prior_run':'20261013-005209-3878cef1','production_inputs_unchanged':True,
            'changed_tool_files':changed,'unchanged_function_bodies':{}}
    for name,(label,wanted) in functions.items():
        old=(EVIDENCE/'reuse-inputs'/label).read_bytes()
        if hashlib.sha256(old).hexdigest()!=prior['inputs'][name]['sha256']:
            raise RuntimeError('冻结源码身份不符: '+name)
        def bodies(text):
            return {node.name:ast.dump(node,include_attributes=False) for node in ast.parse(text).body
                    if isinstance(node,ast.FunctionDef)}
        before=bodies(old.decode('utf-8-sig'))
        after=bodies((ROOT/name).read_text(encoding='utf-8-sig'))
        if any(before[fn]!=after[fn] for fn in wanted):raise RuntimeError('功能检查函数已改变: '+name)
        result['unchanged_function_bodies'][name]=wanted
    recorded=json.loads((EVIDENCE/'reuse-analysis.json').read_text(encoding='utf-8'))
    if recorded!=result:raise RuntimeError('当前分析与记录不一致')
    print('REUSE AUDIT PASS: frozen inputs and 15 producer bodies match; production inputs unchanged')


if __name__=='__main__':main()
