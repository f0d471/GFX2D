#!/usr/bin/env python3
"""F1～F3 后续板测队列：独占串口、严格检查、恢复主线位流。"""
import argparse
import hashlib
import json
import os
import re
from pathlib import Path
import subprocess
import sys
import time
from check_frontend import board_log, VARIANTS
from run_frontend_board import MAIN,ROOT,LOGS,BUILD,register
APP=MAIN/'sw/apps/game/gfx2dGame'
MARK='# --- 程序已加载，以下才是它的输出 ---'
RESUME=False
CACHE_ACTIVE=False

def segment(path):
    t=Path(path).read_text(encoding='utf-8-sig',errors='replace')
    if MARK not in t:raise RuntimeError('missing deployment boundary: '+str(path))
    t=t.split(MARK,1)[1]
    if '# serial lost' in t or '\ndone\n' not in t:raise RuntimeError('incomplete board capture: '+str(path))
    return t
def launch(args,name):
    print('START',name,flush=True)
    with (BUILD/(name+'_runner.log')).open('w') as out:
        r=subprocess.run([str(x) for x in args],cwd=ROOT,stdout=out,stderr=subprocess.STDOUT)
    if r.returncode:raise RuntimeError(name+': '+(BUILD/(name+'_runner.log')).read_text(encoding='utf-8-sig',errors='replace')[-3000:])
    print('CAPTURED',name,flush=True)
def bit_check(path):
    ref=json.loads((MAIN/'sim/engine/g4/logs/artifacts.json').read_text())
    key=path.relative_to(ROOT).as_posix()
    if hashlib.sha256(path.read_bytes()).hexdigest()!=ref[key]['sha256']:raise RuntimeError('bit identity mismatch '+key)
def ready(name,binary):
    if not RESUME or not binary.exists():return False
    try:
        segment(LOGS/(name+'.log'))
        registry=json.loads((LOGS/'artifacts.json').read_text())
        ref=registry[binary.relative_to(ROOT).as_posix()];st=binary.stat()
        return ref['size']==st.st_size and ref['mtime_ns']==st.st_mtime_ns
    except (OSError,ValueError,KeyError,RuntimeError):return False
def capture(binary,name,seconds=600,bit=None,inputs=None):
    if ready(name,binary):
        print('REUSE',name,flush=True);return segment(LOGS/(name+'.log'))
    register(binary)
    args=[sys.executable,MAIN/'tools/board/board_run.py','--bin',binary,'--seconds',seconds,
          '--stop-on','done','--log',LOGS/(name+'.log')]
    args+=['--bit',bit] if bit else ['--no-program']
    if inputs:args+=['--input-script',inputs,'--input-script-after','mode 2 enter']
    launch(args,name);return segment(LOGS/(name+'.log'))
def probe(name,flags,seconds=3600):
    binary=MAIN/'sw/apps/probe/frontendBench'/('build_'+name)/'frontendBench-O2.bin'
    if ready(name,binary):
        print('REUSE',name,flush=True);return segment(LOGS/(name+'.log'))
    launch([sys.executable,MAIN/'tools/check/run_frontend_board.py','--tag',name,'--no-program',
            '--flags='+flags,'--seconds',seconds],name)
    return segment(LOGS/(name+'.log'))
def game(name,flags,prefix,perf=False,bit=None,inputs=None):
    obj='build_frontend_'+name
    binary=APP/obj/('gfx2dGame-O2'+('-perf' if perf else '')+'.bin')
    if ready(name,binary):
        print('REUSE',name,flush=True);return segment(LOGS/(name+'.log'))
    args=['wsl.exe','-e','make','-C','Main/sw/apps/game/gfx2dGame','-j8','OBJDIR='+obj,
          'RISCV_BIN='+prefix,'EXTRA_CFLAGS='+flags]
    if perf:args+=['PERF=1']
    launch(args,name+'_compile')
    return capture(APP/obj/('gfx2dGame-O2'+('-perf' if perf else '')+'.bin'),name,600,bit,inputs)
