"""Fix only the frozen current package's launcher; retain original manifest/payload."""
import hashlib,json,shutil
from pathlib import Path
ROOT=Path(__file__).resolve().parents[4]
bundle=ROOT/'Main/bit/board_release_game_current_20261024'
manifest=bundle/'manifest.json';original=bundle/'manifest.original.json'
assert not original.exists();shutil.copy2(manifest,original)
before=json.loads(manifest.read_text())
payload={e['path']:e['sha256'] for e in before['files'] if e['path'].startswith(('pc/','board/')) or e['path'] in ('assets.pak','source.zip')}
path=bundle/'Run-PC.ps1';text=path.read_text(encoding='utf-8-sig')
function='''function Get-ReleaseHash([string]$Path) {
    $algorithm = [Security.Cryptography.SHA256]::Create()
    $stream = [IO.File]::OpenRead($Path)
    try { return [BitConverter]::ToString($algorithm.ComputeHash($stream)).Replace('-','').ToLowerInvariant() }
    finally { $stream.Dispose(); $algorithm.Dispose() }
}
'''
text=text.replace("$ErrorActionPreference = 'Stop'\n","$ErrorActionPreference = 'Stop'\n"+function)
text=text.replace('(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()','(Get-ReleaseHash $path)')
path.write_text(text,encoding='utf-8-sig')
for name,sha in payload.items():assert hashlib.sha256((bundle/name).read_bytes()).hexdigest()==sha
for entry in before['files']:
    target=bundle/entry['path'];entry['bytes']=target.stat().st_size;entry['sha256']=hashlib.sha256(target.read_bytes()).hexdigest()
before['files'].append(dict(path=original.name,bytes=original.stat().st_size,sha256=hashlib.sha256(original.read_bytes()).hexdigest()))
before['launcher_revision']='PS5.1 module-independent SHA256; original game payload and source archive unchanged'
manifest.write_text(json.dumps(before,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
evidence=ROOT/'Engineering/TechReports/engine/evidence/19-r4-polish-review-20261024'
qa=evidence/'qa/manifest.json';data=json.loads(qa.read_text());data['baseline_manifest_file']='manifest.original.json'
assert data['baseline_manifest_sha256']==hashlib.sha256(original.read_bytes()).hexdigest()
qa.write_text(json.dumps(data,indent=2)+'\n')
copy=evidence/'copy_manifest.json';records=json.loads(copy.read_text())
for entry in records:
    target=evidence/entry['path'];entry['sha256']=hashlib.sha256(target.read_bytes()).hexdigest();entry['bytes']=target.stat().st_size
copy.write_text(json.dumps(records,indent=2)+'\n')
(Path(__file__).parent/'current-launcher-repair.json').write_text(json.dumps(dict(status='PASS',
    original_manifest_sha256=hashlib.sha256(original.read_bytes()).hexdigest(),unchanged_payload=payload,
    changed=['Run-PC.ps1','manifest.json'],added=['manifest.original.json']),indent=2)+'\n')
print('CURRENT LAUNCHER REPAIR PASS; frozen game payload unchanged')
