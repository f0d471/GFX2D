#!/usr/bin/env python3
"""复制当前主工程生成缓存候选；不修改主线 IP、SDK 或 RTL。需 Efinity setup 环境。"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

MAIN = Path(__file__).resolve().parents[2]
SOURCE = MAIN/'fpga/gfx2d_ti60'


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--output',type=Path,default=MAIN/'sim/engine/g4/build/cache16/gfx2d_ti60')
    ap.add_argument('--size',type=int,default=16384)
    ap.add_argument('--ways',type=int,default=4)
    ap.add_argument('--build',action='store_true')
    ap.add_argument('--dry-run',action='store_true',help='复制并校验参数，不生成或编译 IP')
    ap.add_argument('--overwrite',action='store_true',help='显式允许重建已有候选目录')
    a=ap.parse_args()
    dest=a.output.resolve()
    if dest.is_relative_to(SOURCE.resolve()) or dest.name!='gfx2d_ti60':
        raise SystemExit('output 必须为独立的 gfx2d_ti60 复制目录')
    if dest.exists() and any(dest.iterdir()) and not a.overwrite:
        raise SystemExit('候选目录非空；选择新目录或显式给 --overwrite')
    if a.size not in (4096,8192,16384) or a.ways not in (1,2,4) or a.size>a.ways*4096:
        raise SystemExit('缓存容量或路数不满足 SoC IP 约束')
    if not os.environ.get('EFINITY_HOME'):
        raise SystemExit('先 call <EFINITY>/bin/setup.bat')
    dest.mkdir(parents=True,exist_ok=True)
    for name in ('gfx2d_ti60.xml','gfx2d_ti60.peri.xml','sdc','ip'):
        source=SOURCE/name
        if source.is_dir():
            shutil.copytree(source,dest/name,dirs_exist_ok=True)
        else:
            shutil.copy2(source,dest/name)
    xml=(dest/'gfx2d_ti60.xml').read_text(encoding='utf-8')
    def relocate(match):
        source=(SOURCE/match.group(2)).resolve()
        return match.group(1)+Path(os.path.relpath(source,dest)).as_posix()+match.group(3)
    xml=re.sub(r'(<efx:design_file name=")([^"]+)(")',relocate,xml)
    (dest/'gfx2d_ti60.xml').write_text(xml,encoding='utf-8')
    settings=dest/'ip/soc/settings.json'
    text=settings.read_text(encoding='utf-8')
    text=text.replace('__REPO_ROOT_FS__',MAIN.as_posix())
    text=text.replace('__REPO_ROOT_BS2__',str(MAIN).replace('\\','\\\\'))
    settings.write_text(text,encoding='utf-8')
    command=[sys.executable,str(Path(__file__).with_name('ipm_gen_soc.py')),
                    '--project',str(dest),'--override','DCacheSize='+str(a.size),
                    'DCACHEWAY='+str(a.ways)]
    if a.dry_run:
        command += ['--dry-run']
    subprocess.run(command,check=True)
    if a.dry_run:
        print('cache variant parameters validated; no IP build')
        return
    conf=json.loads(settings.read_text(encoding='utf-8'))['conf']
    if int(conf['DCacheSize'])!=a.size or int(conf['DCACHEWAY'])!=a.ways:
        raise RuntimeError('生成后的缓存配置与请求不符')
    if a.build:
        out=dest/'outflow';out.mkdir(exist_ok=True)
        p=subprocess.run([str(Path(os.environ['EFINITY_HOME'])/'bin/efx_run.bat'),
                          'gfx2d_ti60.xml'],cwd=dest,capture_output=True)
        (out/'g4_synthesis.log').write_bytes(p.stdout+p.stderr)
        if p.returncode:
            raise RuntimeError('候选编译失败，见 outflow/g4_synthesis.log')
    print('cache variant ready:',dest,'size=',a.size,'ways=',a.ways)


if __name__=='__main__':
    main()
