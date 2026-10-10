#!/usr/bin/env python3
"""F4 的本机重建、唯一产物登记与 JTAG/DDR 板测；串口严格串行。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
ROOT=Path(__file__).resolve().parents[3]
MAIN=ROOT/'Main';LOGS=MAIN/'sim/engine/frontend/logs';BUILD=MAIN/'sim/engine/frontend/build/f4'
JOBS={
 'tests':('test/instanceTest','instanceTest',False,''),
 'tests-final':('test/instanceTest','instanceTest',False,''),
 'fallback':('test/instanceTest','instanceTest',False,''),
 'research-fixed':('probe/f4Bench','f4Bench',False,''),
 'ab':('probe/frontendBench','frontendBench',False,'-I../../game/gfx2dGame/src -DFRONTEND_MODE_LAST=11 -DFRONTEND_MODE_MASK=0x401 -DFRONTEND_ITERATIONS=30 -DFRONTEND_EXECUTE=1'),
 'verify':('probe/frontendBench','frontendBench',False,'-I../../game/gfx2dGame/src -DFRONTEND_MODE_LAST=11 -DFRONTEND_MODE_MASK=0x401 -DFRONTEND_ITERATIONS=1 -DFRONTEND_EXECUTE=1 -DFRONTEND_VERIFY'),
 'pressure-check':('game/gfx2dGame','gfx2dGame',True,'-DGFX2D_START_MODE=3 -DGFX2D_PRESS_CLOCK=1 -DGFX2D_PRESS_KNOB=1024 -DGFX2D_FIELDS_MAX=600 -DGFX2D_START_BACKEND=2 -DGFX_FRONTEND_INST=1'),
 'nmax-base':('game/gfx2dGame','gfx2dGame',True,'-DGFX2D_START_MODE=3 -DGFX2D_PRESS_CLOCK=1 -DGFX2D_PRESS_EXIT=1 -DGFX2D_START_BACKEND=2 -DGFX_FRONTEND_INST=0'),
 'nmax-inst':('game/gfx2dGame','gfx2dGame',True,'-DGFX2D_START_MODE=3 -DGFX2D_PRESS_CLOCK=1 -DGFX2D_PRESS_EXIT=1 -DGFX2D_START_BACKEND=2 -DGFX_FRONTEND_INST=1'),
}
def binary(job):
    app,name,perf,_=JOBS[job]
    return MAIN/'sw/apps'/app/('build_f4_'+job.replace('-','_'))/(name+'-O2'+('-perf' if perf else '')+'.bin')
def register(path):
    record=LOGS/'f4_artifacts.json'
    data=json.loads(record.read_text(encoding='utf-8')) if record.exists() else {}
    stat=path.stat();key=path.relative_to(ROOT).as_posix();old=data.get(key,{})
    if old.get('mtime_ns')!=stat.st_mtime_ns or old.get('size')!=stat.st_size:
        if old.get('sha256'):
            data.setdefault('versions',{})[old['sha256']]={**old,'path':key}
        data[key]={'size':stat.st_size,'mtime_ns':stat.st_mtime_ns,'sha256':hashlib.sha256(path.read_bytes()).hexdigest()}
        record.write_text(json.dumps(data,indent=2)+'\n',encoding='utf-8')
def build(job):
    app,name,perf,flags=JOBS[job]
    code="import pathlib;print(next(pathlib.Path.home().glob('opt/xpack-riscv-none-elf-gcc-13.4.0-*/bin/riscv-none-elf-gcc')))"
    prefix=subprocess.check_output(['wsl.exe','-e','python3','-c',code],text=True).strip()[:-3]
    args=['wsl.exe','-e','make','-C','Main/sw/apps/'+app,'-j8','OBJDIR=build_f4_'+job.replace('-','_'),
          'RISCV_BIN='+prefix,'EXTRA_CFLAGS='+flags]
    if perf:args+=['PERF=1']
    with (BUILD/(job+'_compile.log')).open('w',encoding='utf-8') as out:
        subprocess.run(args,cwd=ROOT,stdout=out,stderr=subprocess.STDOUT,check=True)
    register(binary(job));print('F4 BUILT '+job,flush=True)
def capture(job,program):
    program=program or job in ('tests-final','fallback')
    bit=MAIN/('sim/engine/frontend/build/f4/baseline.bit' if job=='fallback' else 'fpga/gfx2d_ti60/outflow/gfx2d_ti60.bit');register(bit);register(binary(job))
    log=LOGS/('f4_board_'+job+'.log')
    args=[sys.executable,str(MAIN/'tools/board/board_run.py'),'--bin',str(binary(job)),
          '--stop-line','done','--seconds',str(2400 if job.startswith('nmax') else 600),'--log',str(log)]
    args+=['--bit',str(bit)] if program else ['--no-program']
    print('F4 BOARD START '+job,flush=True)
    with (BUILD/(job+'_control.log')).open('w',encoding='utf-8') as out:
        subprocess.run(args,cwd=ROOT,stdout=out,stderr=subprocess.STDOUT,check=True)
    text=log.read_text(encoding='utf-8-sig',errors='replace')
    marker='instanceTest start' if job in ('tests','tests-final','fallback') else 'gfx2dGame MODES' if job.startswith('nmax') or job=='pressure-check' else 'f4Bench start' if job=='research-fixed' else 'frontendBench F1'
    start=text.rfind(marker)
    if start<0 or '# serial lost' in text:raise RuntimeError(job+' provenance/UART failed')
    body=text[start:]
    if not re.search(r'^fail 0\s*$',body,re.M) or not re.search(r'^done\s*$',body,re.M) or re.search(r'^FAIL |engine err ',body,re.M):
        raise RuntimeError(job+' failure: '+body[-1500:])
    if job in ('tests','tests-final','fallback'):
        pairs=re.findall(r'hw_crc=(\d+) sw_crc=(\d+)',body)
        if len(pairs)!=122 or any(a!=b for a,b in pairs) or ('FALLBACK identity preserved before publication' if job=='fallback' else 'MAX split=65535+4465 complete') not in body:
            raise RuntimeError('instanceTest coverage/CRC failed')
    if job=='research-fixed':
        if len(re.findall(r'^round[0-2] base=',body,re.M))!=9 or len(re.findall(r'verify=(172|2494|2860) vbatch=\1',body))!=3:
            raise RuntimeError('research reproduction coverage failed')
    if job.startswith('nmax'):
        subprocess.run([sys.executable,str(MAIN/'tools/check/check_sw9_nmax.py'),str(log),'--status','OK'],check=True,cwd=ROOT)
    print('F4 BOARD PASS '+job,flush=True)
def main():
    p=argparse.ArgumentParser();p.add_argument('--build',nargs='+',choices=JOBS);p.add_argument('--run',nargs='+',choices=JOBS)
    p.add_argument('--program',action='store_true');a=p.parse_args();BUILD.mkdir(parents=True,exist_ok=True);LOGS.mkdir(exist_ok=True)
    for job in a.build or []:build(job)
    for i,job in enumerate(a.run or []):capture(job,a.program and i==0)
if __name__=='__main__':main()
