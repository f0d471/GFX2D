#!/usr/bin/env python3
"""F1～F3 的源码、宿主回放、变异与板测日志检查。"""
import argparse
import csv
import io
import json
import os
import statistics
from pathlib import Path
import subprocess
import sys

MAIN=Path(__file__).resolve().parents[2]
ROOT=MAIN.parent
SIM=MAIN/'sim/engine/frontend'
from check_run import paths
BUILD,LOGS=paths('frontend','frontend')
APP=MAIN/'sw/apps/game/gfx2dGame'
LIB=MAIN/'sw/lib'
VARIANTS={'flat':'','strip':'-DG4_STRIP_RENDER=1',
 'planned':'-DG4_STRIP_RENDER=1 -DGFX_FRONTEND_PLAN=1',
 'template':'-DG4_STRIP_RENDER=1 -DGFX_FRONTEND_TEMPLATE=1',
 'both':'-DG4_STRIP_RENDER=1 -DGFX_FRONTEND_PLAN=1 -DGFX_FRONTEND_TEMPLATE=1',
 'static':'-DG4_STRIP_RENDER=1 -DGFX_FRONTEND_BACKGROUND=1',
 'prepass':'-DG4_STRIP_RENDER=1 -DGFX_FRONTEND_BACKGROUND=2',
 'combo':'-DG4_STRIP_RENDER=1 -DGFX_FRONTEND_BACKGROUND=2 -DGFX_FRONTEND_PLAN=1 -DGFX_FRONTEND_TEMPLATE=1'}

def run(name,args,cwd=ROOT,env=None,red=False):
    LOGS.mkdir(parents=True,exist_ok=True)
    with (LOGS/(name+'.log')).open('w') as out:
        r=subprocess.run([str(a) for a in args],cwd=cwd,env=env,stdout=out,stderr=subprocess.STDOUT)
    text=(LOGS/(name+'.log')).read_text(encoding='utf-8-sig',errors='replace')
    if (r.returncode==0)==red:raise RuntimeError(f'{name}: unexpected exit {r.returncode}: '+text[-2000:])
    return text

def models():
    BUILD.mkdir(parents=True,exist_ok=True)
    common=[LIB/'gfx'/x for x in ('list.c','strip.c','strip_plan.c','frame.c','check.c')]+[LIB/'core/format.c']
    for name in ('planned','frame','pixels'):
        exe=BUILD/('test_'+name)
        extra=[LIB/'gfx/exec_sw.c'] if name=='pixels' else []
        run(name+'_compile',['gcc','-O1','-g','-std=c11','-Wall','-Wextra','-Werror',
                            '-fsanitize=address,undefined','-I'+str(LIB),SIM/('test_'+name+'.c'),*common,*extra,'-o',exe])
        run(name,[exe])
    for macro in ('PLAN_ORDER','PLAN_TAIL','PLAN_EARLY_PUBLISH','TEMPLATE_SOURCE','TEMPLATE_EPOCH','TEMPLATE_GENERATION','TEMPLATE_PATTERN','FRAME_REUSE',
                  'STRIP_DROP_FIELD','STRIP_FEEDBACK_CHECK','PREPASS_INITIALIZATION'):
        exe=BUILD/('mutant_'+macro)
        source='test_frame.c' if macro=='FRAME_REUSE' else 'test_pixels.c' if macro=='PREPASS_INITIALIZATION' else 'test_planned.c'
        extra=[LIB/'gfx/exec_sw.c'] if macro=='PREPASS_INITIALIZATION' else []
        run(macro+'_compile',['gcc','-O1','-g','-std=c11','-Wall','-Wextra','-Werror',
                             '-fsanitize=address,undefined','-DINJECT_'+macro,'-I'+str(LIB),SIM/source,*common,*extra,'-o',exe])
        run(macro,[exe],red=True)
    print('FRONTEND MODELS PASS',flush=True)

