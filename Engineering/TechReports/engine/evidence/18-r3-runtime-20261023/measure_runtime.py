#!/usr/bin/env python3
"""One-time R3 recording experiment; uses existing production game and validators."""
import argparse,hashlib,json,os,re,shlex,sys
from pathlib import Path
ROOT=next(p for p in Path(__file__).resolve().parents if (p/'Main/tools/check/check_run.py').exists())
sys.path.insert(0,str(ROOT/'Main/tools/check'))
sys.path.insert(0,str(ROOT/'Main/tools/board'))
from check_run import command
from check_board_smoke import uart_port
from check_feature_synthesis import compiler_inputs
import board_run

def obj(path):
    path=Path(path)
    return dict(path=path.relative_to(ROOT).as_posix(),bytes=path.stat().st_size,
                sha256=hashlib.sha256(path.read_bytes()).hexdigest())

def main():
    p=argparse.ArgumentParser();p.add_argument('--tree',type=Path,required=True)
    p.add_argument('--out',type=Path,required=True);p.add_argument('--mode',choices=('nmax','counters'),required=True)
    p.add_argument('--prepare',action='store_true');a=p.parse_args()
    tree=a.tree.resolve();out=a.out.resolve();out.mkdir(parents=True,exist_ok=True)
    bit=tree/'fpga/gfx2d_ti60/outflow/gfx2d_ti60.bit'
    if a.mode=='nmax':
        app=ROOT/'Main/sw/apps/game/gfx2dGame';target=out/('build_prepare' if a.prepare else 'build_final')
        flags='-DGFX2D_START_MODE=3 -DGFX2D_START_BACKEND=2 -DGFX2D_PRESS_CLOCK=1 -DGFX2D_PRESS_EXIT=1'
        rel=os.path.relpath(target,app).replace('\\','/')
        script=('r3perf_cc="$(find "$HOME/opt" -path "*/bin/riscv-none-elf-gcc" -print -quit)"; '
                'test -n "$r3perf_cc" || exit 3; make -C Main/sw/apps/game/gfx2dGame -j8 PERF=1 '
                'OBJDIR='+shlex.quote(rel)+' EXTRA_CFLAGS='+shlex.quote(flags)+
                ' RISCV_BIN="${r3perf_cc%gcc}"')
        command(['wsl.exe','-e','bash','-c',script],out/'build.log')
        binary=target/'gfx2dGame-O2-perf.bin'
        if a.prepare:
            (out/'prepare.json').write_text(json.dumps(dict(flags=flags,binary=obj(binary)),indent=2)+'\n')
            print('PREPARED no board operations',flush=True);return
        inputs=[*compiler_inputs(tree).values(),binary,ROOT/'Main/assets/build/assets.pak']
        inputs += [x for x in (ROOT/'Main/sw').rglob('*') if x.is_file() and x.suffix in ('.c','.h','.mk','.ld')
                   and not any(v.startswith(('build','logs')) for v in x.relative_to(ROOT/'Main/sw').parts)]
        before=[obj(x) for x in dict.fromkeys(inputs)]
        port=uart_port();serial=out/'serial.log'
        control=command([sys.executable,ROOT/'Main/tools/board/board_run.py','--bit',bit,'--bin',binary,
            '--port',port,'--seconds','1500','--stop-line','done','--log',serial],out/'control.log',timeout=1600)
        if str(binary.stat().st_size)+' bytes written at address' not in control:raise RuntimeError('load bytes missing')
        raw=serial.read_text(encoding='utf-8-sig')
        boundary='# --- 程序已加载，以下才是它的输出 ---'
        if boundary not in raw or '# serial lost' in raw:raise RuntimeError('invalid serial')
        current=raw.split(boundary,1)[1]
        if len(re.findall(r'^nmax result ',current,re.M))!=2:raise RuntimeError('results not after current load')
        command([sys.executable,ROOT/'Main/tools/check/check_sw9_nmax.py',serial,'--status','OK'],out/'nmax-check.log')
        command([sys.executable,ROOT/'Main/tools/check/check_sw9_perf.py',serial,'--fields-min','7200'],out/'perf-check.log')
        rows=[x.split(',') for x in current.splitlines() if x.startswith('csv,')]
        if not rows or any(x[3]!='hw-tex' for x in rows):raise RuntimeError('wrong backend')
        for item in before:
            if obj(ROOT/item['path'])!=item:raise RuntimeError('changed input '+item['path'])
        results=re.findall(r'^nmax result OK n_max (\d+) knob (\d+) ft_max (\d+) ft_p99 (\d+)',current,re.M)
        result=dict(scope='recording only; no N_max acceptance threshold',backend='HW/TEX',flags=flags,
                    port=port,results=[dict(nmax=int(n),knob=int(k),ft_max=int(m),ft_p99=int(q)) for n,k,m,q in results],
                    historical_reference=[1891,2237],inputs=before)
    else:
        install=board_run.find_efinity();ocd=board_run.find_openocd(install)
        commands=['halt','mdw 0xf8100004 1','mdw 0xf810000c 1',
                  'mdw 0xf810011c 2','mdw 0xf8101040 2','resume','shutdown']
        ok,text=board_run.run_openocd(ocd,str(ROOT/'Main/tools/board/gfx2d_debug.cfg'),
                                    str(ROOT/'Main/sdk'),commands)
        (out/'openocd.log').write_text(text,encoding='utf-8')
        if not ok:raise RuntimeError('OpenOCD read failed')
        vals={int(addr,16):[int(n,16) for n in words.split()] for addr,words in
              re.findall(r'(?m)^0x([0-9a-f]+):\s*((?:[0-9a-f]{8}\s*)+)',text)}
        if vals.get(0xf8100004)!=[0xd46f] or vals.get(0xf810000c)!=[4]:raise RuntimeError('not completed B7 raster frame')
        wait,busy=vals[0xf810011c];cycles,refs=vals[0xf8101040]
        if not cycles or not refs:raise RuntimeError('missing raster counters')
        result=dict(scope='non-reset JTAG read after completed raster capture; diagnostic counters',
                    wait_ddr=wait,controller_busy=busy,raster_cycles=cycles,retired_refs=refs,
                    bitstream=obj(bit),commands=commands)
    (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({k:v for k,v in result.items() if k!='inputs'}),flush=True)

if __name__=='__main__':main()
