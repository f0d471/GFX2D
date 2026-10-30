#!/usr/bin/env python3
"""检查运行的输入身份、同轮复用、状态和输出目录。"""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import uuid
import xml.etree.ElementTree as ET
import fnmatch

ROOT = Path(__file__).resolve().parents[3]
RUNS = ROOT / 'Main/sim/build/check-runs'
CACHE_DIR = RUNS / 'cache'
SOURCE_SUFFIXES = {'.c', '.h', '.inc', '.v', '.vh', '.py', '.sh', '.ps1', '.mk',
                   '.xml', '.sdc', '.json', '.bin', '.dat', '.txt', '.g2dr', '.png', '.bdf',
                   '.hex', '.ld', '.cfg', '.yaml', '.yml'}
_SESSION = None


class IncompleteCheck(RuntimeError):
    """选定范围中的必需项没有执行。"""


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    pending = path.with_suffix(path.suffix + '.' + uuid.uuid4().hex[:8] + '.new')
    pending.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    pending.replace(path)


def git(*args):
    return subprocess.check_output(['git', '-c', 'safe.directory=' + ROOT.as_posix(),
                                    *args], cwd=ROOT)


def source_files():
    files = git('ls-files', '-z', '--cached', '--others', '--exclude-standard', '--', 'Main')
    result = []
    for name in files.decode('utf-8').split('\0'):
        p = Path(name)
        if not name or 'logs' in p.parts or (p.suffix.lower() not in SOURCE_SUFFIXES and p.name.lower()!='makefile'):
            continue
        if any(x == 'build' or x.startswith(('build_', 'build-', 'work_')) or x == 'outflow' for x in p.parts):
            continue
        if (ROOT / p).is_file():
            result.append(p.as_posix())
    return sorted(set(result))


