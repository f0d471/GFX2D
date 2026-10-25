"""Cross-check native Windows game frame/record CRCs against accepted WSL cases."""
import json,os,shutil,subprocess,sys
from pathlib import Path

ROOT=Path(__file__).resolve().parents[4]
os.chdir(ROOT);sys.path.insert(0,str(ROOT/'Main/tools/check'))
from r4_checks import compare
run=ROOT/'Main/sim/build/check-runs/20261024-173917-188abb7f'
expected=json.loads((run/'logs/r4/expected.json').read_text())
output=Path(__file__).parent/'native-pixels';output.mkdir(exist_ok=True)
results=[]
for name,ladder,scene in [('l1',1,0),('l2',2,0),('cup',1,1)]:
    dest=output/name;(dest/'replay').mkdir(parents=True,exist_ok=True)
    shutil.copy2(run/'build/r4'/('host-'+name)/'replay/r4_replay.h',dest/'replay/r4_replay.h')
    flags='-DGFX2D_START_MODE=4 -DGFX2D_R4_HW=0 -DGFX2D_R4_JUDGE=1 -DGFX2D_R4_REPLAY=1'
    flags+=' -DGFX2D_FRAMES='+str(len(expected[name]))+' -DGFX2D_R4_LADDER='+str(ladder)+' -DGFX2D_R4_SCENE='+str(scene)
    with (dest/'build.log').open('w',encoding='utf-8') as log:
        subprocess.run([sys.executable,str(ROOT/'Main/tools/board/build_pc.py'),'--out',str(dest),'--flags='+flags],
            stdout=log,stderr=subprocess.STDOUT,check=True)
    env={**os.environ,'SDL_VIDEODRIVER':'dummy','SDL_AUDIODRIVER':'dummy','GFX2D_UNPACED':'1'}
    with (dest/'frames.log').open('w',encoding='utf-8') as log:
        subprocess.run([str(dest/'gfx2dGame.exe')],cwd=dest,env=env,stdout=log,stderr=subprocess.STDOUT,
            check=True,timeout=180)
    result=compare((dest/'frames.log').read_text(encoding='utf-8'),expected[name])
    results.append(dict(scene=name,**result));print('NATIVE PIXELS PASS '+name,flush=True)
(output/'result.json').write_text(json.dumps(dict(status='PASS',results=results,
    reference_run=run.name,scope='native Windows and accepted WSL exact frame/record CRC; 3 scenes'),indent=2)+'\n')