def sample_check(text,reference,red=False):
    rows=[x for x in text.splitlines() if x.startswith('g ')]
    if len(rows)!=17 or 'fail 0' not in text:raise RuntimeError('bad replay sample count')
    bad=[int(x.split()[1]) for x in rows if x!=reference[int(x.split()[1])]]
    if bool(bad)!=red:raise RuntimeError('unexpected CRC sample result '+str(bad))
    return {'samples':17,'mismatches':bad}
def main():
    global RESUME,CACHE_ACTIVE
    p=argparse.ArgumentParser();p.add_argument('--wait-log');p.add_argument('--resume',action='store_true');a=p.parse_args()
    RESUME=a.resume
    BUILD.mkdir(parents=True,exist_ok=True);LOGS.mkdir(parents=True,exist_ok=True)
    if a.wait_log:
        end=time.monotonic()+9000
        while time.monotonic()<end:
            try:
                t=Path(a.wait_log).read_text(encoding='utf-8-sig',errors='replace')
                if MARK in t and '\ndone\n' in t.split(MARK,1)[1]:break
            except OSError:pass
            time.sleep(2)
        else:raise RuntimeError('waiting for cost matrix timed out')
        rows=board_log(a.wait_log,list(range(10)),[0,4,10])
        Path(a.wait_log).with_suffix('.json').write_text(json.dumps(rows,indent=2)+'\n')
    discovery="import pathlib; print(next(pathlib.Path.home().glob('opt/xpack-riscv-none-elf-gcc-13.4.0-*/bin/riscv-none-elf-gcc')))"
    prefix=subprocess.check_output(['wsl.exe','-e','python3','-c',discovery],cwd=ROOT,text=True).strip()[:-3]
    mainbit=MAIN/'fpga/gfx2d_ti60/outflow/gfx2d_ti60.bit'
    cachebit=MAIN/'sim/engine/g4/build/cache16/gfx2d_ti60/outflow/gfx2d_ti60.bit'
    bit_check(mainbit);bit_check(cachebit)
    launch([sys.executable,MAIN/'tools/board/board_run.py','--bit',mainbit,'--check-only'],'main_bit_start')
    results={}
    text=probe('all_outputs_4k','-DFRONTEND_VERIFY=1 -DFRONTEND_EXECUTE=1 -DFRONTEND_ITERATIONS=1',1800)
    rows=[list(map(int,x.split(',')[1:])) for x in text.splitlines() if x.startswith('fverify,')]
    expected={(s,m,r) for s in range(17) for m in range(10) for r in range(3)}
    if len(rows)!=510 or {tuple(x[:3]) for x in rows}!=expected or any(x[3] or x[4]!=x[5] for x in rows) or 'fail 0' not in text:
        raise RuntimeError('same-frame CRC/PERF/padding failed')
    output_rows=board_log(LOGS/'all_outputs_4k.log',list(range(10)),list(range(17)),min_frames=1)
    (LOGS/'all_outputs_4k.json').write_text(json.dumps(output_rows,indent=2)+'\n')
    results['same_frame']={'groups':510,'states':17,'modes':10,'crc_perf_padding_bad':0}
    diag=[list(map(int,x.split(',')[1:])) for x in text.splitlines() if x.startswith('fdiag,')]
    if not diag or any(diag[-1][2:]):raise RuntimeError('same-frame scan/IRQ diagnostics failed')
    results['same_frame']['diag']=diag[-1]
    text=probe('f1_sets_isolated_4k','-DFRONTEND_SETS=1 -DG4_STRIP_COMPARE=1 -DFRONTEND_ISOLATE=1',600)
    rows=[list(map(int,x.split(',')[1:])) for x in text.splitlines() if x.startswith('fsets,')]
    expected={(same,cold,kind,r) for same in range(2) for cold in range(2) for kind in range(3) for r in range(3)}
    if len(rows)!=len(expected) or {tuple(x[:4]) for x in rows}!=expected or any(x[-1] or x[4]!=1000 for x in rows) or 'fail 0' not in text:
        raise RuntimeError('set mechanism matrix failed')
    results['sets']={'groups':len(expected),'bad':0}
    isolate=' -DG4_STRIP_COMPARE=1 -DFRONTEND_ISOLATE=1 '
    for tag,extra,modes in [('f1_stages_4k','-DGFX_FRONTEND_PROFILE=1 -DFRONTEND_MODE_LAST=2',[0,1]),
                            ('f1_detail_4k','-DGFX_FRONTEND_PROFILE=1 -DGFX_FRONTEND_DETAIL=1 -DFRONTEND_MODE_FIRST=1 -DFRONTEND_MODE_LAST=2',[1]),
                            ('f1_store_4k','-DGFX_FRONTEND_PROFILE=1 -DGFX_FRONTEND_DETAIL=1 -DGFX_FRONTEND_DETAIL_STORE=1 -DFRONTEND_MODE_FIRST=1 -DFRONTEND_MODE_LAST=2',[1])]:
        probe(tag,'-DFRONTEND_SCENE_MASK=0x411 '+extra+isolate)
        data=board_log(LOGS/(tag+'.log'),modes,[0,4,10]);(LOGS/(tag+'.json')).write_text(json.dumps(data,indent=2)+'\n')
    flags='-DFRONTEND_SCENE_MASK=0x411 -DFRONTEND_MODE_LAST=5'+isolate
    probe('f3_cpu_4k',flags,5400)
    data=board_log(LOGS/'f3_cpu_4k.log',list(range(5)),[0,4,10]);(LOGS/'f3_cpu_4k.json').write_text(json.dumps(data,indent=2)+'\n')
    binary=MAIN/'sw/apps/probe/frontendBench/build_f3_cpu_4k/frontendBench-O2.bin'
    CACHE_ACTIVE=True
    capture(binary,'f3_cpu_16k',5400,cachebit)
    data=board_log(LOGS/'f3_cpu_16k.log',list(range(5)),[0,4,10]);(LOGS/'f3_cpu_16k.json').write_text(json.dumps(data,indent=2)+'\n')
    launch([sys.executable,MAIN/'tools/board/board_run.py','--bit',mainbit,'--check-only'],'main_bit_restore')
    CACHE_ACTIVE=False
    text=probe('surface_guard_4k','-DFRONTEND_DRIVER_ONLY=1',60)
    if 'fail 0' not in text or 'driver busy SURF rejected ok' not in text or 'driver idle SURF restore ok' not in text:
        raise RuntimeError('surface lifetime guard failed')
    results['surface_guard']={'busy_rejected':True,'idle_restored':True}
    reference={int(x.split()[1]):x for x in (LOGS/'host_flat.log').read_text(encoding='utf-8-sig').splitlines() if x.startswith('g ')}
    common='-DSTG_FIELD=4 -DSTG_JUDGE_REPLAY=1 -DSW9_REPLAY_HW=1 -DG4_SAMPLE_STEP=600 '
    sizes={}
    for index,name in enumerate(('strip','planned','template','both','static','prepass','combo')):
        text=game('samples_'+name,common+VARIANTS[name],prefix)
        results[name]=sample_check(text,reference)
        elf=APP/('build_frontend_samples_'+name)/'gfx2dGame-O2.elf'
        raw=subprocess.check_output(['wsl.exe','-e',prefix+'size',elf.relative_to(ROOT).as_posix()],cwd=ROOT,text=True)
        values=list(map(int,raw.splitlines()[1].split()[:3]))
        sizes[name]={'text_rodata':values[0],'data':values[1],'bss':values[2]}
    (LOGS/'code_sizes.json').write_text(json.dumps(sizes,indent=2)+'\n',encoding='utf-8')
    for name,extra,red in [('cache_bad',' -DGFX_FRONTEND_SKIP_INVALIDATE=1',True),('cache_good','',False)]:
        text=game(name,common+VARIANTS['prepass']+' -DGFX_FRONTEND_CACHE_PROBE=1'+extra,prefix)
        results[name]=sample_check(text,reference,red)
    flags='-DGFX2D_START_MODE=3 -DGFX2D_PRESS_KNOB=1984 -DGFX2D_PRESS_PREWARM_STEPS=1200 -DGFX2D_FRAMES=1200 '
    for backend,extra,name in [(2,'-DGFX_FRONTEND_BACKGROUND=2','tex'),(3,'','strip'),(3,'-DGFX_FRONTEND_BACKGROUND=2','prepass')]:
        text=game('perf_pipeline_'+name,flags+f'-DGFX2D_START_BACKEND={backend} '+extra,prefix,perf=True)
        if 'fail 0' not in text or 'csvsum,' not in text:raise RuntimeError('production perf incomplete '+name)
        results['perf_'+name]={'complete':True}
    events=[]
    for start,modekey in [(1000,None),(11000,128),(23000,512)]:
        if modekey:events += [(start-2000,modekey),(start-1000,0)]
        for k in range(4):events += [(start+2000*k,64),(start+2000*k+1000,0)]
    events += [(33000,256),(34000,0)]
    inputs=','.join(f'{t}:{key}' for t,key in events)
    text=game('switch_uart','-DGFX2D_START_BACKEND=3 -DGFX2D_FIELDS_MAX=4200 -DGFX2D_PRESS_KNOB=64 -DGFX_FRONTEND_BACKGROUND=2 -DGFX_FRONTEND_PLAN=1 -DGFX_FRONTEND_TEMPLATE=1',prefix,perf=True,inputs=inputs)
    if 'fail 0' not in text or 'csvsum,' not in text:raise RuntimeError('mode/backend switch failed')
    backends=set(re.findall(r'backend (\S+) frame',text));modes=set(re.findall(r'mode ([123]) (?:enter )?frame',text))
    if not {'sw','hw','hw-tex','hw-strip'}<=backends or not {'1','2','3'}<=modes:raise RuntimeError('switch coverage incomplete')
    results['switch']={'complete':True,'backends':sorted(backends),'modes':sorted(modes)}
    text=game('prepass_failure_pipeline','-DGFX2D_START_MODE=2 -DGFX2D_START_BACKEND=3 -DGFX_FRONTEND_BACKGROUND=2 -DGFX2D_FRAMES=60 -DINJECT_FRONTEND_PREPASS_FAILURE=1',prefix,perf=True)
    if 'engine err code 1' not in text or 'csvsum,' not in text:raise RuntimeError('prepass failure did not reach consumer')
    sums=[list(map(int,x.split(',')[1:])) for x in text.splitlines() if x.startswith('csvsum,')]
    if not sums or sums[-1][0]!=60 or sums[-1][1]<=0 or sums[-1][2]!=sums[-1][1]:raise RuntimeError('failed frame was presented')
    results['prepass_failure']={'expected_error':True,'frames':60,'swaps':0,'fields':sums[-1][1]}
    (LOGS/'board_results.json').write_text(json.dumps(results,indent=2)+'\n')
    print('FRONTEND BOARD QUEUE PASS',flush=True)
if __name__=='__main__':
    try:main()
    except Exception as e:
        print('FAIL:',e,file=sys.stderr,flush=True)
        if CACHE_ACTIVE:
            mainbit=MAIN/'fpga/gfx2d_ti60/outflow/gfx2d_ti60.bit'
            bit_check(mainbit)
            launch([sys.executable,MAIN/'tools/board/board_run.py','--bit',mainbit,'--check-only'],'main_bit_restore_after_failure')
        raise