def legacy():
    BUILD.mkdir(parents=True,exist_ok=True)
    common=[LIB/'gfx'/x for x in ('list.c','strip.c','strip_plan.c','check.c','exec_sw.c')]+[LIB/'core/format.c']
    for name,source in [('legacy_strips',MAIN/'sim/engine/stripctl/test_strips.c'),
                        ('legacy_errors',MAIN/'sim/engine/stripctl/test_errors.c'),
                        ('legacy_batch',MAIN/'sim/engine/g4/test_batch.c')]:
        exe=BUILD/name
        run(name+'_compile',['gcc','-O1','-g','-std=c11','-Wall','-Wextra','-Werror',
                            '-fsanitize=address,undefined','-I'+str(LIB),source,*common,'-o',exe])
        run(name,[exe])
    print('FRONTEND LEGACY V1/V2 SOFTWARE PASS',flush=True)

def pipeline_model():
    game=MAIN/'sw/apps/game/gfx2dGame/src'
    sources=[SIM/'test_pipeline.c',MAIN/'sw/services/gfx/renderer.c',MAIN/'sw/bsp/memmap.c',
             *[LIB/'gfx'/x for x in ('list.c','strip.c','strip_plan.c','frame.c','check.c','exec_sw.c','census.c','instances.c','pb.c')],LIB/'core/format.c']
    flags=['-m32','-O1','-g','-std=c11','-Wall','-Wextra','-Werror','-D__riscv','-DSW9_PERF=1','-DGFX_FRONTEND_BACKGROUND=2',
           '-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-I'+str(LIB),'-I'+str(MAIN/'sw/bsp/include'),'-I'+str(MAIN/'sw/ports/myriad'),'-I'+str(game),'-I'+str(MAIN/'sw/services/gfx')]
    for name,defines,red in [('pipeline_model',[],False),('pipeline_early_release',['-DINJECT_PIPELINE_EARLY_RELEASE'],True),('pipeline_instance_reset',['-DGFX_FRONTEND_INST=1','-DF4_LEASE_RESET_TEST'],False)]:
        exe=BUILD/name
        run(name+'_compile',['gcc',*flags,*defines,*sources,'-o',exe])
        run(name,[exe],red=red)
    print('FRONTEND ASYNC PIPELINE MODEL PASS',flush=True)

def replay(names):
    env=dict(os.environ,GFX2D_UNPACED='1')
    for name in names:
        flags='-DSTG_FIELD=4 -DSTG_JUDGE_REPLAY=1 -DGFX2D_HEADLESS=1 '+VARIANTS[name]
        obj='build_frontend_'+name
        run('host_'+name+'_compile',['make','-j2','PLATFORM=host','OBJDIR='+obj,'EXTRA_CFLAGS='+flags],APP)
        data=run('host_'+name,[APP/obj/'gfx2dGame'],APP,env)
        lines=[x for x in data.splitlines() if x.startswith('g ')]
        if len(lines)!=9208 or 'fail 0' not in data or '\nerr ' in data:raise RuntimeError(name+': incomplete/failed replay')
        reference=LOGS/'host_flat.log'
        if reference.exists():
            ref=[x for x in reference.read_text(encoding='utf-8-sig').splitlines() if x.startswith('g ')]
            if lines!=ref:raise RuntimeError(name+': replay mismatch at '+str(next(i for i,(a,b) in enumerate(zip(lines,ref)) if a!=b)))
        print('FRONTEND HOST PASS',name,len(lines),flush=True)

