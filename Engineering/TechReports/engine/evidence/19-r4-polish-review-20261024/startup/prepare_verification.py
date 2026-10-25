import hashlib,json,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[4]
BASE=Path(__file__).parent
sys.path.insert(0,str(ROOT/'Main/tools/check'))
from check_run import selected_sources
ids={'software':'20261024-173917-188abb7f','window':'20261024-170736-690e3edf',
     'board':'20261024-192236-f8d2026c','layers':'20261024-194329-2f170490'}
runs={};deltas=[]
for role,name in ids.items():
    folder=ROOT/'Main/sim/build/check-runs'/name
    result=json.loads((folder/'result.json').read_text(encoding='utf-8-sig'));assert result['status']=='PASS'
    data=json.loads((folder/'run.json').read_text(encoding='utf-8-sig'))
    assert set(data['inputs'])==set(selected_sources(data['input_patterns']))
    for file,old in data['inputs'].items():
        current=hashlib.sha256((ROOT/file).read_bytes()).hexdigest()
        if current!=old['sha256']:
            assert file=='Main/tools/board/package_game.py',(role,file)
            deltas.append(dict(run=name,path=file,scope='packaging-only late PS5.1 launcher fix',
                accepted_sha256=old['sha256'],release_sha256=current))
    runs[role]=result
current=ROOT/'Main/bit/board_release_game_current_20261024'
manifest=json.loads((current/'manifest.json').read_text(encoding='utf-8-sig'))
for file,sha in manifest['rtl'].items():assert hashlib.sha256((ROOT/file).read_bytes()).hexdigest()==sha
fixture=BASE/'tamper-test';(fixture/'pc').mkdir(parents=True,exist_ok=True)
(fixture/'Run-PC.ps1').write_bytes((current/'Run-PC.ps1').read_bytes())
expected=hashlib.sha256(b'original').hexdigest()
(fixture/'manifest.json').write_text(json.dumps(dict(files=[dict(path='pc/gfx2dGame.exe',sha256=expected)])))
(fixture/'pc/gfx2dGame.exe').write_bytes(b'changed')
tamper=[]
for exe in ('C:/Windows/System32/WindowsPowerShell/v1.0/powershell.exe','E:/Software/Powershell/7/pwsh.exe'):
    process=subprocess.run([exe,'-NoProfile','-ExecutionPolicy','Bypass','-File',str(fixture/'Run-PC.ps1')],
        stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
    text=process.stdout.decode('utf-8','replace')
    assert process.returncode!=0 and 'Release file changed' in text,(exe,text)
    label='ps51' if 'WindowsPowerShell' in exe else 'ps7'
    (fixture/(label+'.log')).write_bytes(process.stdout)
    tamper.append(dict(shell=label,status='EXPECTED_RED',reason='changed executable rejected before launch'))
board=json.loads((ROOT/'Main/sim/build/check-runs'/ids['board']/'logs/r4-board/result.json').read_text(encoding='utf-8-sig'))
native=json.loads((BASE/'native-pixels/result.json').read_text(encoding='utf-8-sig'));assert native['status']=='PASS'
payload=dict(status='PASS',scope='selected render/window/layer acceptance plus exact native CRC; package entry proof in runtime/',
    runs=runs,source_scope_bridge=deltas,render_and_rtl_inputs_unchanged=True,rtl_difference_count=0,
    board_results=board['results'],native_pixels=native,launcher_tamper=tamper,
    note='Broad original run inputs included the package generator. Only its post-acceptance PS5.1 hash implementation changed; C/H/atlas/bit/build inputs are identical. Original run boundaries retained.')
(BASE/'verification.json').write_text(json.dumps(payload,indent=2)+'\n')
print('RELEASE VERIFICATION PASS; renderer/RTL input differences=0; PS5.1/PS7 tamper EXPECTED_RED')
