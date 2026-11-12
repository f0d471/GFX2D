#!/usr/bin/env python3
"""F4 生产实例通路：独立像素金标准、完整引擎、变异与兼容性。"""
import argparse
import json
import os
import sys
import re
from pathlib import Path
import subprocess
from check_run import IncompleteCheck, paths, session
from feature_build import write_headers
ROOT=Path(__file__).resolve().parents[3]
MAIN=ROOT/'Main'
SIM=MAIN/'sim/engine/frontend'
BUILD,LOGS=paths('frontend','f4')
INCLUDE=write_headers('B3',BUILD/'include')
GFX_CORE=[MAIN/'sw/lib/core'/p for p in ('format.c','fixmath.c','sin_tab.c')]
RTL=MAIN/'rtl'
def run(tag,args,cwd=ROOT,timeout=600):
    p=subprocess.run(list(map(str,args)),cwd=cwd,capture_output=True,text=True,timeout=timeout)
    body=p.stdout+p.stderr
    dest=(BUILD if tag.endswith('_compile') and not body else LOGS)/('f4_'+tag+'.log')
    dest.write_text(body,encoding='utf-8')
    if p.returncode:raise RuntimeError(tag+' exit '+str(p.returncode)+': '+body[-2000:])
    return body
def models():
    src=[SIM/'test_instances.c',*sorted((MAIN/'sw/lib/gfx').glob('*.c')),*GFX_CORE]
    flags=['gcc','-O1','-g','-std=c11','-Wall','-Wextra','-Werror','-I'+str(MAIN/'sw/lib')]
    run('host_compile',flags+src+['-o',BUILD/'host'])
    out=run('host',[BUILD/'host',BUILD/'fixtures'])
    assert 'frames=10000' in out and 'BOUNDARY PASS' in out
    run('sanitize_compile',flags+['-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie']+src+['-o',BUILD/'sanitize'])
    out2=run('sanitize',[BUILD/'sanitize'])
    assert 'frames=10000' in out2
    print(out.strip(),flush=True)
def compile_rtl(tag,defines=()):
    sources=[SIM/'tb_engine_instances.v',*sorted((RTL/'engine').glob('**/*.v')),
        RTL/'vendor/verilog-axi/axi_dma_rd.v',RTL/'vendor/verilog-axi/axi_dma_wr.v',MAIN/'sim/axi_probe/axi_ram_model.v']
    run(tag+'_compile',['iverilog','-g2012','-s','tb_engine_instances','-I'+str(INCLUDE),'-I'+str(RTL/'engine')]+
        ['-D'+d for d in defines]+['-o',BUILD/(tag+'.vvp')]+sources)
    return BUILD/(tag+'.vvp')
def rtl_cases(profile):
    return list(range(144)) if profile!='quick' else sorted(set([6*i+i%6 for i in range(24)]+[66]))

def rtl():
    binary=compile_rtl('rtl')
    cases=rtl_cases(session().profile)
    for n in cases:
        out=run('rtl_%03d'%n,['vvp','-n',binary,'+CASE='+str(BUILD/'fixtures'/('case%02d'%n))],timeout=120)
        if 'RESULT: PASS' not in out or 'FAIL' in out:raise RuntimeError('RTL case '+str(n)+': '+out[-3500:])
    print('F4 RTL PASS cases='+str(len(cases))+' scope='+session().profile,flush=True)
def mutations():
    mutations=['STRIDE','XYUV','STATE','SKIP','OWNER','EARLY_DONE']
    for name in mutations:
        binary=compile_rtl('mut_'+name,['INJ_INST_'+name])
        zero=run('mut_'+name+'_zero',['vvp','-n',binary,'+CASE='+str(BUILD/'fixtures/case00')])
        assert 'RESULT: PASS' in zero, name+' damages zero-instance control'
        out=run('mut_'+name,['vvp','-n',binary,'+CASE='+str(BUILD/'fixtures/case66')])
        assert 'RESULT: FAIL' in out,name+' escaped'
        print('F4 MUTATION detected '+name,flush=True)
