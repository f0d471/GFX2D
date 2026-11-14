#!/usr/bin/env python3
"""R2 单元、端口、真实搬运层十万条与独立变异检查。"""
import argparse
import json
from pathlib import Path
import subprocess
import sys

from check_run import ROOT, command, session, write_json
from check_rast import SOURCES

E = 'Main/rtl/engine/'
S = 'Main/sim/engine/'
FULL = [S+'rast/tb_engine_rast.v', E+'rast/engine_rlst.v', E+'rast/engine_rast.v',
        E+'core/engine_mover.v', E+'buffer/engine_band.v', E+'buffer/engine_tex.v',
        'Main/rtl/vendor/verilog-axi/axi_dma_rd.v', 'Main/rtl/vendor/verilog-axi/axi_dma_wr.v',
        'Main/sim/axi_probe/axi_ram_model.v']
FLAGS = ['GFX_HAS_RAST', 'GFX_HAS_BAND_HW', 'GFX_HAS_TEX']


def main():
    current = session()
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--group', required=True, choices=('rlst', 'ports', 'rtl'))
    a = p.parse_args()
    build, logs = current.build('r2-'+a.group), current.logs('r2-'+a.group)
    results = []

    def compile_case(name, top, sources, flags=()):
        exe = build/(name+'.vvp')
        body = command(['iverilog','-g2012','-Wportbind','-Wselect-range','-Wimplicit',
                        '-I',E+'include','-s',top,*['-D'+f for f in flags],'-o',exe,*sources], logs/(name+'.compile.log'))
        assert 'warning:' not in body.lower(), body
        return exe

    def run(name, exe, args=(), mutation=False):
        log = logs/(name+'.log')
        try:
            body = command(['vvp','-n',exe,*args],log,timeout=1800)
            failed = False
        except subprocess.CalledProcessError:
            body = log.read_text()
            failed = True
        if mutation:
            assert failed and 'FATAL:' in body and 'watchdog' not in body.lower(), (name,body[-2000:])
            print('MUTATION DETECTED '+name+' '+body.split('FATAL:')[-1].splitlines()[0],flush=True)
        else:
            assert not failed and 'RESULT: PASS ' in body, (name,body[-2000:])
            print(name+' '+next(x for x in body.splitlines() if x.startswith('RESULT:')),flush=True)
        results.append({'name':name,'expected_failure':mutation,'log':log.relative_to(ROOT).as_posix()})

    rlst_src=[S+'rlst/tb_engine_rlst.v',E+'rast/engine_rlst.v']
    core_src=[S+'rast/tb_rast_core.v',E+'rast/engine_rast.v']
    port_src=[S+'rast/tb_rast_tex_port.v',E+'buffer/engine_tex.v']
    if a.group=='rlst':
        run('normal',compile_case('normal','tb_engine_rlst',rlst_src))
    elif a.group=='ports':
        run('core',compile_case('core','tb_rast_core',core_src))
        for lat in (1,2):
            flags=['GFX_HAS_RAST_TEX']+(['RAST_TEX_LAT2'] if lat==2 else [])
            run(f'tex-lat{lat}',compile_case(f'tex-lat{lat}','tb_rast_tex_port',port_src,flags))
            run(f'core-tex-lat{lat}',compile_case(f'core-tex-lat{lat}','tb_rast_core',
                core_src+[E+'buffer/engine_tex.v'],flags))
        # 新模块尚未进入生产预设；关闭态由真实源文件清单而非宏名字推断。
        import feature_build as fb
        import feature_config as fc
        for preset in fc.PRESETS:
            sources=fb.enabled_rtl(fc.parse_name(preset))
            present=any(Path(x).stem in ('engine_rlst','engine_rast') for x in sources)
            assert present==bool(fc.parse_name(preset)['RAST'])
        print('CLOSED PASS raster sources follow RAST; legacy TEX retained by L1',flush=True)
    else:
        library=build/'reference.so'
        command(['gcc','-std=gnu11','-O2','-Wall','-Wextra','-Werror','-fPIC','-shared',
                 '-IMain/sw/lib',*SOURCES,'-o',library],logs/'reference-build.log')
        for name,extra in [('directed',['--directed']),('random',['--count','100000' if current.profile!='quick' else '200'])]:
            body=command([sys.executable,'Main/tools/check/rast_stream.py','--reference-lib',library,
                          '--out',build/name/'frame',*extra],logs/(name+'-vectors.log'))
            assert 'EXPORT PASS' in body and body.count('CROSS-C PASS') >= 2
            print(body.strip(),flush=True)
        binaries={}
        def vector_args(vector,l1=False,off=0):
            base=build/vector/('frame_l1' if l1 else 'frame')
            stats=json.loads((build/vector/'frame.stats.json').read_text())
            commits=stats['l1_commits' if l1 else 'l2_commits']
            return [f'+MAIN={base}.main.bin',f'+FRAME={base}.frame.bin',f'+EXP={base}.exp.bin',
                    f'+TRIOFF={off}',f'+COMMITS={commits}']
        for name,flags in [('l1',FLAGS),('l2',FLAGS+['GFX_HAS_RAST_TEX']),
                           ('l2-lat2',FLAGS+['GFX_HAS_RAST_TEX','RAST_TEX_LAT2'])]:
            binaries[name]=compile_case(name,'tb_engine_rast',FULL,flags)
            run(name+'-random',binaries[name],vector_args('random',name=='l1'))
            run(name+'-directed',binaries[name],vector_args('directed',name=='l1',4000))
        for off in (4032,4064):run(f'split-{off}',binaries['l2'],vector_args('directed',False,off))
        # 十三个变异体已移到 Main/sim/mutation/mutants.yaml；它们的发现者是上面的
        # 随机/定向/跨页用例（RAST 系）与 rlst/ports 组（RLST 系）。
        print('R2 变异体已移到 Main/sim/mutation/mutants.yaml',flush=True)
    write_json(logs/'summary.json',{'scope':a.group,'profile':current.profile,'cases':results})
    print(f'R2 {a.group} PASS cases={len(results)}',flush=True)


if __name__=='__main__':main()