def board_log(path,expected_modes,expected_scenes,canonical=True,min_frames=1000):
    data=Path(path).read_text(encoding='utf-8-sig',errors='replace')
    marker='# --- 程序已加载，以下才是它的输出 ---'
    if marker not in data:raise ValueError('missing deployment boundary')
    data=data.split(marker,1)[1];lines=data.splitlines()
    probes=[int(x.split(',')[1]) for x in lines if x.startswith('fprobe,')]
    probe=probes[-1] if probes else 0
    if '# serial lost' in data or 'FAIL' in data or 'fail 0' not in lines or 'done' not in lines:raise ValueError('failed/incomplete board log')
    header=next(x for x in lines if x.startswith('fh,'))
    rows=list(csv.DictReader(io.StringIO('\n'.join([header]+[x for x in lines if x.startswith('fcsv,')]))))
    keys=set();groups={}
    for row in rows:
        r={k:int(v) for k,v in row.items() if k!='fh'}
        if any(v<0 or v>0xffffffff for v in r.values()):raise ValueError('integer field overflow')
        key=(r['scene'],r['mode'],r['repeat'])
        if key in keys:raise ValueError('duplicate group')
        keys.add(key)
        if r['n']<min_frames or r['errors'] or r['bytes']!=32*r['records'] or not 0<r['p50']<=r['p99']<=r['max']:raise ValueError('invalid row')
        if 'logical_draws' in r and r['logical_draws']!=sum(r[k] for k in ('eq_copy','eq_alpha','eq_add','eq_const','eq_mul','eq_st')):raise ValueError('logical equation count mismatch')
        if 'refs' in r and r['mode'] in (2,4,7,9) and (r['refs']!=r['records'] or r['plan_items']>r['logical_draws']):raise ValueError('plan/ref accounting mismatch')
        r['avg']=((r['total_hi']<<32)+r['total_lo'])/r['n']
        groups.setdefault(key[:2],[]).append(r)
    expected={(s,m,r) for s in expected_scenes for m in expected_modes for r in range(3)}
    if keys!=expected:raise ValueError('missing/extra matrix groups')
    result=[]
    for (s,m),rs in sorted(groups.items()):
        if len({r['crc'] for r in rs})!=1:raise ValueError('output unstable')
        av=[r['avg'] for r in rs];spread=(max(av)-min(av))/min(av)
        result.append({'scene':s,'mode':m,'avg':sum(av)/3,'probe_cycles':probe,'avg_corrected':sum(av)/3-probe,
                       'spread':spread,'stable':spread<=.02,
                       'p50':[r['p50'] for r in rs],'p99':[r['p99'] for r in rs],
                       'max':[r['max'] for r in rs],'enemy':rs[0]['visible_enemy'],
                       'records':rs[0]['records'],'bytes':rs[0]['bytes'],'crc':rs[0]['crc'],
                       **{k:[r[k] for r in rs] for k in ('exec_avg','exec_max','pre_avg','band_avg','gpu_busy_avg','logical_draws',
                              'plan_items','refs','template_hits','template_misses') if k in rs[0]}})
    inputs=SIM/'inputs/manifest.json'
    if canonical and inputs.exists():
        canon={(r['scene'],r['mode']):r for r in json.loads(inputs.read_text())}
        for r in result:
            logical=canon.get((r['scene'],0))
            if logical and 'logical_draws' in r and any(v!=logical['records']-3 for v in r['logical_draws']):raise ValueError('logical workload changed')
            if r['mode']<=4:
                expected=canon.get((r['scene'],min(1,r['mode'])))
                if expected and (r['crc']!=expected['crc'] or r['records']!=expected['records']):raise ValueError('fixed output mismatch')
    return result

def checker_selftest():
    import tempfile
    with tempfile.TemporaryDirectory(prefix='gfx_frontend_check_') as tmp:
        path=Path(tmp)/'board.log'
        head='fh,scene,mode,repeat,n,total_hi,total_lo,p50,p99,max,visible_enemy,records,bytes,errors,crc\n'
        body=''.join(f'fcsv,0,0,{r},1000,0,100000,100,110,120,1,5,160,0,123\n' for r in range(3))
        good='# --- 程序已加载，以下才是它的输出 ---\n'+head+body+'fail 0\ndone\n';path.write_text(good,encoding='utf-8');board_log(path,[0],[0],canonical=False)
        mutants=[good.replace('done',''),good+body.splitlines()[0]+'\n',good.replace('160','159'),
                 good.replace('100,110,120','120,110,100'),good.replace('100000','4294967296'),
                 good.replace(body.splitlines()[1]+'\n',''),good+'# serial lost\n']
        for x in mutants:
            path.write_text(x,encoding='utf-8')
            try:board_log(path,[0],[0],canonical=False)
            except ValueError:continue
            raise RuntimeError('checker accepted malformed evidence')
    print('FRONTEND LOG CHECKER SELFTEST PASS',flush=True)