def protocols():
    cases=[('stride_cross4k',200,()),('overlap_identity',201,()),('array_bus_error',202,('-Ptb_engine_instances.ERRARRAY=1',)),
           ('array_timeout',203,('-Ptb_engine_instances.TIMEOUT_TEST=1',)),('bad_packet',204,()),
           ('reset_recovery',66,('-Ptb_engine_instances.RESET_TEST=1',)),
           ('prefetch_timeout_identity',205,('-Ptb_engine_instances.TIMEOUT_TEST=2',))]
    for tag,n,params in cases:
        sources=[SIM/'tb_engine_instances.v',*sorted((RTL/'engine').glob('**/*.v')),
            RTL/'vendor/verilog-axi/axi_dma_rd.v',RTL/'vendor/verilog-axi/axi_dma_wr.v',MAIN/'sim/axi_probe/axi_ram_model.v']
        run(tag+'_compile',['iverilog','-g2012','-s','tb_engine_instances','-I'+str(INCLUDE),'-I'+str(RTL/'engine'),*params,'-o',BUILD/(tag+'.vvp'),*sources])
        out=run(tag,['vvp','-n',BUILD/(tag+'.vvp'),'+CASE='+str(BUILD/'fixtures'/('case%02d'%n))])
        assert 'RESULT: PASS' in out and 'FAIL' not in out,tag+': '+out[-2500:]
        print('F4 PROTOCOL PASS '+tag,flush=True)
def lease():
    game=MAIN/'sw/apps/game/gfx2dGame/src'
    flags=['gcc','-O1','-g','-std=c11','-Wall','-Wextra','-Werror','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-DGFX_FRONTEND_INST=1',
        '-I'+str(MAIN/'sw/lib'),'-I'+str(MAIN/'sw/bsp/include'),'-I'+str(MAIN/'sw/ports/myriad'),'-I'+str(game),'-I'+str(MAIN/'sw/services/gfx')]
    run('lease_compile',flags+[SIM/'test_instance_lease.c',MAIN/'sw/services/gfx/renderer.c',MAIN/'sw/bsp/memmap.c',
        *sorted((MAIN/'sw/lib/gfx').glob('*.c')),*GFX_CORE,'-o',BUILD/'lease'])
    print(run('lease',[BUILD/'lease']).strip(),flush=True)
def v2():
    import importlib.util
    spec=importlib.util.spec_from_file_location('g3_f4_review',MAIN/'tools/check/check_g3_stripctl.py')
    m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
    m.BUILD=BUILD/'v2';m.LOGS=BUILD/'v2_logs'
    sys_argv=sys.argv[:];sys.argv=[str(spec.origin)]
    try:m.main()
    finally:sys.argv=sys_argv
    for p in m.LOGS.glob('*.log'):
        if p.stat().st_size:(LOGS/('f4_v2_'+p.name)).write_bytes(p.read_bytes())
    (LOGS/'f4_v2_results.json').write_bytes((m.LOGS/'formal_results.json').read_bytes())
def band():
    import importlib.util,contextlib,io
    spec=importlib.util.spec_from_file_location('g2_f4_review',MAIN/'tools/check/check_g2_band.py')
    m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
    # check_g2_band.py 的 CASE 路径相对其既有忽略 build 目录；原始 logs 独立隔离。
    m.LOGS=BUILD/'legacy_band_logs';m.LOGS.mkdir(parents=True,exist_ok=True)
    args=sys.argv[:];sys.argv=[str(spec.origin)];captured=io.StringIO()
    try:
        with contextlib.redirect_stdout(captured):m.main()
    finally:
        sys.argv=args
        (LOGS/'f4_legacy_band.log').write_text(captured.getvalue(),encoding='utf-8')
    for p in m.LOGS.glob('*'):
        if p.suffix in ['.log','.json'] and p.stat().st_size:(LOGS/('f4_legacy_band_'+p.name)).write_bytes(p.read_bytes())
    print(captured.getvalue().strip(),flush=True)

def legacy():
    env=os.environ.copy()
    if not env.get('EFINITY_HOME'):
        raise RuntimeError('先设置 EFINITY_HOME 为安装目录的 WSL 路径，见开发环境第 1 节')
    for part in ['ctrl','tex','frame','geom','mover','pix','band']:
        if part=='band':session().perform('f4.band',band);continue
        args=['bash',str(MAIN/'sim/engine'/part/'run.sh')]
        if not Path(args[1]).exists():raise RuntimeError('missing legacy entry: '+part)
        p=subprocess.run(args,cwd=ROOT,env=env,capture_output=True,text=True,timeout=1200)
        (LOGS/('f4_legacy_'+part+'.log')).write_text(p.stdout+p.stderr,encoding='utf-8')
        if p.returncode:raise RuntimeError('legacy '+part+': '+(p.stdout+p.stderr)[-2000:])
        print('F4 LEGACY PASS '+part,flush=True)
    session().perform('f4.v2',v2)



