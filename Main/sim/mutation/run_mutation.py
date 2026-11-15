#!/usr/bin/env python3
"""外部变异活动：冻结输入、独立变异、全部基线先行，证据进入公共检查会话。"""
from __future__ import annotations
import argparse
from collections import Counter
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import signal
import subprocess
import sys
import time
import yaml

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'Main/tools/check'))
from check_run import Session, source_files, identity, write_json

MANIFEST = ROOT / 'Main/sim/mutation/mutants.yaml'
# 只识别基础设施失败；检查器的 AssertionError/RuntimeError 本身不代表编译失败。
INFRA = re.compile(r'COMPILE[- ](?:FAIL|ERROR)|SETUP-ERROR|\berror:\s|syntax error|Unable to bind|error\(s\) during elaboration|Unable to open|missing cases|main open|No such file or directory|Segmentation fault', re.I)
WATCHDOG = re.compile(r'WATCHDOG|看门狗|仿真没有自己结束', re.I)
VERILOG_TOKEN = re.compile(
    r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\\\S+|'
    r"(?:\d+)?'[sS]?[bBoOdDhH][0-9a-fA-F_xXzZ?]+|'[01xXzZ]|"
    r'[A-Za-z_$][A-Za-z0-9_$]*|\d+|===|!==|>>>|<<<|==|!=|<=|>=|'
    r'<<|>>|&&|\|\||\+\+|--|\*\*|~\^|\^~|~&|~\||\+:|-:|::|->|=>|\*>|[^\s]')


def verilog_tokens(text):
    """返回代码 token 及字节解码后的字符区间；字符串和多字符运算符保持完整。"""
    return [m for m in VERILOG_TOKEN.finditer(text) if not m.group().startswith(('//', '/*'))]


def token_span(text, find):
    source = verilog_tokens(text)
    wanted = [m.group() for m in verilog_tokens(find)]
    if not wanted: raise ValueError('Verilog 规则没有代码 token')
    values = [m.group() for m in source]
    hits = [i for i in range(len(source)-len(wanted)+1) if values[i:i+len(wanted)] == wanted]
    if len(hits) != 1: raise ValueError('规则必须恰好匹配一次')
    begin = hits[0]
    return source[begin].start(), source[begin+len(wanted)-1].end()


def load_manifest(path=MANIFEST):
    rows = yaml.safe_load(Path(path).read_text(encoding='utf-8'))
    if not isinstance(rows, list) or not rows:
        raise ValueError('变异清单必须是非空列表')
    seen = set()
    for row in rows:
        for key in ('id', 'target', 'rules', 'runs', 'spec', 'defect'):
            if not row.get(key): raise ValueError(f'{row.get("id")}: 缺少 {key}')
        if row['id'] in seen: raise ValueError('重复变异: '+row['id'])
        seen.add(row['id'])
        rel = Path(row['target'])
        if rel.is_absolute() or '..' in rel.parts or not rel.as_posix().startswith('Main/'):
            raise ValueError('变异目标必须位于 Main: '+str(rel))
        for rule in row['rules']:
            if not rule.get('find') or 'replace' not in rule:
                raise ValueError(row['id']+': 规则不完整')
            mode = rule.get('match', 'literal')
            if mode not in ('literal', 'verilog') or (mode == 'verilog' and rel.suffix not in ('.v', '.sv', '.vh')):
                raise ValueError(row['id']+': 不支持的规则匹配方式')
        for run in row['runs']:
            if set(run)-{'cmd','evidence'}:raise ValueError(row['id']+': 发现者只接受 cmd/evidence')
            if not run.get('cmd') or not run.get('evidence'):
                raise ValueError(row['id']+': 每个发现者必须登记命令和具体见红位置')
            re.compile(run['evidence'])
    return rows


