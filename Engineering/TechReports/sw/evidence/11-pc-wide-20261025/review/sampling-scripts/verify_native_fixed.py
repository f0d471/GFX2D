import hashlib, json, os, re, subprocess, sys
from pathlib import Path

folder=Path(__file__).resolve().parent
root=folder.parents[3]
baseline=json.loads((folder/'baseline/frames.json').read_text())
results={}
for name,ladder,scene in [('l1',1,0),('l2',2,0),('cup',1,1)]:
    out=folder/('native-fixed-'+name)
    flags=(f'-DGFX2D_START_MODE=4 -DGFX2D_FRAMES=60 -DGFX2D_R4_LADDER={ladder}'
           f' -DGFX2D_R4_SCENE={scene} -DGFX2D_R4_HW=0 -DGFX2D_R4_JUDGE=1')
    with (folder/('native-fixed-'+name+'.build.log')).open('w') as log:
        subprocess.run([sys.executable,str(root/'Main/tools/board/build_pc.py'),
            '--out',str(out),'--flags='+flags],cwd=root,stdout=log,stderr=subprocess.STDOUT,check=True)
    binary=out/'gfx2dGame.exe'
    result=subprocess.run([str(binary)],cwd=out,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,
        timeout=120,env={**os.environ,'SDL_VIDEODRIVER':'dummy','SDL_AUDIODRIVER':'dummy',
        'GFX2D_UNPACED':'1','GFX2D_ASPECT':'fixed','GFX2D_ASSETS_PAK':str(out/'assets.pak')})
    (folder/('native-fixed-'+name+'.log')).write_bytes(result.stdout)
    body=result.stdout.decode('utf-8',errors='replace')
    rows=[list(x) for x in re.findall(r'^R4CAD (\d+) .*? ([0-9a-f]{8}) ([0-9a-f]{8})\s*$',body,re.M)]
    if result.returncode or rows!=baseline[name] or re.findall(r'^fail (\d+)\s*$',body,re.M)!=['0']:
        raise RuntimeError('native fixed pixels/records differ: '+name)
    results[name]=dict(status='PASS',frames=60,flags=flags,bytes=binary.stat().st_size,
        sha256=hashlib.sha256(binary.read_bytes()).hexdigest())
    print('NATIVE FIXED PASS '+name+' 60 pixels and records match baseline',flush=True)
(folder/'native-fixed-result.json').write_text(json.dumps(dict(status='PASS',scenes=results,
    scope='native Windows fixed-canvas pixels/records vs pre-change WSL baseline; 180 frames'),indent=2)+'\n')
