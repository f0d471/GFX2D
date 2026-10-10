#!/usr/bin/env python3
"""在 WSL 的隔离源码目录重建 F1 首轮，并对比唯一部署清单里的预期身份。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile

ROOT=Path(__file__).resolve().parents[3]
SIM=ROOT/'Main/sim/engine/frontend'
BASE='1e2cd8d'
KEY='Main/sw/apps/probe/frontendBench/build/frontendBench-O2.bin'
def main():
    p=argparse.ArgumentParser();p.add_argument('--riscv-prefix');a=p.parse_args()
    prefix=a.riscv_prefix or os.environ.get('RISCV_BIN')
    if not prefix:prefix=str(next(Path.home().glob('opt/xpack-riscv-none-elf-gcc-13.4.0-*/bin/riscv-none-elf-gcc')))[:-3]
    build=SIM/'build';build.mkdir(exist_ok=True)
    folder=Path(tempfile.mkdtemp(prefix='baseline_repro_',dir=build)).resolve()
    try:
        archive=folder/'sources.tar';tree=folder/'source';tree.mkdir()
        subprocess.run(['git','-c','safe.directory='+str(ROOT),'archive',BASE,'Main/sw','Main/sdk',
                        'Main/tools/check/replay2c.py','--output',str(archive)],cwd=ROOT,check=True)
        with tarfile.open(archive) as t:
            members=t.getmembers()
            if any(not (tree/m.name).resolve().is_relative_to(tree) or not (m.isfile() or m.isdir()) for m in members):
                raise RuntimeError('archive path/type rejected')
            t.extractall(tree)
        app=tree/'Main/sw/apps/probe/frontendBench';(app/'src').mkdir(parents=True)
        frozen=SIM/'baseline';shutil.copy2(frozen/'Makefile',app/'Makefile')
        for name in ('main.c','consume.c'):shutil.copy2(frozen/name,app/'src'/name)
        assets=tree/'Main/assets/build';assets.mkdir(parents=True)
        for name in ('assets.h','assets.pak'):shutil.copy2(ROOT/'Main/assets/build'/name,assets/name)
        with (build/'baseline_rebuild.log').open('w') as out:
            subprocess.run(['make','-C',str(app),'-j8','RISCV_BIN='+prefix],cwd=ROOT,stdout=out,stderr=subprocess.STDOUT,check=True)
        image=(app/'build/frontendBench-O2.bin').read_bytes()
        registry=json.loads((SIM/'logs/artifacts.json').read_text())[KEY]
        match=len(image)==registry['size'] and hashlib.sha256(image).hexdigest()==registry['sha256']
        result={'base_revision':BASE,'registry':'Main/sim/engine/frontend/logs/artifacts.json',
                'artifact_key':KEY,'rebuilt_size':len(image),'registered_identity_match':match}
        (SIM/'logs/baseline_registered_reproduction.json').write_text(json.dumps(result,indent=2)+'\n')
        if not match:raise RuntimeError('rebuilt F1 image differs from deployed baseline')
        print('F1 BASELINE REBUILD PASS: registered 124430-byte image reproduced')
    finally:
        if folder.parent!=build.resolve() or not folder.name.startswith('baseline_repro_'):
            raise RuntimeError('refusing cleanup outside owned build folder')
        shutil.rmtree(folder)
if __name__=='__main__':main()