def mutated_bytes(original, rules):
    text = original.decode('utf-8')
    # 统一规则行尾，写回仍保留原文件的行尾风格。
    crlf = b'\r\n' in original
    text = text.replace('\r\n', '\n')
    for rule in rules:
        find = rule['find'].replace('\r\n', '\n')
        replace = rule['replace'].replace('\r\n', '\n')
        mode = rule.get('match', 'literal')
        if mode == 'verilog':
            begin, end = token_span(text, find)
            text = text[:begin] + replace + text[end:]
        elif mode == 'literal':
            if text.count(find) != 1: raise ValueError('规则必须恰好匹配一次')
            text = text.replace(find, replace, 1)
        else:
            raise ValueError('不支持的规则匹配方式: '+str(mode))
    result = text.replace('\n', '\r\n').encode() if crlf else text.encode()
    if result == original: raise ValueError('变异没有改变源码')
    return result


@contextmanager
def mutation(shadow, row, pristine):
    # 每条规则只改登记的目标，立即恢复；跨文件的上一条错误不可能留下。
    target = shadow / row['target']
    for name, data in pristine.items():
        if (shadow/name).read_bytes() != data:
            raise RuntimeError('变异前源文件被污染: '+name)
    expected = mutated_bytes(pristine[row['target']], row['rules'])
    target.write_bytes(expected)
    try:
        yield
    finally:
        changed = []
        for name, data in pristine.items():
            path = shadow/name
            if not path.is_file() or path.read_bytes() != (expected if name == row['target'] else data):
                changed.append(name)
        for name in set(changed)|{row['target']}:
            path=shadow/name;path.parent.mkdir(parents=True,exist_ok=True)
            path.write_bytes(pristine[name])
        if changed: raise RuntimeError('变异期间有未登记的源码变化: '+', '.join(changed))


def snapshot(current):
    # 复制 Git 登记的真实输入（含 .bin），排除输出目录；不猜测文件扩展名是否是夹具。
    names = source_files()
    if not names: raise RuntimeError('源码输入清单为空')
    shadow = current.build('mutation')/'shadow'
    shadow.mkdir(parents=True, exist_ok=False)
    inputs = {}
    for name in names:
        src, dest = ROOT/name, shadow/name
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dest)
        inputs[name] = identity(src)
    # 素材源中的 PNG/字体等由 source_files 收录；生成物在隔离树里生成。
    context = {'schema': 1, 'revision': current.read()['revision'], 'files': sorted(inputs)}
    write_json(shadow/'.check-source.json', context)
    write_json(current.logs('mutation')/'snapshot.json', {'revision': context['revision'], 'inputs': inputs})
    return shadow, inputs


def classify(result, evidence):
    if result['timeout']: return 'KILLED-WEAK'
    if INFRA.search(result['text']): return 'INVALID'
    if result['returncode'] == 0: return 'SURVIVED'
    if WATCHDOG.search(result['text']) and not re.search(evidence, result['text']): return 'KILLED-WEAK'
    return 'KILLED' if re.search(evidence, result['text']) else 'KILLED-WEAK'


def aggregate(states):
    if not states or 'INVALID' in states: return 'INVALID'
    if all(s == 'KILLED' for s in states): return 'KILLED'
    if all(s == 'SURVIVED' for s in states): return 'SURVIVED'
    if 'SURVIVED' in states: return 'PARTIAL'
    return 'KILLED-WEAK'


def toolchain_identity():
    result={'pyyaml':yaml.__version__,'environment':{'EFINITY_HOME':os.environ.get('EFINITY_HOME','')}}
    for name, flags in [('python3',['-VV']),('gcc',['--version']),('iverilog',['-V']),('vvp',['-V']),('bash',['--version'])]:
        found=shutil.which(name)
        if not found: raise RuntimeError('缺少变异工具链: '+name)
        path=Path(found).resolve()
        version=subprocess.run([found,*flags],capture_output=True,text=True,timeout=15)
        result[name]={'path':str(path),'sha256':identity(path)['sha256'],
                      'version':(version.stdout+version.stderr).strip()}
    primitive=Path(os.environ.get('EFINITY_HOME',''))/'sim_models/verilog/efx_srl8.v'
    if primitive.is_file():result['vendor_primitive']={'path':str(primitive),'sha256':identity(primitive)['sha256']}
    return result


