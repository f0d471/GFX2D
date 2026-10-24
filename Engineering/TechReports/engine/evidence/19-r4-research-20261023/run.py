"""Reproducible R4 research sampling; does not implement the demo."""
import hashlib,json,os,re,subprocess,sys
from pathlib import Path

ROOT=Path.cwd();OUT=Path(__file__).resolve().parent
sys.path.insert(0,str(ROOT/'Main/tools/check'))
from feature_synthesis import collect
from check_board_smoke import uart_port
TREE=ROOT/'Main/sim/build/check-runs/20261023-193434-5007f558/build/feature-presets/B7'
BIT=TREE/'fpga/gfx2d_ti60/outflow/gfx2d_ti60.bit'
def write(name,data):
    (OUT/name).write_text(json.dumps(data,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
def run(args,name,env=None,timeout=300):
    p=subprocess.run(list(map(str,args)),cwd=ROOT,env=env,capture_output=True,timeout=timeout)
    b=p.stdout+p.stderr;(OUT/name).write_bytes(b)
    print(name,'exit',p.returncode,flush=True)
    if p.returncode:raise RuntimeError(name+' failed: '+b[-1500:].decode('utf-8','replace'))
    return b.decode('utf-8-sig','replace')
def identity(p):
    b=p.read_bytes();return dict(path=p.relative_to(ROOT).as_posix(),bytes=len(b),sha256=hashlib.sha256(b).hexdigest())
def inputs():
    files=[* (ROOT/'Main/sw/lib').rglob('*.c'),* (ROOT/'Main/sw/lib').rglob('*.h'),
        * (ROOT/'Main/sw/lib').rglob('*.inc'),* (ROOT/'Main/sw/bsp').rglob('*.c'),
        * (ROOT/'Main/sw/bsp').rglob('*.h'),* (ROOT/'Main/sw/mk').glob('*.mk'),
        ROOT/'Main/sw/bsp/ti60/linker/gfx2d.ld',OUT/'probe.c',OUT/'run.py',BIT,
        ROOT/'Main/tools/board/board_run.py',ROOT/'Main/tools/board/gfx2d_debug.cfg']
    return [identity(p) for p in sorted(set(files))]
def main():
    stage=sys.argv[1]
    if stage=='prepare':
        before=inputs();write('inputs.json',before)
        write('plan.json',dict(scope='R4 predevelopment research, synthetic sampling only',profile='research',
            production_changes=False,checks=['unchanged B7 timing and RTL identity','JTAG/UART/debug',
            'host-reference fixture CRC','target CPU plus integrated raster sample','120-frame cadence sample'],
            excluded=['R4 application acceptance','new bitstream synthesis','Flash','human HDMI observation',
                      '60-second demo acceptance','actual cup3d performance']))
        timing=collect(TREE)
        differences=[]
        for p in (TREE/'rtl').rglob('*'):
            if p.is_file() and p.suffix in ('.v','.vh'):
                original=ROOT/'Main/rtl'/p.relative_to(TREE/'rtl')
                if not original.exists() or original.read_bytes()!=p.read_bytes():differences.append(p.relative_to(TREE).as_posix())
        write('b7.json',dict(tree=TREE.relative_to(ROOT).as_posix(),timing=timing,
                           rtl_differences=differences,bitstream=identity(BIT)))
        print('B7 RTL differences:',differences,flush=True)
        wsl='/mnt/'+ROOT.drive[0].lower()+ROOT.as_posix()[2:]
        out=OUT.relative_to(ROOT).as_posix()
        script='r4cc="$(find "$HOME/opt" -path "*/bin/riscv-none-elf-gcc" -print -quit)"; test -n "$r4cc" || exit 3; '
        script+='make -C Main/sw -f mk/target.mk -j8 APP=r4Probe APP_SRCDIR=../sim/build/r4-research-20261023 APP_SRCS=probe.c LIBS="core gfx" OBJDIR=../sim/build/r4-research-20261023/target RISCV_BIN="${r4cc%gcc}"'
        run(['wsl.exe','-e','bash','-c',script],'target-build.log')
        run(['wsl.exe','-e','bash','-c',
            'make -C Main/sw -f mk/host.mk -j8 APP=r4Probe APP_SRCDIR=../sim/build/r4-research-20261023 APP_SRCS=probe.c LIBS="core gfx" OBJDIR=../sim/build/r4-research-20261023/host REPLAYS='],'host-build.log')
        run(['wsl.exe','-e','bash','-c','SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy '+out+'/host/r4Probe'],'host.log')
        # Scan and debug use the same driver stack as deployment.
        env=dict(os.environ)
        for k in ['EFINITY_HOME','PYTHONHOME','EFXPGM_HOME','EFXPT_HOME','EFXDBG_HOME','EFXIPM_HOME','EFXSVF_HOME','EFXSERDESDBG_HOME','EFINITY_USER_DIR_INI']:env.pop(k,None)
        run([Path(sys.executable).parents[2]/'pgm/bin/ftdi_pgm.bat','--scan_usb'],'usb.log',env)
        port=uart_port();write('port.json',dict(port=port))
        run([sys.executable,ROOT/'Main/tools/board/board_run.py','--check-only','--bit',BIT], 'debug.log')
        write('artifacts.json',[identity(OUT/'target/r4Probe-O2.bin'),identity(OUT/'target/r4Probe-O2.elf'),identity(OUT/'host/r4Probe')])
        if before!=inputs():raise RuntimeError('research inputs changed')
    elif stage=='board':
        if json.loads((OUT/'inputs.json').read_text(encoding='utf-8'))!=inputs():raise RuntimeError('changed inputs')
        port=json.loads((OUT/'port.json').read_text())['port']
        run([sys.executable,ROOT/'Main/tools/board/board_run.py','--no-program','--bin',OUT/'target/r4Probe-O2.bin',
            '--port',port,'--stop-line','done','--seconds','240','--log',OUT/'serial.log'],'control.log',timeout=330)
        body=(OUT/'serial.log').read_text(encoding='utf-8-sig')
        marker='# --- 程序已加载，以下才是它的输出 ---'
        if marker not in body:raise RuntimeError('missing load boundary')
        body=body.split(marker,1)[1]
        if '# serial lost' in body or not re.search(r'^done\s*$',body,re.M):raise RuntimeError('incomplete capture')
        if re.findall(r'^fail (\d+)\s*$',body,re.M)!=['0']:raise RuntimeError('probe failed')
        def rows(kind,text):return [[int(x) for x in s.split()] for s in re.findall('^'+kind+r' (.+)$',text,re.M)]
        host=(OUT/'host.log').read_text(encoding='utf-8')
        if rows('CRC',body)!=rows('CRC',host):raise RuntimeError('fixture CRC mismatch')
        samples=rows('SAMPLE',body);cad=rows('CADENCE',body)
        if len(samples)!=24 or len(cad)!=120:raise RuntimeError('missing samples')
        times=sorted(r[1] for r in cad)
        write('result.json',dict(status='PASS',scope='synthetic research probes, not R4 acceptance',
            workload=rows('WORK',body),samples=samples,crc=rows('CRC',body),health=rows('HEALTH',body),
            cadence=dict(frames=len(cad),p50=times[59],p99=times[118],maximum=max(times),
                exact_due=sum(r[2]==r[3] for r in cad),late=sum(r[3]>r[2] for r in cad),
                over_budget=sum(r[1]>BOARD_BUDGET for r in cad)),
            inputs_unchanged=json.loads((OUT/'inputs.json').read_text(encoding='utf-8'))==inputs()))
        print((OUT/'result.json').read_text(encoding='utf-8'))
BOARD_BUDGET=3333333
if __name__=='__main__':main()
