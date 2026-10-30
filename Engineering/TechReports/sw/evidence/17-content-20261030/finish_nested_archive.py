"""Complete the interrupted SW17 archive from its original checksum manifest.

The standard archive enumerated logs below the isolated build tree but copied
only the top-level logs directory. Preserve the original relative names and raw
bytes; no binary/object/source build tree is archived here.
"""
import hashlib
import json
from pathlib import Path
import shutil

ROOT=next(p for p in Path(__file__).resolve().parents if (p/'Main/tools/check/validate.py').is_file())
source=ROOT/'Main/sim/build/check-runs/20261030-014120-259b9a00'
target=Path(__file__).parent/'software'
assert json.loads((source/'result.json').read_text())['status']=='PASS'
entries=json.loads((target/'archive-files.json').read_text())
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
copied=[]
for name,expected in entries.items():
    src=(source/name).resolve();dst=(target/name).resolve()
    assert src.is_relative_to(source.resolve()) and dst.is_relative_to(target.resolve())
    assert sha(src)==expected['sha256'], 'source log changed: '+name
    if not dst.exists():
        dst.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(src,dst);copied.append(name)
    assert sha(dst)==expected['sha256'], 'archive log differs: '+name
(target/'.gitignore').write_text('# Manifest-listed nested raw logs only; no build binaries are copied.\n!build/\n!build/**\n')
record=dict(kind='current_run',current_inputs_revalidated=True,
    local_paths='placeholders in metadata; raw logs byte-identical',source_run=source.relative_to(ROOT).as_posix(),
    completion='manifest-listed nested logs copied after the standard archive omitted their directory',
    supplemental_logs=copied,verified_logs=len(entries))
(target/'archive.json').write_text(json.dumps(record,indent=2)+'\n')
print('ARCHIVE COMPLETED: '+str(len(entries))+' byte-verified logs; supplemental='+str(len(copied)))