def execute(shadow, cmd, destination, profile, timeout):
    destination.mkdir(parents=True, exist_ok=False)
    # 公共会话输出与 shell 套件输出均由当前调用独占，禁止按 mtime 搜集旧日志。
    task = 'Main/sim/build/check-runs/task'
    task_path = shadow/task
    if task_path.exists(): shutil.rmtree(task_path)
    tokens = shlex.split(cmd)
    legacy = None
    if tokens[:1] == ['bash'] and len(tokens)>1:
        legacy = shadow/Path(tokens[1]).parent/'build'
        if legacy.exists(): shutil.rmtree(legacy)
    env = dict(os.environ)
    for key in ('GFX_CHECK_RUN_DIR','GFX_CHECK_SHARED'):
        env.pop(key, None)
    env.update(GFX_CHECK_RUN_DIR=task, GFX_CHECK_PROFILE=profile,
               GFX_CHECK_SOURCE_MANIFEST=str(shadow/'.check-source.json'))
    started = time.monotonic()
    output = destination/'stdout.log'
    timed_out = False
    with output.open('w') as handle:
        proc = subprocess.Popen(['bash','-c',cmd],cwd=shadow,env=env,stdout=handle,
                                stderr=subprocess.STDOUT,start_new_session=True)
        try: proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            os.killpg(proc.pid, signal.SIGKILL)
            proc.wait()
    logs = []
    for origin, label in ((task_path/'logs','suite'), (legacy,'legacy')):
        if origin and origin.exists():
            for src in sorted(origin.rglob('*.log')):
                dest = destination/label/src.relative_to(origin)
                dest.parent.mkdir(parents=True,exist_ok=True)
                shutil.copy2(src,dest)
                logs.append(dest)
    if (task_path/'run.json').exists():
        shutil.copy2(task_path/'run.json',destination/'suite-run.json')
        if not json.loads((destination/'suite-run.json').read_text())['inputs']:
            raise RuntimeError('套件源码输入身份为空')
    text = output.read_text(errors='replace')+'\n'+'\n'.join(p.read_text(errors='replace') for p in logs)
    result = {'returncode':proc.returncode,'timeout':timed_out,'seconds':round(time.monotonic()-started,3),
              'cmd':cmd,'text':text}
    write_json(destination/'process.json',{k:v for k,v in result.items() if k!='text'})
    return result


def audit(rows):
    for row in rows:
        mutated_bytes((ROOT/row['target']).read_bytes(),row['rules'])
        doc = ROOT/row['spec']['doc']
        if not doc.is_file() or row['spec']['section'] not in doc.read_text(encoding='utf-8'):
            raise ValueError(row['id']+': 规格出处无效')
    print(f'AUDIT PASS mutants={len(rows)}')


