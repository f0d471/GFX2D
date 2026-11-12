#!/usr/bin/env python3
"""R3 real engine_top frames, APB/terminal/reset contracts and mutations."""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys
import feature_config as fc
import feature_build as fb
from check_run import ROOT, session, command, write_json
from r3_vectors import generate


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--only',choices=('l1-min','l1','l2-min','l2','lat2'))
    p.add_argument('--frames',type=int)
    p.add_argument('--no-protocols',action='store_true')
    p.add_argument('--no-mutations',action='store_true')
    a=p.parse_args();current=session()
    build=current.build('r3');logs=current.logs('r3');rows=[]
    count=a.frames or (1 if current.profile=='quick' else 11)
    for name in ([a.only] if a.only else ['l1-min','l1','l2-min','l2','lat2']):
        tree=build/name/'rtl';shutil.copytree(ROOT/'Main/rtl',tree)
        config=fc.parse_name('B7');textured=not name.startswith('l1')
        config['RAST_TEX']=textured
        if name.endswith('-min'):
            config['INST']=False
            config.update(EQ_EXT=False,ALPHA=False,KEY=False,DELIVERY=False)
            if not textured:config['TEX']=False
        assert fc.is_legal(config)
        fb.write_feature_files(config,fc.name_of(config),tree/'engine')
        import regs_gen
        (tree/'engine/engine_regs_offsets.vh').write_text(regs_gen.gen_vh(fc.caps(config)))
        frames=1 if name.endswith('-min') else count
        vectors=build/name/'vectors';generate(vectors,frames,textured)
        excluded={Path(x).name for flag,files in fc.FEATURE_SOURCES.items()
                  if not config[flag] for x in files}
        sources=[x for x in sorted((tree/'engine').glob('**/*.v')) if x.name not in excluded]
        sources += [tree/'vendor/verilog-axi'/x for x in ('axi_dma_rd.v','axi_dma_wr.v')]
        sources += [ROOT/'Main/sim/axi_probe/axi_ram_model.v']

        def execute(tag,negative=False):
            binary=build/(tag+'.vvp')
            text=command(['iverilog','-g2012','-Wportbind','-Wimplicit','-Wselect-range',
                '-s','tb_r3','-I',tree/'engine',*(['-DR3_LAT2'] if name=='lat2' else []),
                '-o',binary,ROOT/'Main/sim/engine/r3/tb_r3.v',*sources],logs/(tag+'.compile.log'))
            if 'warning:' in text.lower():raise RuntimeError(text)
            args=['vvp','-n',binary,f'+VECTORS={vectors}',f'+FRAMES={1 if negative else frames}']
            if not a.no_protocols:args+=['+PROTOCOLS']
            try:
                text=command(args,logs/(tag+'.log'),timeout=3600)
                failed=False
            except subprocess.CalledProcessError:
                text=(logs/(tag+'.log')).read_text();failed=True
            if negative:
                if not failed or 'FATAL:' not in text or 'watchdog' in text.lower():
                    raise RuntimeError('mutation lacks a concrete witness: '+tag+'\n'+text[-1500:])
                print('R3 MUTATION DETECTED '+tag+' '+text.split('FATAL:')[-1].splitlines()[0],flush=True)
            elif failed or 'RESULT: PASS R3' not in text:
                raise RuntimeError(tag+'\n'+text[-2500:])
            else:print(next(x for x in text.splitlines() if 'RESULT: PASS R3' in x),flush=True)
            rows.append(dict(name=tag,mutation=negative,log=(logs/(tag+'.log')).relative_to(ROOT).as_posix()))
        execute(name)
        if name=='l2' and not a.no_mutations:
            jobs=[('owner','engine_top.v','.fd_raster(r3_fd_valid)',".fd_raster(1'b0)"),
                  ('shadow','rast/engine_r3.v','.tri_base(tri_base_r)','.tri_base(sh_tri)'),
                  ('stage','core/engine_cmd.v','err_stage<=7','err_stage<=0'),
                  ('early-done','core/engine_cmd.v',
                   "if(ras_active && {1'b0,strip_id}+6'd1==strips_r) st<=S_RFINAL;",
                   "if(ras_active && {1'b0,strip_id}+6'd1==strips_r) begin pend_done<=1;st<=S_TERM;end")]
            for tag,file,before,after in jobs:
                path=tree/'engine'/file;original=path.read_text()
                assert before in original,tag
                try:
                    path.write_text(original.replace(before,after))
                    execute('mutation-'+tag,True)
                finally:path.write_text(original)
    current.verify()
    write_json(logs/'result.json',dict(profile=current.profile,checks=rows,
        frames_per_variant=count,scope='R3 engine_top integration, not board or synthesis'))
    print('R3 PASS variants='+str(len(rows)),flush=True)


if __name__=='__main__':main()
