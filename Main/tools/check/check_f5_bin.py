#!/usr/bin/env python3
"""F5 的研究复现与正式 PB/RTL 判据，产物写入既有忽略 build。"""
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
from check_run import IncompleteCheck, paths, session
from feature_build import write_headers

ROOT = Path(__file__).resolve().parents[3]
MAIN = ROOT / 'Main'
SIM = MAIN / 'sim/engine/frontend'
BUILD, LOGS = paths('frontend', 'f5')
INCLUDE = write_headers('B5', BUILD/'include')
GFX_CORE = [MAIN/'sw/lib/core'/p for p in ('format.c','fixmath.c','sin_tab.c')]

def run(tag, args, timeout=600):
    p = subprocess.run(list(map(str, args)), cwd=ROOT, capture_output=True,
                       text=True, timeout=timeout)
    body = p.stdout + p.stderr
    (LOGS / ('f5_' + tag + '.log')).write_text(body, encoding='utf-8')
    if p.returncode == 3:
        raise IncompleteCheck(tag + ': 必需项未执行')
    if p.returncode:
        raise RuntimeError(tag + ': ' + body[-3000:])
    return body

def research():
    spec = importlib.util.spec_from_file_location('pb_analysis', SIM / 'f5_pb_analysis.py')
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    manifest = json.loads((SIM / 'inputs/manifest.json').read_text())
    actual = [m.scene_stats(e) for e in manifest if e['mode'] == 0]
    old = json.loads((SIM / 'f5_pb_stats.json').read_text())
    assert actual == old, '研究统计未逐字段复现'
    for e in manifest:
        if e['mode']:
            continue
        surf, recs = m.load_v1(SIM / ('inputs/scene%02d_mode0.bin' % e['scene']))
        pb = m.build_pb(recs, surf)
        expected, _ = m.load_v2(SIM / ('inputs/scene%02d_mode1.bin' % e['scene']))
        counts = pb['counts'][:]
        counts[0] += pb['n_nop']
        assert counts == expected[:30], ('每桶引用计数', e['scene'])
    flags = ['gcc', '-std=c11', '-O1', '-g', '-I' + str(MAIN / 'sw/lib'),
             '-fsanitize=address,undefined', '-fno-pie', '-no-pie']
    run('research_compile', flags + [SIM / 'f5_pb_fuzz.c',
        *sorted((MAIN / 'sw/lib/gfx').glob('*.c')), *GFX_CORE,
        '-o', BUILD / 'research'])
    out = run('research_review', [BUILD / 'research', '10000'])
    assert '10000 通过 / 0 容量跳过 / 0 失败' in out
    assert out.count('：红') == 6
    print('F5 RESEARCH PASS: 17 scenes exact buckets; ASan/UBSan 10000 scenes; 6 mutations')

def models():
    flags=['gcc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-I'+str(MAIN/'sw/lib'),
           '-fsanitize=address,undefined','-fno-pie','-no-pie']
    src=[*sorted((MAIN/'sw/lib/gfx').glob('*.c')),*GFX_CORE]
    run('host_compile',flags+[SIM/'test_pb.c',*src,'-o',BUILD/'host'])
    print(run('host',[BUILD/'host',BUILD/'bin_cases.dat']).strip(),flush=True)
    run('bounds_compile',flags+[SIM/'test_pb_bounds.c',*src,'-o',BUILD/'bounds'])
    print(run('bounds',[BUILD/'bounds']).strip(),flush=True)

def compile_rtl(tag,top,defines=(),params=()):
    src=[SIM/(top+'.v'),*sorted((MAIN/'rtl/engine').glob('**/*.v')),
         MAIN/'rtl/vendor/verilog-axi/axi_dma_rd.v',MAIN/'rtl/vendor/verilog-axi/axi_dma_wr.v',MAIN/'sim/axi_probe/axi_ram_model.v']
    run(tag+'_compile',['iverilog','-g2012','-s',top,'-I'+str(INCLUDE),'-I'+str(MAIN/'rtl/engine/include'),
                       *['-D'+d for d in defines],*params,'-o',BUILD/(tag+'.vvp'),*src])
    return BUILD/(tag+'.vvp')

def bin():
    binary=compile_rtl('bin','tb_engine_bin')
    import re
    count=64 if session().profile=='quick' else 10000
    args=['vvp','-n',binary,'+CASES='+str(BUILD/'bin_cases.dat'),'+LIMIT='+str(count)]
    out=run('bin',args,timeout=3600)
    assert re.search(r'RESULT: PASS F5 BIN scenes=\s*'+str(count)+r' errors=\s*0\b',out),out[-2000:]
    print('F5 BIN PASS scenes='+str(count)+' scope='+session().profile,flush=True)