def identity(path):
    st = path.stat()
    return {'size': st.st_size, 'mtime_ns': st.st_mtime_ns,
            'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}


def input_identity(path):
    st=path.stat()
    data={'size':st.st_size,'mtime_ns':st.st_mtime_ns}
    payload=path.read_bytes()
    if path.suffix=='.xml':
        try:
            root=ET.fromstring(payload)
            if root.tag.endswith('project'):
                for key in ['last_change','last_run_state','last_run_flow','config_result_in_sync',
                            'design_ood','place_ood','route_ood']:
                    root.attrib.pop(key,None)
                for node in root.iter():
                    if node.text is not None and not node.text.strip():node.text=None
                    if node.tail is not None and not node.tail.strip():node.tail=None
                payload=ET.tostring(root)
                data['identity_kind']='project_inputs_without_flow_status'
        except ET.ParseError:pass
    data['sha256']=hashlib.sha256(payload).hexdigest()
    return data


def cached_identities(names, role, fresh=False):
    path=CACHE_DIR/(role+'.json')
    cache=json.loads(path.read_text(encoding='utf-8')) if path.exists() else {}
    result={};changed=False
    for name in names:
        p=ROOT/name;st=p.stat();old=cache.get(name,{})
        if not fresh and (st.st_size,st.st_mtime_ns)==(old.get('size'),old.get('mtime_ns')):
            result[name]=old
        else:
            result[name]=input_identity(p) if role=='inputs' else identity(p)
            cache[name]=result[name];changed=True
    if changed:write_json(path,cache)
    return result


def selected_sources(patterns):
    names=[name for name in source_files() if any(fnmatch.fnmatchcase(name,p) for p in patterns if not p.startswith('@'))]
    if '@tracked-text' in patterns:
        from localize_paths import candidates
        names += [name for name,_ in candidates(True)]
    for pattern in patterns:
        if not any(c in pattern for c in '*?[') and not pattern.startswith('@') and (ROOT/pattern).is_file():
            names.append(pattern)
    return sorted({name for name in names if (ROOT/name).is_file()})


class Session:
    def __init__(self, directory=None, profile=None, input_patterns=None, fresh=False):
        if not __debug__:raise RuntimeError('检查入口禁止 python -O：正式判据使用 assert')
        profile = profile or os.environ.get('GFX_CHECK_PROFILE', 'acceptance')
        if profile not in ('quick', 'acceptance', 'release'):
            raise ValueError('profile 必须是 quick / acceptance / release')
        directory = directory or os.environ.get('GFX_CHECK_RUN_DIR')
        self.path = (ROOT / directory).resolve() if directory else RUNS / (time.strftime('%Y%m%d-%H%M%S') + '-' + uuid.uuid4().hex[:8])
        if not self.path.is_relative_to(RUNS.resolve()) or self.path == RUNS.resolve():
            raise ValueError('运行目录必须在 Main/sim/build/check-runs/ 下')
        self.path.mkdir(parents=True, exist_ok=True)
        self.manifest = self.path / 'run.json'
        self.profile = profile
        settings = {key: os.environ.get(key, '') for key in
                    ('HOST_CC', 'CC', 'CFLAGS', 'CPPFLAGS', 'LDFLAGS', 'OPT', 'RISCV_BIN',
                     'SW6_QUICK', 'SW7_SKIP_SEARCH', 'SW8_SKIP_LONG', 'EXTRA_CFLAGS',
                     'INJECT', 'VEC', 'GOLDEN', 'SAN32', 'TEST_ARGS', 'MAKEFLAGS', 'ASSETS_PAK',
                     'ASSETS_BUILD', 'GFX2D_ASSETS_PAK', 'GFX2D_TX_BUDGET',
                     'GFX2D_ASPECT', 'GFX2D_FULLSCREEN', 'GFX2D_SCALE_FILTER',
                     'APP_PROFILE','APPS','APP_CATALOG')}
        if self.manifest.exists():
            old = self.read()
            self.patterns=input_patterns or old.get('input_patterns',['Main/**'])
            if self.patterns!=old.get('input_patterns',['Main/**']):raise RuntimeError('本轮输入范围已改变，请开始新轮')
            if old['profile'] != profile or old['settings'] != settings:
                raise RuntimeError('运行参数已改变，请建立新的运行目录')
            self.verify(fresh)
        else:
            self.patterns=input_patterns or ['Main/**']
            inputs = cached_identities(selected_sources(self.patterns),'inputs',fresh)
            write_json(self.manifest, {'schema': 1, 'run_id': self.path.name,
                'revision': git('rev-parse', 'HEAD').decode().strip(), 'profile': profile,
                'settings': settings, 'input_patterns':self.patterns,'inputs': inputs, 'checks': {}, 'artifacts': {}})
        # 相对仓库根的路径可由 Windows 与 WSL 共用。
        os.environ['GFX_CHECK_RUN_DIR'] = self.path.relative_to(ROOT.resolve()).as_posix()
        os.environ['GFX_CHECK_PROFILE'] = profile
        os.environ['GFX_CHECK_SHARED'] = (self.path / 'shared').as_posix()

    def read(self):
        return json.loads(self.manifest.read_text(encoding='utf-8'))

    def verify(self, fresh=False):
        data = self.read()
        if data.get('invalidated'):raise RuntimeError(data['invalidated'])
        inputs = data['inputs']
        if set(inputs) != set(selected_sources(self.patterns)):
            data['invalidated']='检查输入集合已改变，请建立新的运行目录'
            write_json(self.manifest,data)
            raise RuntimeError(data['invalidated'])
        refreshed = False
        for name, previous in inputs.items():
            p = ROOT / name
            st = p.stat()
            if not fresh and (st.st_size, st.st_mtime_ns) == (previous['size'], previous['mtime_ns']):
                continue
            current = cached_identities([name],'inputs',fresh)[name]
            if current['sha256'] != previous['sha256']:
                data['invalidated']='检查输入已改变: ' + name + '；请建立新的运行目录'
                write_json(self.manifest,data)
                raise RuntimeError(data['invalidated'])
            inputs[name] = current
            refreshed = True
        if refreshed:
            write_json(self.manifest, data)

    def logs(self, component):
        path = self.path / 'logs' / component
        path.mkdir(parents=True, exist_ok=True)
        return path

    def build(self, component):
        path = self.path / 'build' / component
        path.mkdir(parents=True, exist_ok=True)
        return path

    def record(self, name, status, **details):
        data = self.read()
        previous = data['checks'].get(name, {})
        data['checks'][name] = {'status': status, 'attempt': previous.get('attempt', 0) + (status == 'RUNNING'), **details}
        write_json(self.manifest, data)

    def perform(self, name, action, reusable=True):
        self.verify()
        previous=self.read()['checks'].get(name,{})
        log=previous.get('details',{}).get('log')
        if reusable and previous.get('status') == 'PASS' and (not log or (ROOT/log).is_file()):
            print('REUSE ' + name + ' (same run, inputs and settings)', flush=True)
            return
        started = time.monotonic()
        self.record(name, 'RUNNING')
        try:
            result = action()
            self.verify()
        except Exception as error:
            incomplete = isinstance(error, IncompleteCheck) or getattr(error, 'returncode', None) == 3
            self.record(name, 'INCOMPLETE' if incomplete else 'FAIL', error=str(error),
                        seconds=round(time.monotonic() - started, 3))
            raise
        self.record(name, 'PASS', seconds=round(time.monotonic() - started, 3),
                    details=result if isinstance(result,dict) else {})
        return result

    def require(self, names):
        self.verify()
        checks = self.read()['checks']
        missing = [name for name in names if checks.get(name, {}).get('status') != 'PASS']
        missing += [name for name in names if checks.get(name,{}).get('details',{}).get('log') and
                    not (ROOT/checks[name]['details']['log']).is_file()]
        if missing:
            raise IncompleteCheck('本轮缺少通过证据: ' + ', '.join(missing))

    def artifact(self, path):
        path = Path(path).resolve()
        key = path.relative_to(ROOT.resolve()).as_posix()
        data = self.read()
        old = data['artifacts'].get(key, {})
        st = path.stat()
        if (old.get('size'), old.get('mtime_ns')) != (st.st_size, st.st_mtime_ns):
            data['artifacts'][key] = cached_identities([key],'artifacts')[key]
            write_json(self.manifest, data)
        return key


def session():
    global _SESSION
    if _SESSION is None:
        # 公共参数由所有检查入口接收，避免在每个 argparse 中复制定义。
        for option, variable in [('--run-dir', 'GFX_CHECK_RUN_DIR'), ('--profile', 'GFX_CHECK_PROFILE')]:
            if option in sys.argv:
                index = sys.argv.index(option)
                if index + 1 == len(sys.argv):
                    raise ValueError(option + ' 缺少取值')
                os.environ[variable] = sys.argv[index + 1]
                del sys.argv[index:index + 2]
        _SESSION = Session()
        print('RUN '+_SESSION.path.relative_to(ROOT).as_posix(),flush=True)
    return _SESSION


def paths(component, build):
    if '--help' in sys.argv or '-h' in sys.argv:
        return RUNS/'help/build'/build,RUNS/'help/logs'/component
    current = session()
    return current.build(build), current.logs(component)


def command(args, log, cwd=ROOT, env=None, timeout=1800):
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open('w', encoding='utf-8') as output:
        p = subprocess.run(list(map(str, args)), cwd=cwd, env=env, stdout=output,
                           stderr=subprocess.STDOUT, timeout=timeout)
    if p.returncode == 3:
        raise IncompleteCheck(str(log) + ': 必需项未执行')
    if p.returncode:
        raise subprocess.CalledProcessError(p.returncode, args)
    return log.read_text(encoding='utf-8', errors='replace')
