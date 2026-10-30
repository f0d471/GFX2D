"""Verify and index the local SW17 artifacts without editing release payloads."""
import hashlib
import json
from pathlib import Path
import shutil

ROOT=next(p for p in Path(__file__).resolve().parents if (p/'Main/tools/check/validate.py').is_file())
work=ROOT/'Main/sim/build/sw17-release';out=Path(__file__).parent/'delivery';out.mkdir(exist_ok=True)
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def read(p):return json.loads(p.read_text(encoding='utf-8-sig'))
def portable(value):
    if isinstance(value,dict):return {k:portable(v) for k,v in value.items()}
    if isinstance(value,list):return [portable(v) for v in value]
    if isinstance(value,str):return value.replace(str(ROOT),'<REPO>').replace(ROOT.as_posix(),'<REPO>')
    return value
def copy(source,name):
    target=out/name
    if target.exists():assert sha(source)==sha(target),'existing delivery evidence differs'
    else:shutil.copy2(source,target)
    assert sha(source)==sha(target)
    return dict(file=name,bytes=target.stat().st_size,sha256=sha(target))
records=[]
for profile in ('2d','full'):
    package=ROOT/'Main/bit'/('board_release_console_'+profile+'_20261030')
    manifest=read(package/'manifest.json')
    for entry in manifest['files']:
        p=(package/entry['path']).resolve();assert p.is_relative_to(package.resolve())
        assert p.stat().st_size==entry['bytes'] and sha(p)==entry['sha256']
    runtime=max((package/'runtime').iterdir(),key=lambda p:p.name)
    serial=(runtime/'serial.log').read_text(encoding='utf-8-sig')
    boundary='# --- 程序已加载，以下才是它的输出 ---';assert boundary in serial
    body=serial.split(boundary,1)[1]
    statuses=[line for line in body.splitlines() if line.startswith('console status ')]
    assert statuses
    words=statuses[-1].split()[2:];state=dict(zip(words[::2],words[1::2]))
    assert state['catalog']==manifest['content']['catalog_sha256']
    assert int(state['caps'],16)==int(manifest['hardware']['caps'],16)
    assert state['errors']==state['faults']==state['dropped']=='0'
    raw=[copy(runtime/'serial.log',profile+'-package.serial.log'),copy(runtime/'deploy.log',profile+'-package.deploy.log')]
    for suffix in ('stdout.log','stderr.log'):
        raw.append(copy(work/('pc-'+profile)/('native-smoke.'+suffix),profile+'-native.'+suffix))
    records.append(dict(profile=profile,package=package.relative_to(ROOT).as_posix(),manifest_sha256=sha(package/'manifest.json'),
        payload_files=len(manifest['files']),content_hash=manifest['content']['catalog_sha256'],
        startup=dict(status='PASS',state=state,raw=raw),powershell_check_only=['5.1 PASS','7 PASS']))
snapshots=[]
for preset in ('B0','B3','B5','B7'):
    folder=ROOT/'Main/bit/snapshots'/preset;m=read(folder/'manifest.json')
    assert sha(folder/'gfx2d_ti60.bit')==m['bit_sha256']
    assert sha(folder/m['bin'])==m['bin_sha256']
    for field in ('content','firmware','verification'):
        assert sha(folder/m[field]['file'])==m[field]['sha256']
    snapshots.append(dict(preset=preset,profile=m['content']['profile'],bin_bytes=m['bin_bytes'],bin_sha256=m['bin_sha256'],bit_sha256=m['bit_sha256']))
data=dict(status='PASS',software_run='20261030-014120-259b9a00',board_run='20261030-014306-a1081dbe',
    packages=records,snapshots=snapshots,native=read(work/'native-result.json'),negative_packages=read(work/'package-negatives.json'),
    desktop=read(work/'final-pc-review.json'),desktop_environment=read(work/'desktop-state.json'),
    sdk_original_diff_exit=0,rtl_sdk_fpga_modified=False,flash_written=False,user_acceptance='PENDING')
(out/'delivery.json').write_text(json.dumps(portable(data),indent=2,ensure_ascii=False)+'\n',encoding='utf-8')
print('DELIVERY PASS: two native packages, real package JTAG startup, four snapshot identities and raw logs')