def campaign(current, rows, timeout, keep=False):
    logdir = current.logs('mutation')
    shadow=None
    report = {'schema':2,'profile':current.profile,'revision':current.read()['revision'],
              'baseline':{},'mutants':[],'counts':{},'status':'INCOMPLETE'}
    try:
        audit(rows)
        tools=toolchain_identity()
        write_json(logdir/'toolchain.json',tools)
        shadow, inputs = snapshot(current)
        pristine = {row['target']:(shadow/row['target']).read_bytes() for row in rows}
        # G4 等消费者的素材准备也在干净副本中执行，绝不复制历史 build。
        if any('check_g4_integration' in r['cmd'] for row in rows for r in row['runs']):
            prepared = execute(shadow,'python3 Main/tools/assets/pack_assets.py',logdir/'prepare',current.profile,timeout)
            if prepared['returncode'] or prepared['timeout']: raise RuntimeError('素材生成失败，见 mutation/prepare')
        commands = list(dict.fromkeys(r['cmd'] for row in rows for r in row['runs']))
        for i, cmd in enumerate(commands):
            result = execute(shadow,cmd,logdir/'baseline'/f'{i:02d}',current.profile,timeout)
            valid = result['returncode']==0 and not result['timeout'] and not INFRA.search(result['text'])
            report['baseline'][cmd] = {'status':'PASS' if valid else 'FAIL', 'seconds':result['seconds'],
                                       'log':f'baseline/{i:02d}/stdout.log'}
            print(f'BASELINE {i+1}/{len(commands)} {"PASS" if valid else "FAIL"} {result["seconds"]:.1f}s {cmd}',flush=True)
        if any(x['status']!='PASS' for x in report['baseline'].values()):
            raise RuntimeError('未变异基线失败；本轮不执行任何变异')
        for row in rows:
            entry = {'id':row['id'],'target':row['target'],'rules':row['rules'],'runs':[]}
            with mutation(shadow,row,pristine):
                entry['source_sha256']=hashlib.sha256((shadow/row['target']).read_bytes()).hexdigest()
                for n, run in enumerate(row['runs']):
                    result = execute(shadow,run['cmd'],logdir/'mutants'/row['id']/str(n),current.profile,timeout)
                    status = classify(result,run['evidence'])
                    hit = re.search(run['evidence'],result['text'])
                    entry['runs'].append({'cmd':run['cmd'],'status':status,'seconds':result['seconds'],
                        'returncode':result['returncode'],'evidence':hit.group(0) if hit else '',
                        'expected_evidence':run['evidence'],'log':f'mutants/{row["id"]}/{n}/stdout.log'})
            entry['status']=aggregate([r['status'] for r in entry['runs']])
            report['mutants'].append(entry)
            print(f'MUTANT {len(report["mutants"])}/{len(rows)} {row["id"]} {entry["status"]}',flush=True)
            write_json(logdir/'mutation-report.json',report)
        report['counts']=dict(Counter(r['status'] for r in report['mutants']))
        report['status']='PASS' if len(report['mutants'])==len(rows) and set(report['counts'])=={'KILLED'} else 'FAIL'
        if toolchain_identity()!=tools:raise RuntimeError('工具链在本轮运行期间改变')
        if report['status']!='PASS': raise RuntimeError('变异验收未通过: '+str(report['counts']))
        return {'log':(logdir/'mutation-report.json').relative_to(ROOT).as_posix(),'counts':report['counts']}
    except BaseException as exc:
        report['status']='FAIL'
        report['error']=str(exc)
        raise
    finally:
        report['counts']=dict(Counter(r['status'] for r in report['mutants']))
        write_json(logdir/'mutation-report.json',report)
        write_json(logdir/'artifacts.json',{p.relative_to(logdir).as_posix():identity(p) for p in logdir.rglob('*') if p.is_file()})
        try:
            current.verify(True)
        finally:
            if shadow is not None and not keep: shutil.rmtree(shadow)


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    mode=ap.add_mutually_exclusive_group(required=True)
    mode.add_argument('--list',action='store_true');mode.add_argument('--audit',action='store_true');mode.add_argument('--run',action='store_true')
    ap.add_argument('--ids');ap.add_argument('--profile',choices=['quick','acceptance','release'],default=os.environ.get('GFX_CHECK_PROFILE','acceptance'))
    ap.add_argument('--run-dir');ap.add_argument('--timeout',type=float,default=1800)
    ap.add_argument('--keep-shadow',action='store_true')
    args=ap.parse_args()
    rows=load_manifest()
    if args.ids:
        wanted=set(args.ids.split(','))
        if wanted-{r['id'] for r in rows}: raise ValueError('未知变异编号')
        rows=[r for r in rows if r['id'] in wanted]
    if args.list:
        for row in rows:print(row['id'],row['target'])
        return
    if args.audit:return audit(rows)
    directory=args.run_dir or os.environ.get('GFX_CHECK_RUN_DIR')
    existing=directory and (ROOT/directory/'run.json').is_file()
    patterns=None if existing else ['Main/**',*sorted({r['spec']['doc'] for r in rows})]
    current=Session(args.run_dir,args.profile,patterns)
    print('RUN '+current.path.relative_to(ROOT).as_posix(),flush=True)
    current.perform('mutation',lambda:campaign(current,rows,args.timeout,args.keep_shadow),reusable=False)
    print('MUTATION PASS',flush=True)

if __name__=='__main__':
    try:main()
    except (RuntimeError,ValueError,OSError) as error:
        print('FAIL '+str(error),file=sys.stderr);sys.exit(1)