def frame():
    binary=compile_rtl('frame','tb_engine_pb')
    out=run('frame',['vvp','-n',binary])
    assert 'RESULT: PASS' in out and 'FAIL' not in out
    print('F5 FRAME PASS pixels/padding/READY/AXI_counters',flush=True)

def mutations():
    # 六个变异体（bin.clip/order/skip/ready/high_byte/stale）已移到
    # Main/sim/mutation/mutants.yaml，由上面的分桶正常用例承担发现责任。
    print('F5 MUTATIONS delegated to Main/sim/mutation/mutants.yaml',flush=True)

def protocols():
    for n in [*range(1,9),12,13,14,15,17,18,20,21,22,23,24,25]:
        binary=compile_rtl('protocol_'+str(n),'tb_engine_pb',params=['-Ptb_engine_pb.TEST='+str(n)])
        out=run('protocol_'+str(n),['vvp','-n',binary])
        assert 'RESULT: PASS' in out and 'FAIL' not in out,out[-2000:]
        print('F5 PROTOCOL PASS '+str(n),flush=True)
    for height in [8,32]:
        binary=compile_rtl('height_'+str(height),'tb_engine_pb',params=['-Ptb_engine_pb.STRIP_H='+str(height)])
        out=run('height_'+str(height),['vvp','-n',binary])
        assert 'RESULT: PASS' in out and 'FAIL' not in out,out[-2000:]
        print('F5 HEIGHT PASS '+str(height),flush=True)

def game():
    source=MAIN/'sw/apps/game/gfx2dGame/src';lib=MAIN/'sw/lib'
    run('game_dump_compile',['gcc','-O2','-std=c11','-Wall','-Wextra','-Werror',
        '-ffunction-sections','-fdata-sections','-Wl,--gc-sections',
        *['-I'+str(p) for p in [lib,MAIN/'sw/bsp/include',source,MAIN/'assets/build']],
        MAIN/'tools/check/f5_game_dump.c',MAIN/'sw/ports/myriad/stg_modes.c',
        *sorted((source/'game').glob('stg_*.c')),*sorted((lib/'gfx').glob('*.c')),
        *sorted((lib/'core').glob('*.c')),'-o',BUILD/'game_dump'])
    for height in [16,32]:
        prefix=BUILD/('game'+str(height))
        out=run('game_golden_'+str(height),[BUILD/'game_dump',prefix,MAIN/'assets/build/assets.pak','1984',str(height)])
        assert 'F5 GAME GOLDEN PASS' in out
        for timed in ([0,1] if height==16 else [0]):
            tag='game_'+str(height)+'_'+str(timed)
            binary=compile_rtl(tag,'tb_engine_pb_game',params=['-Ptb_engine_pb_game.SH='+str(height),
                '-Ptb_engine_pb_game.TIMED_MODEL='+str(timed)])
            out=run(tag,['vvp','-n',binary,'+case='+str(prefix)],timeout=1800)
            assert 'RESULT: PASS' in out and 'FAIL' not in out and 'WATCHDOG' not in out,out[-2000:]
            print('F5 REAL GAME FRAME PASS H='+str(height)+' TIMED='+str(timed),flush=True)

def replay():
    import os
    app=MAIN/'sw/apps/game/gfx2dGame'
    env=dict(os.environ,GFX2D_UNPACED='1')
    results=[]
    for name,extra in [('flat',''),('pb','-DSW9_REPLAY_HW=1 -DG4_STRIP_RENDER=1 -DGFX_FRONTEND_BIN=1')]:
        obj='build_f5_host_'+name
        flags='-DSTG_FIELD=4 -DSTG_JUDGE_REPLAY=1 -DGFX2D_HEADLESS=1 '+extra
        run('replay_'+name+'_compile',['make','-C',app,'-j8','PLATFORM=host','OBJDIR='+obj,'EXTRA_CFLAGS='+flags])
        p=subprocess.run([str(app/obj/'gfx2dGame')],cwd=app,env=env,capture_output=True,text=True,timeout=3600)
        body=p.stdout+p.stderr;(LOGS/('f5_replay_host_'+name+'.log')).write_text(body,encoding='utf-8')
        lines=[x for x in body.splitlines() if x.startswith('g ')]
        assert p.returncode==0 and len(lines)==9208 and 'fail 0' in body,name+': '+body[-2000:]
        results.append(lines)
    assert results[0]==results[1],next((str(i) for i,(a,b) in enumerate(zip(*results)) if a!=b),'length')
    print('F5 HOST REPLAY PASS frames=9208 row_differences=0',flush=True)

