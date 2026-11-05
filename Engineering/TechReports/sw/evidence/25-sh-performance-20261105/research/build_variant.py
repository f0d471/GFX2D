import os,subprocess,sys,json
from pathlib import Path
root=Path.cwd();variant=sys.argv[1]
out=root/'Main/sim/build/sw21-board-audit'/variant
env=os.environ.copy()
flags='-DSH_REFERENCE_MATH=1 -DSH_DISABLE_VERTEX_CACHE=1' if variant=='reference' else '-DSH_PROFILE=1' if variant=='profile' else ''
env['EXTRA_CFLAGS']=flags
if variant=='size':
    env['EXTRA_CFLAGS']='-Os';env['MAKEFLAGS']='APP_O3_SRCS= LIB_O3_SRCS='
with (out.parent/(variant+'.build.log')).open('w') as log:
    subprocess.run(['python3','Main/tools/board/build_console.py','--out',str(out)],env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
(out/'variant.json').write_text(json.dumps(dict(variant=variant,flags=flags),indent=2))
print('VARIANT BUILD PASS '+variant)
