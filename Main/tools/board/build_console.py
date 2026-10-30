#!/usr/bin/env python3
"""Build and inspect one selected Ti60 console firmware, without deployment."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
from console_catalog import ROOT,APP,load,resolve


def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out',type=Path,required=True)
    ap.add_argument('--app-profile',choices=['2d','full'],default='full')
    ap.add_argument('--apps');args=ap.parse_args();out=args.out.resolve()
    if not out.is_relative_to(ROOT/'Main/sim/build'):ap.error('output must stay in Main/sim/build')
    if os.name=='nt':
        from build_pc import wsl_path
        cmd=['wsl.exe','-e','python3',wsl_path(__file__),'--out',wsl_path(out),'--app-profile',args.app_profile]
        if args.apps is not None:cmd+=['--apps',args.apps]
        raise SystemExit(subprocess.call(cmd,cwd=ROOT))
    expected=resolve(load(),args.app_profile,None if args.apps is None else args.apps.replace(',',' ').split())
    compiler=shutil.which(os.environ.get('RISCV_BIN','riscv-none-elf-')+'gcc')
    if not compiler:
        compiler=next(iter(sorted((Path.home()/'opt').glob('**/bin/riscv-none-elf-gcc'))),None)
    if not compiler:ap.error('riscv-none-elf-gcc is required')
    prefix=str(compiler)[:-3]
    cmd=['make','-C',str(APP),'-j4','PLATFORM=target','APP_PROFILE='+args.app_profile,
         'OBJDIR='+os.path.relpath(out,APP),'RISCV_BIN='+prefix]
    if args.apps is not None:cmd+=['APPS='+args.apps.replace(',',' ')]
    env={k:v for k,v in os.environ.items() if k not in ('APPS','APP_PROFILE','APP_CATALOG')}
    subprocess.run(cmd,cwd=ROOT,check=True,env=env)
    binary=out/'console-O2-perf.bin';elf=binary.with_suffix('.elf')
    content=json.loads((out/'catalog/content.json').read_text())
    if content!=expected:raise RuntimeError('build selection differs from requested content')
    if content['catalog_sha256'].encode() not in binary.read_bytes():raise RuntimeError('compiled content identity missing')
    if binary.stat().st_size>content['image_limit_bytes']:raise RuntimeError('bootloader image budget exceeded')
    assets=ROOT/'Main/assets/build/assets.pak'
    if assets.stat().st_size>content['asset_limit_bytes']:raise RuntimeError('Flash asset budget exceeded')
    symbols=subprocess.check_output([prefix+'nm','--defined-only',str(elf)],text=True)
    names=[line.split()[-1] for line in symbols.splitlines() if line.split()]
    r4=[n for n in names if n.startswith(('r4_','replay_r4_','rast_')) or n=='g_app_r4']
    if not content['enable_3d'] and r4:raise RuntimeError('3D symbols in 2D firmware: '+','.join(r4))
    if any(a['key']=='r4' for a in content['apps']) and 'g_app_r4' not in names:raise RuntimeError('selected R4 missing')
    size=subprocess.check_output([prefix+'size','-B',str(elf)],text=True)
    memory=list(map(int,size.splitlines()[1].split()[:3]))
    if sum(memory)>4092*1024:raise RuntimeError('program RAM budget exceeded')
    (out/'symbols.txt').write_text(symbols);(out/'size.txt').write_text(size)
    shutil.copy2(out/'catalog/content.json',out/'content.json')
    meta=dict(version=1,profile=content['profile'],catalog_sha256=content['catalog_sha256'],
        compiler=subprocess.check_output([str(compiler),'--version'],text=True).splitlines()[0],
        command=[x.replace(str(ROOT),'<REPO>').replace(prefix,'<RISCV_BIN>') for x in cmd],
        source_commit=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
        source_dirty=bool(subprocess.check_output(['git','status','--porcelain','--','Main/sw','Main/tools/board','Main/tools/check'],cwd=ROOT,text=True).strip()),
        files={p.name:dict(bytes=p.stat().st_size,sha256=sha(p)) for p in [binary,elf,binary.with_suffix('.map')]},
        assets=dict(bytes=assets.stat().st_size,sha256=sha(assets)),
        memory=dict(text=memory[0],data=memory[1],bss=memory[2],limit=4092*1024),
        image_limit_bytes=content['image_limit_bytes'],three_d_symbols=len(r4),scope='build and budget inspection; not board acceptance')
    (out/'firmware.json').write_text(json.dumps(meta,indent=2)+'\n')
    print('CONSOLE BUILD PASS '+content['profile']+' bytes='+str(binary.stat().st_size)+' 3d_symbols='+str(len(r4)))


if __name__=='__main__':main()