def lease():
    game=MAIN/'sw/apps/game/gfx2dGame/src'
    flags=['gcc','-std=c11','-O1','-Wall','-Wextra','-Werror','-ffunction-sections',
           '-fdata-sections','-Wl,--gc-sections','-DGFX_FRONTEND_BIN=1',
           '-I'+str(MAIN/'sw/lib'),'-I'+str(MAIN/'sw/bsp/include'),'-I'+str(MAIN/'sw/ports/myriad'),'-I'+str(game),'-I'+str(MAIN/'sw/services/gfx')]
    run('lease_compile',flags+[SIM/'test_pb_lease.c',MAIN/'sw/services/gfx/renderer.c',MAIN/'sw/bsp/memmap.c',
        *sorted((MAIN/'sw/lib/gfx').glob('*.c')),*GFX_CORE,'-o',BUILD/'lease'])
    print(run('lease',[BUILD/'lease']).strip(),flush=True)
    run('fallback_compile',flags+['-D__riscv',SIM/'test_pb_fallback.c',MAIN/'sw/services/gfx/renderer.c',MAIN/'sw/bsp/memmap.c',
        *sorted((MAIN/'sw/lib/gfx').glob('*.c')),*GFX_CORE,'-o',BUILD/'fallback'])
    print(run('fallback',[BUILD/'fallback']).strip(),flush=True)

def ddr_log():
    run('ddr_log_compile',['gcc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror',
        '-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(MAIN/'sw/bsp/include'),
        SIM/'test_ddr_log.c','-o',BUILD/'ddr_log'])
    assert 'F5 DDR LOG PASS' in run('ddr_log',[BUILD/'ddr_log'])

def legacy():
    spec=importlib.util.spec_from_file_location('f4_compat',MAIN/'tools/check/check_f4_instances.py')
    m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
    m.BUILD=BUILD/'legacy';m.LOGS=LOGS/'f5_legacy_f4'
    m.BUILD.mkdir(exist_ok=True);(m.BUILD/'fixtures').mkdir(exist_ok=True);m.LOGS.mkdir(exist_ok=True)
    m.models();m.rtl();m.protocols();m.lease()
    print('F5 LEGACY F4 PASS',flush=True)

def legacy_all():
    spec=importlib.util.spec_from_file_location('all_compat',MAIN/'tools/check/check_f4_instances.py')
    m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
    m.BUILD=BUILD/'legacy_all';m.LOGS=LOGS/'f5_legacy_all'
    m.BUILD.mkdir(exist_ok=True);m.LOGS.mkdir(exist_ok=True)
    m.legacy()
    print('F5 LEGACY ALL PASS ctrl/TEX/frame/geom/mover/pix/BAND/v2',flush=True)

def platform():
    """仓库规定的提交检查；应用构建串行，避免共享生成物互相覆盖。"""
    import os
    if not (Path(os.environ.get('EFINITY_HOME',''))/'sim_models/verilog/efx_srl8.v').exists():
        raise IncompleteCheck('按开发环境设置 WSL 的 EFINITY_HOME 后再运行 --platform')
    prefix=os.environ.get('RISCV_BIN')
    if not prefix:
        compiler=next(Path.home().glob('opt/xpack-riscv-none-elf-gcc-13.4.0-*/bin/riscv-none-elf-gcc'),None)
        if compiler is None:raise IncompleteCheck('缺少交叉工具链，设置 RISCV_BIN')
        prefix=str(compiler)[:-3]
    env=dict(os.environ,RISCV_BIN=prefix)
    run('platform_unit',['bash',MAIN/'tools/check/check_host.sh','lib'],timeout=1800)
    print('F5 PLATFORM PASS unit64/32',flush=True)
    for tag,script in [('layers','check_sw_layers.sh'),('sw6','check_sw6_demo.sh'),
                       ('sw7','check_sw7_game.sh'),('sw8','check_sw8_stats.sh')]:
        p=subprocess.run(['bash',str(MAIN/'tools/check'/script)],cwd=ROOT,env=env,
                         capture_output=True,text=True,timeout=1800)
        (LOGS/('f5_platform_'+tag+'.log')).write_text(p.stdout+p.stderr,encoding='utf-8')
        if p.returncode==3:raise IncompleteCheck(tag+': 必需项未执行')
        assert p.returncode==0,(tag,(p.stdout+p.stderr)[-2500:])
        print('F5 PLATFORM PASS '+tag,flush=True)
    session().perform('f5.platform_hw',platform_hw)