def profiling(path,prefix):
    data=Path(path).read_text(encoding='utf-8-sig')
    marker='# --- 程序已加载，以下才是它的输出 ---'
    if marker not in data:raise ValueError('profile lacks deployment boundary')
    data=data.split(marker,1)[1]
    if 'fail 0' not in data or '\ndone\n' not in data:raise ValueError('profile incomplete')
    rows=[list(map(int,x.split(',')[1:])) for x in data.splitlines() if x.startswith(prefix+',')]
    groups={}
    for scene,mode,repeat,phase,cycles,count in rows:
        key=(scene,mode,phase);r=groups.setdefault(key,{})
        if repeat in r:raise ValueError('duplicate profiling row')
        r[repeat]=(cycles,count)
    out=[]
    for (scene,mode,phase),values in sorted(groups.items()):
        if set(values)!={0,1,2} or len({x[1] for x in values.values()})!=1:raise ValueError('unstable/incomplete profiling count')
        out.append({'scene':scene,'mode':mode,'phase':phase,
                    'cycles':sum(x[0] for x in values.values())/3,'count':values[0][1]})
    return out

def summarize():
    result={}
    for tag,prefix in [('f1_stages_4k','fstage'),('f1_detail_4k','fdetail'),('f1_store_4k','fdetail')]:
        result[tag]=profiling(LOGS/(tag+'.log'),prefix)
    for tag,modes in [('f1_stages_4k',[0,1]),('f1_detail_4k',[1]),('f1_store_4k',[1]),
                      ('f3_cpu_4k',list(range(5))),('f3_cpu_16k',list(range(5)))]:
        result[tag+'_totals']=board_log(LOGS/(tag+'.log'),modes,[0,4,10])
    for r in result['f1_store_4k']:
        if r['phase']==6:
            total=next(x for x in result['f1_store_4k_totals'] if x['scene']==r['scene'] and x['mode']==r['mode'])
            if r['count']!=total['records']:raise ValueError('store probe count differs from physical output')
    result['stage_ledger']=[]
    for total in result['f1_stages_4k_totals']:
        parts=[x for x in result['f1_stages_4k'] if x['scene']==total['scene'] and x['mode']==total['mode']]
        if {x['phase'] for x in parts}!=set(range(8)):raise ValueError('stage coverage')
        counts=sum(x['count'] for x in parts)
        if counts!=total['records']-(1 if total['mode']==0 else 0):raise ValueError('stage output accounting')
        result['stage_ledger'].append({'scene':total['scene'],'mode':total['mode'],'total_raw':total['avg'],
            'stage_sum_raw':sum(x['cycles'] for x in parts),'residual_raw':total['avg']-sum(x['cycles'] for x in parts),
            'empty_probe_cycles':total['probe_cycles']})
    result['detail_ledger']=[]
    for scene in (0,4,10):
        total=next(x for x in result['f1_store_4k_totals'] if x['scene']==scene)
        parts={x['phase']:x for x in result['f1_store_4k'] if x['scene']==scene}
        if set(parts)!=set(range(7)):raise ValueError('detail coverage')
        result['detail_ledger'].append({'scene':scene,'total_raw':total['avg'],
            'parts_raw':parts,'emit_including_store_raw':parts[3]['cycles'],
            'store_raw':parts[6]['cycles'],
            'emit_less_store_raw':parts[3]['cycles']-parts[6]['cycles'],
            'residual_raw':total['avg']-sum(parts[p]['cycles'] for p in range(6)),
            'empty_probe_cycles':total['probe_cycles'],
            'note':'Store is nested in emit. Timed callbacks alter layout and add overhead; subtraction is not uninstrumented generation time.'})
    (LOGS/'cost_results.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print('FRONTEND COST LEDGER PASS',flush=True)

def production_summary():
    result={}
    marker='# --- 程序已加载，以下才是它的输出 ---'
    for name in ('tex','strip','prepass'):
        path=LOGS/('perf_pipeline_'+name+'.log')
        full=path.read_text(encoding='utf-8-sig')
        if marker not in full:raise ValueError('production lacks deployment boundary')
        data=full.split(marker,1)[1];lines=data.splitlines()
        if '# serial lost' in data or [x for x in lines if x.startswith('fail ')]!=['fail 0'] or lines.count('done')!=1 or any(x.startswith('err ') for x in lines):
            raise ValueError('production incomplete/failed '+name)
        header=next(x.split(',')[1:] for x in lines if x.startswith('csvh,'))
        raw=[x.split(',')[1:] for x in lines if x.startswith('csv,')]
        if not raw or any(len(x)!=len(header) for x in raw):raise ValueError('production CSV columns')
        rows=[dict(zip(header,x)) for x in raw]
        sums=[list(map(int,x.split(',')[1:])) for x in lines if x.startswith('csvsum,')]
        if (len(sums)!=1 or len(sums[0])!=7 or sums[0][0]!=1200 or sums[0][-1]!=0 or
            sums[0][-2]!=len(rows) or sums[0][1]-sums[0][0]!=sums[0][2] or
            [int(r['t']) for r in rows]!=list(range(len(rows)))):
            raise ValueError('production frame count/overflow/row sequence')
        diagnostic=next(x for x in lines if x.startswith('diag '))
        tokens=diagnostic.split()[1:]
        diag={k:int(v) for k,v in zip(tokens[::2],tokens[1::2])}
        if diag['fifo_min']<160 or any(diag[k] for k in ('zero','under','faults','swap_retry')):
            raise ValueError('production scan/fault diagnostics '+name)
        steady=rows[2:]
        if not steady or any(r['mode']!='press' or r['pace']!='pipeline' or int(r['overflow']) for r in rows):
            raise ValueError('production mode/pace/overflow')
        keys=('n_bullet','cmd','pixel','cyc_logic','cyc_build','cyc_exec','cpu_avg_cyc','engine_busy_avg_cyc','fps_milli')
        result[name]={'source':path.relative_to(ROOT).as_posix(),'csvsum':sums[0],
            'diagnostics':diag,'steady_rows':len(steady),'discarded_rows':2,
            'mean':{k:statistics.mean(int(r[k]) for r in steady) for k in keys},
            'min':{k:min(int(r[k]) for r in steady) for k in keys},
            'max':{k:max(int(r[k]) for r in steady) for k in keys},
            'bottlenecks':sorted({r['bottleneck'] for r in steady}),
            'note':'One 1200-frame production run per backend, 16x16 K1984 after 1200-step prewarm; advancing states differ with pacing. Not a paired frozen-state comparison or Nmax confirmation.'}
    (LOGS/'production_results.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print('FRONTEND PRODUCTION LOG PASS',flush=True)

def main():
    p=argparse.ArgumentParser();p.add_argument('--models',action='store_true');p.add_argument('--legacy',action='store_true');p.add_argument('--pipeline',action='store_true');p.add_argument('--summarize',action='store_true');p.add_argument('--production',action='store_true');p.add_argument('--host',nargs='+',choices=VARIANTS)
    p.add_argument('--board-log');p.add_argument('--modes',nargs='+',type=int);p.add_argument('--scenes',nargs='+',type=int)
    a=p.parse_args()
    if a.models:models();checker_selftest()
    if a.legacy:legacy()
    if a.pipeline:pipeline_model()
    if a.summarize:summarize()
    if a.production:production_summary()
    if a.host:replay(a.host)
    if a.board_log:
        result=board_log(a.board_log,a.modes,a.scenes)
        dest=Path(a.board_log).with_suffix('.json');dest.write_text(json.dumps(result,indent=2)+'\n')
        print('FRONTEND BOARD LOG PASS',len(result),'groups')
if __name__=='__main__':
    try:main()
    except (RuntimeError,ValueError) as e:print('FAIL:',e,file=sys.stderr);sys.exit(1)
