"""Archive this completed research once, with verified copies and no binaries."""
import hashlib,json,shutil,subprocess,sys
from pathlib import Path
ROOT=Path.cwd();OUT=Path(__file__).resolve().parent
DEST=ROOT/'Engineering/TechReports/engine/evidence/19-r4-research-20261023'
if DEST.exists():raise RuntimeError('archive already exists')
def identity(p):
    b=p.read_bytes();return dict(path=p.relative_to(ROOT).as_posix(),bytes=len(b),sha256=hashlib.sha256(b).hexdigest())
inputs=json.loads((OUT/'inputs.json').read_text(encoding='utf-8'))
for obj in inputs:
    if identity(ROOT/obj['path'])!=obj:raise RuntimeError('input changed: '+obj['path'])
additional=[]
for pattern in ['Main/sdk/bsp/efinix/EfxSapphireSoc/include/**/*.h',
                'Main/sdk/bsp/efinix/EfxSapphireSoc/app/**/*.h',
                'Main/sdk/software/standalone/include/**/*.h',
                'Main/sdk/software/standalone/driver/**/*.h',
                'Main/sdk/software/standalone/common/*.S',
                'Main/sdk/software/standalone/common/*.mk']:
    additional.extend(ROOT.glob(pattern))
additional.extend([ROOT/'Main/tools/board/input_forward.py',OUT/'finish.py',OUT/'summarize.py',OUT/'archive.py'])
supplement=[identity(p) for p in sorted(set(additional)) if p.is_file()]
(OUT/'supplemental-inputs.json').write_text(json.dumps(supplement,indent=2)+'\n',encoding='utf-8')
restore=json.loads((OUT/'restore.json').read_text())
binary=ROOT/restore['binary']['path']
if identity(binary)!=restore['binary']:raise RuntimeError('restore binary changed')
control=(OUT/'restore.control.log').read_text(encoding='utf-8',errors='replace')
if str(binary.stat().st_size)+' bytes written at address' not in control:raise RuntimeError('restore missing deployed byte count')
files=['probe.c','run.py','summarize.py','finish.py','archive.py','plan.json','inputs.json','supplemental-inputs.json',
       'artifacts.json','b7.json','port.json','usb.log','debug.log','target-build.log','host-build.log',
       'host.log','control.log','serial.log','result.json','summary.json','toolchain.log','packet-audit.json',
       'round-div-disassembly.txt','restore-build.log','restore.control.log','restore.serial.log',
       'restore-followup.serial.log','restore.json']
DEST.mkdir(parents=True)
for name in files:shutil.copyfile(OUT/name,DEST/name)
for src,name in [('target/.cflags','target-flags.txt'),('restore/.cflags','restore-flags.txt')]:
    shutil.copyfile(OUT/src,DEST/name)
manifest=[]
for p in sorted(DEST.iterdir()):
    if p.name=='target-flags.txt':src=OUT/'target/.cflags'
    elif p.name=='restore-flags.txt':src=OUT/'restore/.cflags'
    else:src=OUT/p.name
    if p.read_bytes()!=src.read_bytes():raise RuntimeError('copy mismatch')
    b=p.read_bytes();manifest.append(dict(path=p.name,bytes=len(b),sha256=hashlib.sha256(b).hexdigest()))
data=dict(status='PASS',scope='R4 predevelopment research only',source_run=OUT.relative_to(ROOT).as_posix(),
    revision=subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),
    input_count=len(inputs)+len(supplement),copied_files=manifest,binaries_archived=False,
    production_files_modified=False,board_restored=restore['status'],
    limitations=['120 synthetic frames rather than full R4 60-second acceptance','no actual cup workload',
                 'no new synthesis','no L2 frame testing in this sampling','no Flash','no human HDMI observation'])
(DEST/'archive.json').write_text(json.dumps(data,indent=2)+'\n',encoding='utf-8')
print('ARCHIVED',len(manifest),'files; unchanged inputs',len(inputs),'supplemental',len(supplement))