def platform_hw():
    for name in ['hdmi','apb_probe','axi_probe','axi_mux_guard','display','engine/blend']:
        tag=name.replace('/','_')
        run('platform_'+tag,['bash',MAIN/'sim'/name/'run.sh'],timeout=1800)
        print('F5 PLATFORM PASS '+name,flush=True)

def collect():
    if session().profile=='quick':raise RuntimeError('快速档不能生成完整验收汇总')
    session().require(['f5.'+name for name in ['research','models','bin','frame','mutations','protocols','game','replay','lease','legacy','legacy_all','platform','ddr_log']])
    from check_g4_integration import compare
    def body(name):return (LOGS/(name+'.log')).read_text(encoding='utf-8',errors='replace')
    def require(name,marker):
        text=body(name)
        assert marker in text,name
        return text
    require('f5_research_review','10000 通过 / 0 容量跳过 / 0 失败')
    assert body('f5_research_review').count('：红')==6
    require('f5_host','scenes=10000 exact_pixels=10000 skipped=0')
    require('f5_bounds','F5 PB BOUNDARY PASS')
    require('f5_bin','RESULT: PASS F5 BIN scenes=      10000 errors=          0')
    normal=['f5_frame',*[f'f5_protocol_{n}' for n in [*range(1,9),12,13,14,15,17,18,20,21,22,23,24,25]],
            'f5_height_8','f5_height_32','f5_game_16_0','f5_game_16_1','f5_game_32_0']
    for name in normal:
        text=require(name,'RESULT: PASS')
        assert 'FAIL' not in text and 'WATCHDOG' not in text,name
    mutants=['CLIP','ORDER','SKIP','READY','HIGH_BYTE','STALE']
    for name in mutants:require('f5_mut_'+name,'FAIL')
    require('f5_bin_control','RESULT: PASS')
    require('f5_lease','F5 PB LEASE PASS')
    require('f5_fallback','F5 CAPABILITY PASS')
    require('f5_ddr_log','F5 DDR LOG PASS')
    require('f5_legacy_f4/f4_host','F4 HOST PASS frames=10000')
    for i in range(144):require(f'f5_legacy_f4/f4_rtl_{i:03d}','RESULT: PASS')
    for name in ['stride_cross4k','bad_packet','overlap_identity','array_bus_error','array_timeout','prefetch_timeout_identity','reset_recovery']:
        require('f5_legacy_f4/f4_'+name,'RESULT: PASS')
    for name in ['ctrl','tex','frame','geom','mover','pix']:
        require('f5_legacy_all/f4_legacy_'+name,'SIM OK')
    require('f5_legacy_all/f4_legacy_band','G2 FORMAL OK: 17 checks as expected')
    for name in ['layers','sw6','sw7','sw8','unit']:
        require('f5_platform_'+name,'fail 0')
    for name in ['hdmi','apb_probe','axi_probe','axi_mux_guard','display','engine_blend']:
        require('f5_platform_'+name,'SIM OK')
    replay=compare(LOGS/'f5_replay_host_flat.log',LOGS/'f5_replay_host_pb.log')
    result={'research_scenes':10000,'frozen_bucket_states':17,'host_pixel_scenes':10000,
            'rtl_pb_byte_scenes':10000,'protocols':20,'strip_heights':[8,16,32],
            'real_game_frames':['H16 random','H16 timed','H32 random'],
            'mutations_red':mutants,'legacy_f4_frames':144,'host_replay':replay,
            'capability_combinations':4,'platform_checks':'complete'}
    (LOGS/'f5_validation_results.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print('F5 VALIDATION COLLECT PASS '+json.dumps(result),flush=True)

def main():
    p = argparse.ArgumentParser()
    names=['research','models','bin','frame','mutations','protocols','game','replay','lease','legacy','legacy_all','platform','platform_hw','ddr_log','collect']
    for name in names:p.add_argument('--'+name.replace('_','-'),dest=name,action='store_true')
    a = p.parse_args()
    BUILD.mkdir(parents=True, exist_ok=True)
    LOGS.mkdir(exist_ok=True)
    if not any(getattr(a,name) for name in names):p.error('至少选择一组判据')
    selected=[name for name in names if getattr(a,name)]
    if any(name in selected for name in ['bin','mutations']) and 'models' not in selected:
        selected.insert(0,'models')
    for name in selected:
        session().perform('f5.'+name,globals()[name])
    print('F5 CHECK PASS scope='+session().profile+' run='+str(session().path.relative_to(ROOT)),flush=True)

if __name__ == '__main__':
    try:main()
    except IncompleteCheck as error:print('INCOMPLETE '+str(error),file=sys.stderr);sys.exit(3)
