"""Prove that the final SDK/target-only correction did not change host consumers."""
from pathlib import Path
import hashlib,json,subprocess,sys
R=Path(__file__).resolve().parents[4]
sys.path.insert(0,str(R/'Main/tools/check'))
from check_run import input_identity
run=R/'Main/sim/build/check-runs/20261029-031110-e416c678'
old=json.loads((run/'run.json').read_text())
allowed={
 'Main/sw/services/platform/platform.c','Main/sw/ports/ti60/app_port.c',
 'Main/sw/services/test/test_platform.c','Main/sw/services/test/test_scheduler_port.c',
 'Main/tools/check/check_platform_sdk.py'}
changed=[]
for rel,identity in old['inputs'].items():
    p=R/rel
    sha=input_identity(p)['sha256'] if p.is_file() else None
    if sha!=identity['sha256']:changed.append(rel)
if not set(changed)<=allowed:raise RuntimeError('Additional inputs changed: '+str(set(changed)-allowed))
checks=['pack','console','lib','app','frontend','pc-wide','r4']
for name in checks:
    if old['checks']['check:'+name]['status']!='PASS':raise RuntimeError('Not passed: '+name)
# Production app make chooses the host port and does not enable PLATFORM_SDK.
# Its dependency files must not include any of the changed source files/headers.
deps=list((run/'build/console').rglob('*.d'))
if not deps:raise RuntimeError('No actual host dependency files')
for dep in deps:
    body=dep.read_text()
    if any(str(R/rel) in body or rel in body for rel in allowed if rel.endswith(('.c','.h'))):
        raise RuntimeError('Changed source consumed by host frontend: '+str(dep))
value=dict(status='PASS',source_run=run.name,scope=checks,changed_inputs=changed,
    actual_host_dependency_files=len(deps),reason='only independent SDK implementation/test and target-only health port changed',
    not_reused=['platform-sdk','demo'],target_runtime='NOT_RUN')
out=R/'Main/sim/build/sw16/reuse-closure.json';out.write_text(json.dumps(value,indent=2)+'\n')
print('HOST CONSUMER REUSE PASS',','.join(checks),'changed',len(changed))
