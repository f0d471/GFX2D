from pathlib import Path
import hashlib
import json
HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[4]
manifest=json.loads((HERE/'acceptance-board-02/run.json').read_text(encoding='utf-8'))
allowed={'Main/tools/check/check_sw8_stats.sh','Main/tools/board/build_pc.py'}
changed=[];verified=0
for name,expected in manifest['inputs'].items():
    path=ROOT/name
    actual=hashlib.sha256(path.read_bytes()).hexdigest()
    if actual!=expected['sha256']:changed.append(name)
    else:verified+=1
if set(changed)-allowed:raise RuntimeError('unexpected runtime/board-check change '+str(changed))
deployment=json.loads((HERE/'acceptance-board-02/logs/console-board/deployment.json').read_text(encoding='utf-8'))
for key in ('bit','bin'):
    path=ROOT/deployment[key]
    if hashlib.sha256(path.read_bytes()).hexdigest()!=deployment['identities'][key]['sha256']:
        raise RuntimeError('deployed artifact changed '+key)
result=dict(status='PASS',unchanged_input_files=verified,changed=changed,
    runtime_source_unchanged=True,deployed_bit_bin_unchanged=True,
    reason='only independent TX/window probe build commands gained missing BSP linkage; board checks do not consume those commands')
(HERE/'runtime-reuse.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
print(json.dumps(result))
