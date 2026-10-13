#!/usr/bin/env python3
"""F5 串行板测：独立重建、唯一产物登记、正时序检查与 fail/done 消费。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import zlib
ROOT=Path(__file__).resolve().parents[3]
MAIN=ROOT/'Main';SIM=MAIN/'sim/engine/frontend'
from check_run import paths, session
BUILD,LOGS=paths('frontend','f5')
PROBE='-I../../game/gfx2dGame/src -DFRONTEND_MODE_LAST=14 -DFRONTEND_ITERATIONS=30 -DFRONTEND_EXECUTE=1'
JOBS={
 'tests':('test/pbTest','pbTest',False,''),
 'legacy':('test/instanceTest','instanceTest',False,''),
 'v2':('test/stripTest','stripTest',False,''),
 'research':('probe/f4Bench','f4Bench',False,''),
 'verify':('probe/frontendBench','frontendBench',False,PROBE+' -DFRONTEND_MODE_MASK=0xC01 -DFRONTEND_VERIFY'),
 'ab':('probe/frontendBench','frontendBench',False,PROBE+' -DFRONTEND_MODE_MASK=0xC01'),
 'combo':('probe/frontendBench','frontendBench',False,PROBE+' -DFRONTEND_SCENE_MASK=0x4011 -DFRONTEND_MODE_MASK=0x3800 -DFRONTEND_VERIFY'),
 'pressure':('game/gfx2dGame','gfx2dGame',True,'-DGFX2D_START_MODE=3 -DGFX2D_PRESS_CLOCK=1 -DGFX2D_PRESS_KNOB=1024 -DGFX2D_FIELDS_MAX=600 -DGFX2D_START_BACKEND=3 -DGFX_FRONTEND_BIN=1'),
 'nmax':('game/gfx2dGame','gfx2dGame',True,'-DGFX2D_START_MODE=3 -DGFX2D_PRESS_CLOCK=1 -DGFX2D_PRESS_EXIT=1 -DGFX2D_PRESS_AB_BIN=1 -DGFX2D_START_BACKEND=2 -DGFX_FRONTEND_BIN=1 -DGFX_PB_REF_CAP=32768'),
 'replay':('game/gfx2dGame','gfx2dGame',False,'-DSTG_FIELD=4 -DSTG_JUDGE_REPLAY=1 -DSW9_REPLAY_HW=1 -DG4_STRIP_RENDER=1 -DGFX_FRONTEND_BIN=1'),
}
def binary(job):
    app,name,perf,_=JOBS[job]
    return MAIN/'sw/apps'/app/('build_f5_'+job)/(name+'-O2'+('-perf' if perf else '')+'.bin')
def register(path):
    record=LOGS/'f5_artifacts.json'
    data=json.loads(record.read_text(encoding='utf-8')) if record.exists() else {}
    st=path.stat();key=path.relative_to(ROOT).as_posix();old=data.get(key,{})
    if old.get('mtime_ns')!=st.st_mtime_ns or old.get('size')!=st.st_size:
        if old.get('sha256'):data.setdefault('versions',{})[old['sha256']]={**old,'path':key}
        data[key]={'size':st.st_size,'mtime_ns':st.st_mtime_ns,'sha256':hashlib.sha256(path.read_bytes()).hexdigest()}
        record.write_text(json.dumps(data,indent=2)+'\n',encoding='utf-8')
def build(job):
    app,name,perf,flags=JOBS[job]
    code="import pathlib;print(next(pathlib.Path.home().glob('opt/xpack-riscv-none-elf-gcc-13.4.0-*/bin/riscv-none-elf-gcc')))"
    prefix=subprocess.check_output(['wsl.exe','-e','python3','-c',code],text=True).strip()[:-3]
    args=['wsl.exe','-e','make','-C','Main/sw/apps/'+app,'-j8','OBJDIR=build_f5_'+job,
          'RISCV_BIN='+prefix,'EXTRA_CFLAGS='+flags]
    if perf:args+=['PERF=1']
    with (BUILD/(job+'_compile.log')).open('w',encoding='utf-8') as out:
        subprocess.run(args,cwd=ROOT,stdout=out,stderr=subprocess.STDOUT,check=True)
    register(binary(job));print('F5 BUILT '+job,flush=True)
def synthesis():
    path=MAIN/'fpga/gfx2d_ti60/outflow'
    text=(path/'gfx2d_ti60.timing.rpt').read_text()
    begin=text.index('Setup (Max) Clock Relationship');end=text.index('NOTE: Values',begin)
    slacks=[float(x) for x in re.findall(r'^\S+\s+\S+\s+[-\d.]+\s+([-\d.]+)\s+\(',text[begin:end],re.M)]
    if not slacks or min(slacks)<0:raise RuntimeError('F5 禁止部署负时序位流: '+str(slacks))
    bit=path/'gfx2d_ti60.bit'
    if max(p.stat().st_mtime_ns for p in (MAIN/'rtl/engine').glob('*.v'))>bit.stat().st_mtime_ns:
        raise RuntimeError('RTL 晚于位流，先重建')
    for ext in ['place.rpt','timing.rpt','hier_util.rpt']:
        destination=LOGS/('f5_final_main_'+ext)
        report=(path/('gfx2d_ti60.'+ext)).read_bytes()
        if not destination.exists() or destination.read_bytes()!=report:destination.write_bytes(report)
    register(bit)
    return bit
def capture(job,program):
    bit=synthesis();register(binary(job))
    log=LOGS/('f5_board_'+job+'.log')
    args=[sys.executable,str(MAIN/'tools/board/board_run.py'),'--port',os.environ.get('GFX_CHECK_PORT','COM11'),'--bin',str(binary(job)),
          '--stop-line','done','--seconds',str(10800 if job=='replay' else 3600 if job=='nmax' else 1200),'--log',str(log)]
    args+=['--bit',str(bit)] if program else ['--no-program']
    print('F5 BOARD START '+job,flush=True)
    with (BUILD/(job+'_control.log')).open('w',encoding='utf-8') as out:
        subprocess.run(args,cwd=ROOT,stdout=out,stderr=subprocess.STDOUT,check=True)
    body=log.read_text(encoding='utf-8-sig',errors='replace')
    marker='pbTest start' if job=='tests' else 'instanceTest start' if job=='legacy' else 'stripTest G3' if job=='v2' else 'f4Bench start' if job=='research' else 'frontendBench F1' if job in ['verify','ab','combo'] else 'gfx2dGame '
    start=body.rfind(marker)
    if start<0 or '# serial lost' in body:raise RuntimeError(job+' provenance/UART failed')
    body=body[start:]
    if not re.search(r'^fail 0\s*$',body,re.M) or not re.search(r'^done\s*$',body,re.M) or re.search(r'^FAIL |engine err ',body,re.M):
        raise RuntimeError(job+' failed: '+body[-2500:])
    if job=='tests':
        pairs=re.findall(r'hw_crc=(\d+) sw_crc=(\d+)',body)
        assert len(pairs)==74 and all(a==b for a,b in pairs),'PB case coverage'
        assert len(re.findall(r'^pberror test=',body,re.M))==4,'PB error coverage'
    if job in ['verify','combo']:
        rows=[r.split(',') for r in body.splitlines() if r.startswith('fverify,')]
        assert len(rows)==(153 if job=='verify' else 27) and all(r[4]=='0' and r[5]==r[6] for r in rows),'game pixel verification'
    if job=='nmax':
        rows=re.findall(r'^nmax result OK n_max (\d+) knob (\d+) ft_max (\d+) ft_p99 (\d+)',body,re.M)
        assert len(rows)==4 and 'f5 backend TEX end' in body and 'f5 backend PB begin' in body,'same-BIN matrix'
        diag=re.search(r'^diag fifo_min (\d+).* zero (\d+) under (\d+).* faults (\d+) swap_retry (\d+)',body,re.M)
        assert diag and int(diag[1])>=160 and all(int(x)==0 for x in diag.groups()[1:]),'display diagnostics'
        (LOGS/'f5_nmax.json').write_text(json.dumps({'TEX':rows[:2],'PB':rows[2:],'ref_cap':32768,'fifo_min':int(diag[1]),'same_binary':True},indent=2)+'\n')
    print('F5 BOARD PASS '+job,flush=True)

def collect(jobs=None):
    """消费原始串口、长时确认和逐帧身份；不重新部署或计算产物摘要。"""
    from check_g4_integration import compare
    registry=json.loads((LOGS/'f5_artifacts.json').read_text(encoding='utf-8'))
    result={}
    selected=list(JOBS) if not jobs else jobs
    for job in selected:
        path=LOGS/('f5_board_'+job+'.log')
        whole=path.read_text(encoding='utf-8-sig',errors='replace')
        body=whole
        marker='pbTest start' if job=='tests' else 'instanceTest start' if job=='legacy' else 'stripTest G3' if job=='v2' else 'f4Bench start' if job=='research' else 'frontendBench F1' if job in ['verify','ab','combo'] else 'gfx2dGame '
        start=body.rfind(marker)
        if start<0 or '# serial lost' in body:raise RuntimeError(job+': 来源或串口不完整')
        body=body[start:]
        if not re.search(r'^fail 0\s*$',body,re.M) or not re.search(r'^done\s*$',body,re.M) or re.search(r'^(FAIL |engine err |err )',body,re.M):
            raise RuntimeError(job+': 未正常完成')
        result[job]={'log':path.relative_to(ROOT).as_posix(),'fail':0}
        alternate=re.search(r'^# F5 artifact (.+)$',whole,re.M)
        deployed=(ROOT/alternate[1]).resolve() if alternate else binary(job)
        key=deployed.relative_to(ROOT.resolve()).as_posix();identity=registry[key];st=deployed.stat()
        assert (st.st_size,st.st_mtime_ns)==(identity['size'],identity['mtime_ns']),job+': 未登记的产物改动'
        control_source=BUILD/(job+'_control.log')
        if not control_source.exists():control_source=LOGS/('f5_board_'+job+'_control.log')
        control=control_source.read_bytes()
        control_text=control.decode('utf-8',errors='replace')
        assert f"{identity['size']} bytes written at address" in control_text and 'Target successfully examined' in control_text,job+': 部署缺字节数或目标识别'
        saved=LOGS/('f5_board_'+job+'_control.log')
        if not saved.exists() or saved.read_bytes()!=control:saved.write_bytes(control)
        assert saved.read_bytes()==control,job+': 控制日志归档不一致'
        result[job]['artifact']=key
        result[job]['control_log']=saved.relative_to(ROOT).as_posix()
        result[job]['transport']='JTAG DDR' if alternate else 'UART'
        if alternate:
            metadata=json.loads((LOGS/('f5_jtag_'+job+'.json')).read_text(encoding='utf-8'))
            payload=path.read_bytes().split(b'\n',2)[2]
            assert metadata['artifact']==key and len(payload)==metadata['bytes'] and zlib.crc32(payload)==metadata['crc32']
            assert metadata['header'][3]==1 and not metadata['performance_evidence']
        if job=='tests':
            pairs=re.findall(r'hw_crc=(\d+) sw_crc=(\d+)',body)
            assert len(pairs)==74 and all(a==b for a,b in pairs)
            assert len(re.findall(r'^pberror test=',body,re.M))==4
            result[job]['crc_cases']=len(pairs)
        if job in ['verify','combo']:
            rows=[r.split(',') for r in body.splitlines() if r.startswith('fverify,')]
            assert len(rows)==(153 if job=='verify' else 27) and all(r[4]=='0' and r[5]==r[6] for r in rows)
            result[job]['pixel_cases']=len(rows)
        if job in ['verify','ab','combo']:
            diag=re.search(r'^fdiag,(\d+),(\d+),(\d+),(\d+),(\d+)',body,re.M)
            assert diag and int(diag[2])>=160 and all(int(v)==0 for v in diag.groups()[2:])
            result[job]['fifo_min']=int(diag[2])
        if job=='research':
            assert re.findall(r'^ mbin=(\d+)',body,re.M)==['778','5187','5794']
        if job=='ab':
            from statistics import median
            header=next(x for x in body.splitlines() if x.startswith('fh,')).split(',')
            rows=[dict(zip(header,map(int,['0',*x.split(',')[1:]]))) for x in body.splitlines() if x.startswith('fcsv,')]
            assert len(rows)==153
            bins=[list(map(int,x.split(',')[1:])) for x in body.splitlines() if x.startswith('fbin,')]
            assert len(bins)==51
            states=[]
            for scene in range(17):
                record={'scene':scene,'modes':{}}
                for mode in [0,10,11]:
                    samples=[r for r in rows if r['scene']==scene and r['mode']==mode]
                    assert sorted(r['repeat'] for r in samples)==[0,1,2] and all(r['n']==30 and r['errors']==0 for r in samples)
                    record['modes'][str(mode)]={'raw':samples,**{k:median(r[k] for r in samples) for k in ['p50','p99','max','exec_avg','exec_max','gpu_busy_avg']}}
                    record['modes'][str(mode)]['cpu_repeat_span_percent']=100*(max(r['p50'] for r in samples)/min(r['p50'] for r in samples)-1)
                for repeat in range(3):
                    same=[r for r in rows if r['scene']==scene and r['repeat']==repeat]
                    assert all(len({r[k] for r in same})==1 for k in ['visible_enemy','logical_draws','eq_copy','eq_alpha','eq_add','eq_const','eq_mul','eq_st'])
                pb=[r for r in bins if r[0]==scene]
                assert len(pb)==3 and all(r[3:6]==[132096,1024,8192] for r in pb)
                record['pb_per_frame']=[dict(zip(['instances','refs','bin_cycles','pb_rd_beat','pb_wr_beat'],[v/30 for v in r[6:]])) for r in pb]
                states.append(record)
            (LOGS/'f5_cost_results.json').write_text(json.dumps({'states':states,'iterations':30,'repeats':3,'same_binary':True},indent=2)+'\n',encoding='utf-8')
            result[job]['states']=17
        if job=='nmax':
            configs=[x.strip() for x in body.splitlines() if x.startswith('nmax cfg ')]
            expected='nmax cfg N0=64 STEP_MIN=16 T_PROBE=120 T_CONFIRM=3600 SETTLE_MIN=300 SETTLE_FRAMES=30 SETTLE_ABS=2 SETTLE_MAX=600 CONFIRM_RETRY=3 KNOB_MAX=16000 F=1666667'
            assert configs==[expected]*4,'搜索参数或弹型覆盖改变'
            phases=body.split('f5 backend TEX end')
            assert len(phases)==2 and 'f5 backend PB begin' in phases[1]
            matrix={}
            global_diag=[x for x in body.splitlines() if x.startswith('diag ')]
            assert len(global_diag)==1
            for name,phase in zip(['TEX','PB'],phases):
                phase_lines=[x.strip() for x in phase.splitlines()]
                assert all(phase_lines.count('nmax load '+kind)==1 for kind in ['16x16','8x8'])
                rows=[x for x in phase_lines if x.startswith('nmax result ')]
                assert len(rows)==2 and all(x.startswith('nmax result OK ') for x in rows)
                matrix[name]=[]
                for kind,row in zip(['16x16','8x8'],rows):
                    m=re.fullmatch(r'nmax result OK n_max (\d+) knob (\d+) ft_max (\d+) ft_p99 (\d+) probes (\d+) timeouts (\d+)',row)
                    assert m,row
                    values=dict(zip(['n_max','knob','ft_max','ft_p99','probes','timeouts'],map(int,m.groups())))
                    prior=phase_lines[:phase_lines.index(row)]
                    confirm=[x for x in prior if x.startswith('nmax probe ') and ' confirm knob ' in x][-1]
                    assert all(s in confirm for s in ['knob '+m[2]+' ','fields 3600 ','drops 0 ','act_min '+m[1]+' ',' pass']),confirm
                    csv_header=next(x for x in phase_lines if x.startswith('csvh,')).split(',')
                    csv_rows=[dict(zip(csv_header,x.split(','))) for x in prior if x.startswith('csv,')][-2:]
                    assert len(csv_rows)==2 and all(int(r['fps'])==60 and int(r['drop'])==0 for r in csv_rows)
                    previous,last=csv_rows
                    traffic={field:((int(last[field])-int(previous[field]))&0xffffffff)/60 for field in
                             ['hw_rd_burst_cum','hw_wr_burst_cum','hw_rd_beat_cum','hw_wr_beat_cum']}
                    sample={k:int(last[k]) for k in ['t','n_active','n_bullet','cpu_avg_cyc','engine_busy_avg_cyc','window_fields']}
                    sample.update(bottleneck=last['bottleneck'],pace=last['pace'],per_frame_bus=traffic)
                    matrix[name].append({'kind':kind,**values,'confirmation':confirm,'last_confirm_window':sample})
                # 既有独立消费者分别复核两阶段；扫描诊断覆盖完整同BIN运行。
                checked=BUILD/('nmax_'+name+'_checked.log')
                checked.write_text('\n'.join(['gfx2dGame MODES',*[x for x in phase_lines if not x.startswith('diag ') and x not in ['fail 0','done']],global_diag[0],'fail 0','done'])+'\n',encoding='utf-8')
                for checker,flags in [('check_sw9_nmax.py',['--status','OK']),('check_sw9_perf.py',['--fields-min','3600'])]:
                    p=subprocess.run([sys.executable,str(MAIN/'tools/check'/checker),str(checked),*flags],capture_output=True,text=True,encoding='utf-8')
                    (LOGS/('f5_nmax_'+name+'_'+checker[:-3]+'.log')).write_text(p.stdout+p.stderr,encoding='utf-8')
                    if p.returncode:raise RuntimeError(name+' '+checker+': '+p.stdout+p.stderr)
            result[job]['matrix']=matrix
            result[job]['same_binary']=True
            diag=re.search(r'^diag fifo_min (\d+).* zero (\d+) under (\d+).* faults (\d+) swap_retry (\d+)',body,re.M)
            assert diag and int(diag[1])>=160 and all(int(x)==0 for x in diag.groups()[1:])
            result[job]['fifo_min']=int(diag[1])
            result[job]['gain_percent']=[100*(pb['n_max']/tex['n_max']-1) for tex,pb in zip(matrix['TEX'],matrix['PB'])]
        if job=='pressure':
            p=subprocess.run([sys.executable,str(MAIN/'tools/check/check_sw9_perf.py'),str(path),
                              '--fields-min','600','--warmup-rows','1','--steady-drops-max','0'],capture_output=True,text=True,encoding='utf-8')
            (LOGS/'f5_pressure_check.log').write_text(p.stdout+p.stderr,encoding='utf-8')
            if p.returncode:raise RuntimeError('pressure: '+p.stdout+p.stderr)
            result[job]['summary']=next(x for x in body.splitlines() if x.startswith('csvsum,'))
        if job=='replay':result[job].update(compare(LOGS/'f5_replay_host_flat.log',path,False,True))
    result['artifacts_manifest']=(LOGS/'f5_artifacts.json').relative_to(ROOT).as_posix()
    result['bitstream']='Main/fpga/gfx2d_ti60/outflow/gfx2d_ti60.bit'
    complete=set(selected)==set(JOBS)
    result['scope']='full' if complete else 'partial'
    (LOGS/('f5_board_results.json' if complete else 'f5_board_partial_results.json')).write_text(json.dumps(result,indent=2,ensure_ascii=False)+'\n',encoding='utf-8')
    print('F5 BOARD COLLECT PASS '+result['scope']+' jobs='+','.join(selected),flush=True)

def main():
    p=argparse.ArgumentParser();p.add_argument('--build',nargs='+',choices=JOBS);p.add_argument('--run',nargs='+',choices=JOBS)
    p.add_argument('--program',action='store_true');p.add_argument('--collect',nargs='*',choices=JOBS);p.add_argument('--progress',action='store_true');a=p.parse_args();BUILD.mkdir(parents=True,exist_ok=True);LOGS.mkdir(exist_ok=True)
    for job in a.build or []:build(job)
    for i,job in enumerate(a.run or []):capture(job,a.program and i==0)
    if a.collect is not None:collect(a.collect)
    if a.progress:
        from check_g4_integration import replay_progress
        replay_progress(LOGS/'f5_replay_host_flat.log',LOGS/'f5_board_replay.log')
if __name__=='__main__':main()
