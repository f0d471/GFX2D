import hashlib,json,shutil
from pathlib import Path
ROOT=Path(__file__).resolve().parents[4];BASE=Path(__file__).parent
EVIDENCE=ROOT/'Engineering/TechReports/engine/evidence/19-r4-polish-review-20261024'
dest=EVIDENCE/'startup';dest.mkdir(exist_ok=True)
def canonical(data):
    if isinstance(data,str):return data.replace(str(ROOT),'<REPO>').replace(ROOT.as_posix(),'<REPO>')
    if isinstance(data,list):return [canonical(x) for x in data]
    if isinstance(data,dict):return {k:canonical(v) for k,v in data.items()}
    return data
packages={}
for version in ('current','next'):
    package=ROOT/'Main/bit'/('board_release_game_'+version+'_20261024')
    manifest=json.loads((package/'manifest.json').read_text(encoding='utf-8'))
    for entry in manifest['files']:
        assert hashlib.sha256((package/entry['path']).read_bytes()).hexdigest()==entry['sha256'],entry['path']
    packages[version]=dict(path=package.relative_to(ROOT).as_posix(),files=len(manifest['files']),
        manifest_sha256=hashlib.sha256((package/'manifest.json').read_bytes()).hexdigest())
for name in ('launch-current-fixed.json','launch-next-fullscreen.json','launch-next-live.json',
             'current-launcher-repair.json','verification.json'):
    value=json.loads((BASE/name).read_text(encoding='utf-8-sig'))
    (dest/name).write_text(json.dumps(canonical(value),indent=2)+'\n')
shutil.copy2(BASE/'launch-next-fullscreen.png',dest/'next-fullscreen.png')
for label in ('ps51','ps7'):shutil.copy2(BASE/'tamper-test'/(label+'.log'),dest/('tamper-'+label+'.log'))
shutil.copy2(BASE/'launch-current.stderr.log',dest/'initial-ps51-failure.log')
for name in ('verify_pc_package.ps1','repair_current_launcher.py','prepare_verification.py','seal_handoff.py'):
    shutil.copy2(BASE/name,dest/name)
next_package=ROOT/'Main/bit/board_release_game_next_20261024'
runtime=sorted(p for p in (next_package/'runtime').iterdir() if p.is_dir())[-1]
state=json.loads((runtime/'panel_status.json').read_text(encoding='utf-8'))
assert state['connected'] and state['mode']=='r3d' and state['backend']=='hw' and state['fps']==30
(dest/'board-live-state.json').write_text(json.dumps(state,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
for name in ('serial.log','deploy.log'):shutil.copy2(runtime/name,dest/('next-board-'+name))
pc=json.loads((BASE/'launch-next-live.json').read_text(encoding='utf-8-sig'));assert pc['status']=='PASS'
result=dict(status='PASS',packages=packages,pc_pid=pc['pid'],board_state=state,
    scope='both frozen PC entries opened, next fullscreen 2560x1600; old/new JTAG startup checked; next PC and board running',
    no_flash_write=True,rtl_difference_count=0)
(next_package/'runtime/ready.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
(dest/'ready.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
files=[]
for path in sorted(dest.rglob('*')):
    if path.is_file():files.append(dict(path=path.relative_to(dest).as_posix(),bytes=path.stat().st_size,
        sha256=hashlib.sha256(path.read_bytes()).hexdigest()))
(dest/'identity.json').write_text(json.dumps(files,indent=2)+'\n')
print('HANDOFF PASS',packages)
