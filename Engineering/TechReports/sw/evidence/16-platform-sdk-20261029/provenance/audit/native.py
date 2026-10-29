from pathlib import Path
import hashlib,json,os,shutil,subprocess
R=Path(__file__).resolve().parents[4]
out=R/'Main/sim/build/sw16/native-windows';out.mkdir(exist_ok=True)
sdk=R/'Main/sim/build/windows/SDL2-2.32.10/x86_64-w64-mingw32'
if not (sdk/'bin/SDL2.dll').is_file():raise RuntimeError('Existing SDL2 cache required')
if hasattr(os,'sched_getaffinity'):
    cores=sorted(os.sched_getaffinity(0));os.sched_setaffinity(0,set(cores[:2]))
command=['make','-C',str(R/'Main/sw/apps/console'),'-j2','PLATFORM=host',
    'OBJDIR='+str(out),'NAME=console.exe','HOST_CC=x86_64-w64-mingw32-gcc','PERF=1',
    'ASSETS_PAK=assets.pak','SDL_CFLAGS=-isystem '+str(sdk/'include/SDL2'),
    'SDL_LIBS='+str(sdk/'lib/libSDL2.dll.a')+' -static-libgcc -mwindows']
subprocess.run(command,cwd=R,check=True)
shutil.copy2(sdk/'bin/SDL2.dll',out/'SDL2.dll')
shutil.copy2(R/'Main/assets/build/assets.pak',out/'assets.pak')
value=dict(status='BUILD_PASS',scope='native Windows PC frontend, not board firmware',
    command=[x.replace(str(R),'<REPO>') for x in command],
    compiler=subprocess.check_output(['x86_64-w64-mingw32-gcc','--version'],text=True).splitlines()[0],
    files={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in [out/'console.exe',out/'SDL2.dll',out/'assets.pak']})
(out/'build.json').write_text(json.dumps(value,indent=2)+'\n')
print('NATIVE WINDOWS BUILD PASS')
