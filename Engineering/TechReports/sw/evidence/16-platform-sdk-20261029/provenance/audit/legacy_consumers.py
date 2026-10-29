"""Only C/host consumers; deliberately never select game/RTL/protocol groups."""
from pathlib import Path
import json,os,subprocess,sys
R=Path(__file__).resolve().parents[4]
out=R/'Main/sim/build/sw16/legacy-consumers';out.mkdir(exist_ok=True)
if hasattr(os,'sched_getaffinity'):
    os.sched_setaffinity(0,set(sorted(os.sched_getaffinity(0))[:2]))
env={**os.environ,'SDL_VIDEODRIVER':'dummy','SDL_AUDIODRIVER':'dummy'}
groups=[('f4-lease',['Main/tools/check/check_f4_instances.py','--lease','--profile','acceptance']),
        ('f5-lease-replay',['Main/tools/check/check_f5_bin.py','--lease','--replay','--profile','acceptance']),
        ('g4-host',['Main/tools/check/check_g4_integration.py','--host','--profile','acceptance'])]
results=[]
for name,args in groups:
    with (out/(name+'.log')).open('w') as log:
        process=subprocess.run([sys.executable,*args],cwd=R,env=env,stdout=log,stderr=subprocess.STDOUT)
    if process.returncode:raise RuntimeError(name+' failed; see its retained log')
    results.append(dict(name=name,status='PASS',command=args,rtl=False,board=False))
(out/'result.json').write_text(json.dumps(dict(status='PASS',scope='historical host consumers',groups=results),indent=2)+'\n')
print('LEGACY HOST CONSUMERS PASS')
