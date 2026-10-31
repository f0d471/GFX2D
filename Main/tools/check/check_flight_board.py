#!/usr/bin/env python3
"""Scene2 hardware pixels and real FreeRTOS console fixed-30Hz performance."""
import json,os,re,shlex,sys
from pathlib import Path
from check_run import ROOT,session,command,identity,write_json,IncompleteCheck
from r4_checks import build,flags,frames,compare,health,replay_header

MENU_SCRIPT='1200:2,1350:0,1700:2,1850:0,2200:2,2350:0,2700:2,2850:0,3200:1024,3350:0'


def main():
    if os.name!='nt':raise IncompleteCheck('flight-board uses Windows Efinity Python and a physical board')
    current=session();logs=current.logs('flight-board');out=current.build('flight-board')
    sys.path.insert(0,str(ROOT/'Main/tools/board'))
    from release_launch import uart
    port=os.environ.get('GFX_CHECK_PORT') or uart()
    bit=ROOT/'Main/bit/snapshots/B7/gfx2d_ti60.bit'
    meta=json.loads(bit.with_name('manifest.json').read_text(encoding='utf-8'))
    if identity(bit)['sha256']!=meta['bit_sha256']:raise RuntimeError('B7 identity mismatch')
    if int(meta['caps'],16)&0x8000==0:raise RuntimeError('RAST unavailable')
    count=12 if current.profile=='quick' else 60
    samples=120 if current.profile=='quick' else 1800
    selected=flags(1,2,frames=count)
    binary=build(current,'flight-functional',selected)
    app=ROOT/'Main/sw/apps/game/gfx2dGame'
    host=out/'host';rel=os.path.relpath(host,app).replace('\\','/')
    inc=os.path.relpath(replay_header(current,'flight-functional').parent,app).replace('\\','/')
    script='make -C Main/sw/apps/game/gfx2dGame -j4 PLATFORM=host PERF=1 OBJDIR='+shlex.quote(rel)+\
        ' EXTRA_CFLAGS='+shlex.quote(selected+' -DGFX2D_R4_REPLAY=1')+' APP_CPPFLAGS='+shlex.quote('-I'+inc)
    command(['wsl.exe','-e','bash','-c',script],logs/'host.build.log')
    host_exe=host/'gfx2dGame-perf'
    wsl_exe='/mnt/'+host_exe.drive[0].lower()+host_exe.as_posix()[2:]
    body=command(['wsl.exe','-e','env','SDL_VIDEODRIVER=dummy','SDL_AUDIODRIVER=dummy',
                  'GFX2D_UNPACED=1',wsl_exe],logs/'host.log')
    expected=frames(body,count)

    def capture(candidate,name,program=False,console=False):
        serial=logs/(name+'.serial.log')
        args=[sys.executable,ROOT/'Main/tools/board/board_run.py','--bin',candidate,'--port',port,
              '--precise-boundary','--seconds','140','--log',serial]
        args+=['--bit',bit] if program else ['--no-program']
        if console:args+=['--stop-on','console leave flight','--input-script',MENU_SCRIPT,'--input-script-after','menu crc']
        else:args+=['--stop-line','done']
        deploy=command(args,logs/(name+'.control.log'),timeout=320)
        if str(candidate.stat().st_size)+' bytes written at address' not in deploy:raise RuntimeError('program write count')
        text=serial.read_text(encoding='utf-8-sig')
        marker='# --- 程序已加载，以下才是它的输出 ---'
        if marker not in text:raise RuntimeError('missing load boundary')
        current.artifact(candidate);current.artifact(serial)
        return text.split(marker,1)[1]

    results=[]
    text=capture(binary,'hardware',program=True);health(text)
    results.append(dict(case='flight-hardware-pixels',**compare(text,expected)))
    software=build(current,'flight-software',flags(1,2,hw=0,frames=2))
    text=capture(software,'board-software');health(text)
    results.append(dict(case='flight-board-software',**compare(text,expected[:2])))
    injected=build(current,'flight-pixel-injection',flags(1,2,frames=1)+' -DR4_INJECT_PIXEL')
    text=capture(injected,'pixel-injection');health(text)
    results.append(dict(case='flight-pixel-injection',**compare(text,expected[:1],True)))

    console=ROOT/'Main/sw/apps/console'
    for label,extra in [('normal',''),('input',' -DGFX2D_FLIGHT_INPUT_TEST=1')]:
        target=out/('console-'+label);rel=os.path.relpath(target,console).replace('\\','/')
        opts='-DGFX2D_FLIGHT_LIMIT='+str(samples+31)+extra
        script=('riscvcc="$(find "$HOME/opt" -path "*/bin/riscv-none-elf-gcc" -print -quit)"; '
                'test -n "$riscvcc" || exit 3; make -C Main/sw/apps/console -j4 PLATFORM=target APP_PROFILE=full '
                'OBJDIR='+shlex.quote(rel)+' RISCV_BIN="${riscvcc%gcc}" EXTRA_CFLAGS='+shlex.quote(opts))
        built=command(['wsl.exe','-e','bash','-c',script],logs/(label+'.build.log'))
        if re.search(r'\bwarning:|\berror:',built):raise RuntimeError('console build warnings')
        text=capture(target/'console-O2-perf.bin','performance-'+label,console=True);health(text)
        if re.search(r'^console (error|fatal)',text,re.M):raise RuntimeError('console error')
        if not re.search(r'console status .*app flight id 5 backend hw .*errors 0 scene 2 ladder 1',text):
            raise RuntimeError('wrong active application/backend')
        rows=re.findall(r'^R4PERF ([\d ]+)\s*$',text,re.M)
        if len(rows)!=1:raise RuntimeError('performance summary missing')
        v=list(map(int,rows[0].split()))
        if len(v)!=9 or v[0]!=samples or v[3]>3333333 or v[4] or v[5]!=samples or v[6] or v[7]!=samples*2 or v[8]!=samples:
            raise RuntimeError('FreeRTOS flight cadence/budget failed: '+str(v))
        cache=re.findall(r'^R4FCACHE (\d+) (\d+) (\d+) (\d+) (\d+)\s*$',text,re.M)
        if not cache or any(int(a)!=1 or int(b) or int(e) for a,b,_,_,e in cache):raise RuntimeError('prewarm debt/fallback')
        results.append(dict(case='console-30hz-'+label,status='PASS',samples=samples,p50=v[1],p99=v[2],
            maximum=v[3],fields=v[7],presents=v[8],late=v[4],cache=cache[-1]))
    current.verify()
    write_json(logs/'result.json',dict(status='PASS',profile=current.profile,port=port,bit=identity(bit),
        scope='current flight software on frozen B7; hardware pixels and real console timing; no Flash',results=results))
    print('FLIGHT BOARD PASS hardware pixels/FreeRTOS timing/input/prewarm')


if __name__=='__main__':
    try:main()
    except IncompleteCheck as exc:print('INCOMPLETE '+str(exc));sys.exit(3)