def research():
    import importlib.util
    app=MAIN/'sw/apps/probe/f4Bench'
    flags=['gcc','-O1','-g','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie',
        '-I'+str(MAIN/'sw/lib'),'-I'+str(app/'src')]
    run('research_compile',flags+[SIM/'f4_inst_proto.c',app/'src/f4_core.c','-o',BUILD/'research'])
    inputs=sorted((SIM/'inputs').glob('*_mode0.bin'))
    out=run('research_fixed_host',[BUILD/'research',*inputs])
    assert len(inputs)==17 and out.count(' PASS in=')==17
    boundary=run('research_fixed_boundary',[BUILD/'research','--boundary'])
    assert 'boundary PASS' in boundary
    generated=BUILD/'research_inputs';generated.mkdir(exist_ok=True)
    spec=importlib.util.spec_from_file_location('f4_input_generation',app/'gen_inputs.py')
    m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);m.SRC=generated
    selected=[SIM/'inputs'/('scene%02d_mode0.bin'%i) for i in [0,4,14]]
    assert m.main(['gen_inputs.py',*map(str,selected)])==0
    run('research_input_compile',['gcc','-Wall','-Wextra','-Werror','-I'+str(generated),'-c',generated/'f4_scenes.c','-o',generated/'scenes.o'])
    source=(generated/'f4_scenes.c').read_text(encoding='utf-8')
    for path in selected:
        text=re.search(r'const uint8_t f4_'+path.stem+r'\[\d+\] = \{(.*?)\};',source,re.S)
        assert text,path
        data=bytes(int(x,16) for x in re.findall(r'0x([0-9a-f]{2})',text[1]))
        assert data==path.read_bytes(),path
    (LOGS/'f4_research_input_roundtrip.log').write_text('PASS: three generated C arrays byte-identical to frozen input bins; strict C compile passed\n',encoding='utf-8')
    print('F4 RESEARCH PASS scenes=17 DMA_alignment/padding/bounds/identities=checked generated_inputs=byte_equal',flush=True)


