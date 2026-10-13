#!/usr/bin/env python3
"""Windows 调用 WSL 编译、JTAG 装载与证据采集；不写 Flash。"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import os

MAIN=Path(__file__).resolve().parents[2]
ROOT=MAIN.parent
from check_run import paths
BUILD,LOGS=paths('frontend','frontend')

def register(path):
    dest=LOGS/'artifacts.json'
    data=json.loads(dest.read_text()) if dest.exists() else {}
    key=path.relative_to(ROOT).as_posix();st=path.stat();old=data.get(key,{})
    if old.get('size')!=st.st_size or old.get('mtime_ns')!=st.st_mtime_ns:
        data[key]={'size':st.st_size,'mtime_ns':st.st_mtime_ns,'sha256':hashlib.sha256(path.read_bytes()).hexdigest()}
        dest.write_text(json.dumps(data,indent=2)+'\n')

def main():
    p=argparse.ArgumentParser();p.add_argument('--tag',required=True);p.add_argument('--flags',default='')
    p.add_argument('--bit');p.add_argument('--no-program',action='store_true');p.add_argument('--seconds',type=int,default=3600)
    p.add_argument('--riscv-prefix');a=p.parse_args()
    LOGS.mkdir(parents=True,exist_ok=True);BUILD.mkdir(parents=True,exist_ok=True)
    prefix=a.riscv_prefix
    if not prefix:
        discovery="import pathlib; print(next(pathlib.Path.home().glob('opt/xpack-riscv-none-elf-gcc-13.4.0-*/bin/riscv-none-elf-gcc')))"
        prefix=subprocess.check_output(['wsl.exe','-e','python3','-c',discovery],cwd=ROOT,text=True).strip()[:-3]
    obj='build_'+a.tag
    flags='-I'+str(MAIN/'sw/apps/game/gfx2dGame/src').replace('\\','/')
    # WSL 内的应用 include 用相对路径，避免把 Windows 盘符交给 gcc。
    flags='-I../../game/gfx2dGame/src '+a.flags
    cmd=['wsl.exe','-e','make','-C','Main/sw/apps/probe/frontendBench','-j8','OBJDIR='+obj,
         'RISCV_BIN='+prefix,'EXTRA_CFLAGS='+flags]
    with (BUILD/(a.tag+'_compile.log')).open('w') as out:
        subprocess.run(cmd,cwd=ROOT,stdout=out,stderr=subprocess.STDOUT,check=True)
    binary=MAIN/'sw/apps/probe/frontendBench'/obj/'frontendBench-O2.bin';register(binary)
    args=[sys.executable,str(MAIN/'tools/board/board_run.py'),'--bin',str(binary),'--seconds',str(a.seconds),
          '--stop-on','done','--log',str(LOGS/(a.tag+'.log'))]
    if a.bit:args+=['--bit',a.bit]
    if a.no_program:args+=['--no-program']
    subprocess.run(args,cwd=ROOT,check=True)
    print('FRONTEND BOARD CAPTURED',a.tag,flush=True)
if __name__=='__main__':main()
