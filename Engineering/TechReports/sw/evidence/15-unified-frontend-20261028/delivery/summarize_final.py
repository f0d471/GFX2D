from pathlib import Path
import hashlib
import json
import sys
HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[4]
sys.path.insert(0,str(ROOT/'Main/tools/check'))
from check_run import input_identity
base=HERE/'acceptance-software-03'
software=HERE/'acceptance-software-delta-01'
board=HERE/'acceptance-board-02'
windows=HERE/'acceptance-windows-01'
frontend=HERE/'acceptance-frontend-02'
for path in (board,windows,frontend):
    result=json.loads((path/'result.json').read_text(encoding='utf-8'))
    if result.get('status')!='PASS':raise RuntimeError('incomplete final run '+str(path))
a=json.loads((software/'run.json').read_text(encoding='utf-8'))
old=json.loads((base/'run.json').read_text(encoding='utf-8'))
b=json.loads((board/'run.json').read_text(encoding='utf-8'))
aa=a.get('inputs',a.get('sources',{}));bb=b.get('inputs',b.get('sources',{}))
allowed={'Main/tools/check/check_sw8_stats.sh','Main/tools/board/build_pc.py',
         'Main/sim/engine/frontend/test_pipeline.c'}
different=[name for name in aa.keys()&bb.keys() if aa[name]['sha256']!=bb[name]['sha256']]
if set(different)-allowed:raise RuntimeError('cross-run producer identity mismatch '+str(different))
old_inputs=old['inputs']
changed=[name for name in old_inputs.keys()&aa.keys() if old_inputs[name]['sha256']!=aa[name]['sha256']]
if set(changed)-allowed:raise RuntimeError('reused-check producer changed '+str(changed))
base_checks=['pack','layers','workflow','regs','lib','app','pc','demo','game']
for name in base_checks:
    if old['checks']['check:'+name]['status']!='PASS':raise RuntimeError('base check incomplete '+name)
delta_checks=['workflow','stats','pack','r4','console']
for name in delta_checks:
    if a['checks']['check:'+name]['status']!='PASS':raise RuntimeError('delta check incomplete '+name)
all_checks=list(dict.fromkeys(base_checks+delta_checks+['frontend']))
validation_changes=[]
for name,expected in old_inputs.items():
    if input_identity(ROOT/name)['sha256']!=expected['sha256']:
        validation_changes.append(name)
if set(validation_changes)-allowed:raise RuntimeError('unexpected current producer change '+str(validation_changes))
normal=json.loads((board/'logs/console-board/result.json').read_text(encoding='utf-8'))
r4=json.loads((board/'logs/r4-board/result.json').read_text(encoding='utf-8'))
summary=dict(status='PASS',software_checks=all_checks,
    evidence_kind='scoped reuse of unchanged producers; not one combined software run',
    reused_checks=base_checks,current_checks=delta_checks+['frontend'],test_dependency_only_changes=validation_changes,
    board_checks=json.loads((board/'result.json').read_text())['checks'],
    common_source_files=len(aa.keys()&bb.keys()),runtime_source_identity_equal=True,
    source_identity_equal=False,source_differences=different,
    console=normal,r4=r4,flash_written=False,rtl_rebuilt=False,
    native_exe_sha256=hashlib.sha256((HERE/'native-verified/gfx2dGame.exe').read_bytes()).hexdigest())
(HERE/'final-summary.json').write_text(json.dumps(summary,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
print('FINAL CHECKS',summary['software_checks'],summary['board_checks'])
print('RUNTIME SOURCE IDENTITIES',summary['common_source_files'],'equal except declared unused probe scripts',different)
print('CONSOLE',normal['status'],'R4',r4['status'])
