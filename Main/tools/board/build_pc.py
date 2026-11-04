#!/usr/bin/env python3
"""Build a native Windows PC game (or window integration check) using WSL MinGW."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import urllib.request

ROOT=Path(__file__).resolve().parents[3]
LEGACY_APP=ROOT/'Main/sw/apps/game/gfx2dGame'
PRODUCT_APP=ROOT/'Main/sw/apps/console'
VERSION='2.32.10'


def wsl_path(path):
    path=Path(path).resolve()
    return '/mnt/'+path.drive[0].lower()+path.as_posix()[2:]


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out',type=Path,required=True)
    ap.add_argument('--window-test',action='store_true')
    ap.add_argument('--legacy',action='store_true',help='build the explicit development/judge entry')
    ap.add_argument('--flags',default='')
    ap.add_argument('--app-profile',choices=['2d','full'],default='full')
    ap.add_argument('--apps',help='explicit comma-separated content keys')
    args=ap.parse_args();out=args.out.resolve()
    app=LEGACY_APP if args.legacy else PRODUCT_APP
    if ROOT not in out.parents:ap.error('output must stay inside this checkout')
    if os.name=='nt':
        command=['wsl.exe','-e','python3',wsl_path(__file__),'--out',wsl_path(out),'--flags='+args.flags]
        if args.window_test:command.append('--window-test')
        if args.legacy:command.append('--legacy')
        command+=['--app-profile',args.app_profile]
        if args.apps is not None:command+=['--apps',args.apps]
        raise SystemExit(subprocess.call(command,cwd=ROOT))
    out.mkdir(parents=True,exist_ok=True)
    sdk=ROOT/'Main/sim/build/windows'/('SDL2-'+VERSION)/'x86_64-w64-mingw32'
    if not (sdk/'bin/SDL2.dll').exists():
        previous=LEGACY_APP/'build_host_windows_play/deps'/('SDL2-'+VERSION)/'x86_64-w64-mingw32'
        if previous.exists():shutil.copytree(previous,sdk,dirs_exist_ok=True)
        else:
            cache=sdk.parent.parent;cache.mkdir(parents=True,exist_ok=True)
            archive=cache/('SDL2-devel-'+VERSION+'-mingw.tar.gz')
            url='https://github.com/libsdl-org/SDL/releases/download/release-'+VERSION+'/'+archive.name
            urllib.request.urlretrieve(url,archive)
            subprocess.run(['tar','-xzf',str(archive),'-C',str(cache)],check=True)
    compiler='x86_64-w64-mingw32-gcc'
    if not shutil.which(compiler):raise SystemExit('Install the WSL mingw-w64 compiler first.')
    if args.window_test:
        command=[compiler,'-std=gnu11','-O2','-g','-Wall','-Wextra','-Werror',
            '-isystem',str(sdk/'include/SDL2'),'-I',str(ROOT/'Main/sw/bsp/include'),
            str(ROOT/'Main/sim/host/test_window.c'),str(ROOT/'Main/sw/bsp/memmap.c'),
            str(ROOT/'Main/sw/services/input/board_input.c'),
            str(sdk/'lib/libSDL2.dll.a'),'-static-libgcc','-mwindows','-limm32','-o',str(out/'window_test.exe')]
    else:
        command=['make','-C',str(app),'-j6','PLATFORM=host','OBJDIR='+os.path.relpath(out,app),
            'NAME=gfx2dGame.exe','HOST_CC='+compiler,'PERF=1','EXTRA_CFLAGS='+args.flags,
            'ASSETS_PAK=assets.pak','SDL_CFLAGS=-isystem '+str(sdk/'include/SDL2'),
            'SDL_LIBS='+str(sdk/'lib/libSDL2.dll.a')+' -static-libgcc -mwindows']
        if not args.legacy:
            command+=['APP_PROFILE='+args.app_profile]
            if args.apps is not None:command+=['APPS='+args.apps.replace(',',' ')]
    env={k:v for k,v in os.environ.items() if k not in ('APPS','APP_PROFILE','APP_CATALOG')}
    subprocess.run(command,check=True,cwd=ROOT,env=env)
    shutil.copy2(sdk/'bin/SDL2.dll',out/'SDL2.dll')
    if not args.window_test:shutil.copy2(ROOT/'Main/assets/build/assets.pak',out/'assets.pak')
    (out/'build.json').write_text(json.dumps(dict(compiler=compiler,sdl=VERSION,
        flags=args.flags,window_test=args.window_test,frontend='legacy' if args.legacy else 'console',
        command=command),indent=2)+'\n')
    if not args.window_test and not args.legacy:
        shutil.copy2(out/'catalog/content.json',out/'content.json')
    print('WINDOWS BUILD PASS '+str(out),flush=True)


if __name__=='__main__':main()