def profile():
    import shutil
    original=BUILD/'fixtures/case66';target=BUILD/'fixtures/profile_scalar'
    init=bytearray().join(int(x,16).to_bytes(16,'little') for x in original.with_suffix('.init.hex').read_text().splitlines())
    packets=[[int(line,16)>>(32*j)&0xffffffff for j in range(8)] for line in original.with_suffix('.cmd.hex').read_text().splitlines()]
    scalar=[]
    for p in packets:
        if p[0]&255!=5:scalar.append(p);continue
        for i in range(p[2]):
            off=p[1]+i*(p[5]&65535);xy=int.from_bytes(init[off:off+4],'little')
            uv=p[6] if (p[5]>>16)&3==0 else int.from_bytes(init[off+4:off+8],'little')
            scalar.append([(p[0]&~255)|2,uv,xy,p[3],0,p[4],0,p[7]])
    target.with_suffix('.cmd.hex').write_text(''.join(''.join('%08x'%w for w in reversed(p))+'\n' for p in scalar))
    for ext in ['init','surf','exp','tex']:shutil.copyfile(original.with_suffix('.'+ext+'.hex'),target.with_suffix('.'+ext+'.hex'))
    target.with_suffix('.cnt.hex').write_text('%08x\n00000000\n00001000\n'%len(scalar))
    stats=original.with_suffix('.stats.hex').read_text().splitlines();stats[3]='00000000';stats[4]='%08x'%(len(scalar)-1)
    target.with_suffix('.stats.hex').write_text('\n'.join(stats)+'\n')
    binary=compile_rtl('profile',['TIMED']);result={}
    for tag,case in [('scalar',target),('instances',original)]:
        out=run('profile_'+tag,['vvp','-n',binary,'+CASE='+str(case)])
        assert 'RESULT: PASS' in out and 'FAIL' not in out
        line=next(x for x in out.splitlines() if x.startswith('PROFILE '))
        frame=next(x for x in out.splitlines() if x.startswith('FRAME '))
        result[tag]={k:int(v) for k,v in re.findall(r'(\w+)=(\d+)',line+' '+frame)}
    result['scope']='TIMED full-engine fixture, identical initial/expected pixels; stage intervals can overlap; not board cycle decomposition'
    (LOGS/'f4_profile.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print('F4 PROFILE PASS '+json.dumps(result),flush=True)

def review_archive():
    import re
    cache={}
    def read_log(name):
        if name not in cache:cache[name]=(LOGS/name).read_text(encoding='utf-8-sig',errors='replace')
        return cache[name]
    def need(ok,what):
        if not ok:raise RuntimeError('evidence: '+what)
    for tag in ['host','sanitize']:need('frames=10000' in read_log('f4_'+tag+'.log'),tag)
    for i in range(144):need('RESULT: PASS' in read_log('f4_rtl_%03d.log'%i),'rtl '+str(i))
    for name in ['STRIDE','XYUV','STATE','SKIP','OWNER','EARLY_DONE']:
        need('RESULT: FAIL' in read_log('f4_mut_'+name+'.log'),'mutant '+name)
        need('RESULT: PASS' in read_log('f4_mut_'+name+'_zero.log'),'zero control '+name)
    protocol_names=['stride_cross4k','overlap_identity','array_bus_error','array_timeout','bad_packet','reset_recovery','prefetch_timeout_identity']
    for name in protocol_names:need('RESULT: PASS' in read_log('f4_'+name+'.log'),'protocol '+name)
    need('F4 LEASE PASS' in read_log('f4_lease.log'),'lease')
    need('F4 LEASE RESET PASS' in read_log('f4_pipeline_instance_reset.log'),'lease reset recovery')
    for part in ['ctrl','tex','frame','geom','mover','pix','band']:
        body=read_log('f4_legacy_'+part+'.log');need('SIM OK' in body or 'FORMAL OK' in body,'legacy '+part)
    v2=json.loads((LOGS/'f4_v2_results.json').read_text())
    need(v2['software']['scenes_sanitized']==10000 and v2['software']['skipped']==0,'v2 scenes')
    for k in ['rtl16','rtl32']:need(v2[k]['errors']==0 and v2[k]['frame_word_differences']==0,k)
    board={}
    for tag in ['tests-final','fallback','ab','verify','research-fixed','pressure-check','nmax-base','nmax-inst']:
        body=read_log('f4_board_'+tag+'.log')
        marker='instanceTest start' if tag in ['tests-final','fallback'] else 'f4Bench start' if tag=='research-fixed' else 'gfx2dGame MODES' if tag.startswith('nmax') or tag=='pressure-check' else 'frontendBench F1'
        at=body.rfind(marker);need(at>=0,'banner '+tag);body=body[at:]
        lines=[x.strip() for x in body.splitlines()]
        need('# serial lost' not in body and [x for x in lines if re.fullmatch(r'fail [0-9]+',x)]==['fail 0'] and 'done' in lines,'board completion '+tag)
        if tag in ['tests-final','fallback']:
            pairs=re.findall(r'hw_crc=(\d+) sw_crc=(\d+)',body)
            need(len(pairs)==122 and all(a==b for a,b in pairs),'board CRC '+tag)
            need(('fallback 1' if tag=='fallback' else 'fallback 0') in body,'CAPS '+tag)
            board[tag]={'crc_cases':122,'fail':0}
        elif tag=='verify':
            rows=[x.split(',') for x in body.splitlines() if x.startswith('fverify,')]
            need(len(rows)==102 and all(r[4]=='0' and r[5]==r[6] for r in rows),'real game verification')
            board[tag]={'paired_state_checks':102,'fail':0}
        elif tag=='pressure-check':
            header=next(x.split(',') for x in lines if x.startswith('csvh,'))
            samples=[dict(zip(header,x.split(','))) for x in lines if x.startswith('csv,')]
            need(samples and any(int(r['n_bullet'])>100 for r in samples),'pressure workload')
            need(all(int(r['cmd'])>=int(r['n_bullet']) for r in samples),'instance statistics consumer')
            board[tag]={'samples':len(samples),'logical_draws_complete':True,'fail':0}
        elif tag.startswith('nmax'):
            rows=re.findall(r'^nmax result OK n_max (\d+) knob (\d+) ft_max (\d+) ft_p99 (\d+)',body,re.M)
            need(len(rows)==2,'nmax kinds '+tag)
            diag=re.search(r'^diag fifo_min (\d+).* zero (\d+) under (\d+).* faults (\d+) swap_retry (\d+)',body,re.M)
            need(diag and int(diag[1])>=160 and all(int(x)==0 for x in diag.groups()[1:]),'scan diagnostics '+tag)
            header=next(x.split(',') for x in lines if x.startswith('csvh,'))
            samples=[dict(zip(header,x.split(','))) for x in lines if x.startswith('csv,')]
            need(samples and all(int(r['cmd'])>=int(r['n_bullet']) for r in samples),'nmax statistics consumer '+tag)
            board[tag]={'16x16':list(map(int,rows[0])),'8x8':list(map(int,rows[1])),'fifo_min':int(diag[1]),'fail':0}
        else:board[tag]={'fail':0}
    expected=json.loads((MAIN/'sim/engine/g4/logs/artifacts.json').read_text())['Main/fpga/gfx2d_ti60/outflow/gfx2d_ti60.bit']['sha256']
    registry=json.loads((LOGS/'f4_artifacts.json').read_text())
    need(registry['Main/sim/engine/frontend/build/f4/baseline.bit']['sha256']==expected,'fallback baseline identity')
    need('boundary PASS' in read_log('f4_research_fixed_boundary.log'),'research boundaries')
    need('byte-identical' in read_log('f4_research_input_roundtrip.log'),'research input roundtrip')
    need('V2 BLIT_INST rejected before flush' in read_log('f4_board_tests-final.log'),'v2 opcode rejection')
    syn=json.loads((LOGS/'f4_synthesis.json').read_text())
    need(syn['inst']['ram']<=4 and all(s['slack']>=0 for k in syn.values() for s in k['slacks']),'synthesis')
    result={'models_frames':10000,'rtl_cases':144,'mutations_red':6,'protocols':protocol_names,'lease':'PASS','v2':v2,'board':board,'synthesis':syn,
        'artifact_registry':'f4_artifacts.json','scope':'v1-I direct profile; historical full-replay and human HDMI/input observations retain their existing scope'}
    result['scope']='historical archive consistency review; no current deployment claim'
    (session().logs('archive-review')/'f4_results.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print('F4 ARCHIVE REVIEW PASS models=10000 rtl=144 mutations=6 board='+','.join(board),flush=True)

def collect():
    if session().profile=='quick':raise RuntimeError('快速档不能生成完整验收汇总')
    required=['f4.'+name for name in ['models','rtl','mutations','protocols','lease']]
    session().require(required)
    result={'scope':'current host/RTL only; board and synthesis collected separately',
            'profile':session().profile,'models_frames':10000,'rtl_cases':144,'mutations_red':6,
            'groups':{name:session().read()['checks'][name] for name in required}}
    (LOGS/'f4_results.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print('F4 HOST RTL COLLECT PASS scope='+session().profile,flush=True)
def main():
    global LOGS
    p=argparse.ArgumentParser()
    names=['models','rtl','mutations','protocols','lease','v2','band','legacy','research','profile','collect','review_archive']
    for name in names:p.add_argument('--timed-profile' if name=='profile' else '--'+name.replace('_','-'),dest=name,action='store_true')
    p.add_argument('--archive-dir',type=Path)
    a=p.parse_args();BUILD.mkdir(parents=True,exist_ok=True);(BUILD/'fixtures').mkdir(exist_ok=True);LOGS.mkdir(exist_ok=True)
    selected=[name for name in names if getattr(a,name)]
    if not selected:p.error('至少选择一组判据')
    if a.review_archive:
        if selected!=['review_archive'] or not a.archive_dir:p.error('历史复核只与 --archive-dir 一起使用')
        LOGS=(ROOT/a.archive_dir).resolve()
        if not LOGS.is_relative_to(ROOT.resolve()):p.error('历史证据目录必须在仓库内')
    if any(name in selected for name in ['rtl','mutations','protocols','profile']) and 'models' not in selected:
        selected.insert(0,'models')
    for name in selected:session().perform('f4.'+name,globals()[name])
    print('F4 CHECK PASS scope='+session().profile+' run='+str(session().path.relative_to(ROOT)),flush=True)
if __name__=='__main__':
    try:main()
    except IncompleteCheck as error:print('INCOMPLETE '+str(error),file=sys.stderr);sys.exit(3)
