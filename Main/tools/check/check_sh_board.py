#!/usr/bin/env python3
"""Frozen B7 SH software/hardware pixels and real frame-counted timing. JTAG only."""
import json
import hashlib
import os
from pathlib import Path
import re
import sys
import threading
import time
from check_run import ROOT,IncompleteCheck,command,session,write_json


def main():
    if os.name!='nt':raise IncompleteCheck('sh-board needs the Windows JTAG/UART device')
    current=session();logs=current.logs('sh-board');build=current.build('sh-board')
    sys.path.insert(0,str(ROOT/'Main/tools/board'))
    import board_run
    from input_forward import packet
    from build_pc import wsl_path
    bit=ROOT/'Main/bit/snapshots/B7/gfx2d_ti60.bit'
    port=os.environ.get('GFX_CHECK_PORT')
    if not port:raise IncompleteCheck('set GFX_CHECK_PORT after discovering the FT4232H UART')
    current.artifact(bit)
    reuse=os.environ.get('GFX_SH_REUSE_RUN')
    reuse_root=None
    if reuse:
        reuse_root=(ROOT/reuse).resolve()
        old=json.loads((reuse_root/'run.json').read_text(encoding='utf-8'))['inputs']
        fresh=current.read()['inputs']
        prefixes=('Main/sw/','Main/sdk/','Main/assets/','Main/tools/assets/','Main/tools/board/')
        producer=lambda inputs:{p:d['sha256'] for p,d in inputs.items() if p.startswith(prefixes)}
        if producer(old)!=producer(fresh):raise RuntimeError('cached firmware producer inputs changed')
        write_json(logs/'build-reuse.json',dict(scope='compile artifacts only; physical board tests rerun',
            previous_run=reuse,verified_producer_files=len(producer(fresh))))
    original_ocd=board_run.run_openocd
    def fast_ocd(ocd,cfg,workspace,commands,timeout=90):
        return original_ocd(ocd,cfg,workspace,['adapter speed 6000',*commands],timeout=timeout)
    board_run.run_openocd=fast_ocd
    def firmware(name,flags=''):
        if reuse_root:
            out=reuse_root/'build/sh-board'/name
            meta=json.loads((out/'firmware.json').read_text(encoding='utf-8'))
            actual=set(token for token in (out/'.cflags').read_text(encoding='utf-8').split()
                if token.startswith(('-DSH_','-DR4_BOARD_CRC','-DR4_INJECT_PIXEL','-DGFX2D_R4_JUDGE')))
            if actual!=set(flags.split()):raise RuntimeError('cached firmware flags differ '+name)
            for filename,identity in meta['files'].items():
                if hashlib.sha256((out/filename).read_bytes()).hexdigest()!=identity['sha256']:
                    raise RuntimeError('cached firmware artifact changed '+filename)
            for filename in ['console-O2-perf.bin','console-O2-perf.elf','firmware.json','content.json']:
                current.artifact(out/filename)
            return out/'console-O2-perf.bin'
        out=build/name
        command(['wsl.exe','-e','env','EXTRA_CFLAGS='+flags,'python3',
                 wsl_path(ROOT/'Main/tools/board/build_console.py'),'--out',wsl_path(out)],
                 logs/(name+'.build.log'),timeout=600)
        for file in ['console-O2-perf.bin','console-O2-perf.elf','firmware.json','content.json']:
            current.artifact(out/file)
        return out/'console-O2-perf.bin'
    def run(name,binary,seconds=180,program=False,stop=None,inspect=True):
        args=['board_run','--bin',str(binary),'--port',port,'--precise-boundary',
              '--seconds',str(seconds),'--log',str(logs/(name+'.serial.log'))]
        args+=['--bit',str(bit)] if program else ['--no-program']
        if stop:args+=['--stop-line',stop,'--input-script','300:16384,700:0','--input-script-after','console enter menu']
        old=sys.argv;sys.argv=args
        try:
            with (logs/(name+'.loader.log')).open('w',encoding='utf-8') as output:
                from contextlib import redirect_stdout,redirect_stderr
                with redirect_stdout(output),redirect_stderr(output):rc=board_run.main()
        finally:sys.argv=old
        if rc:raise RuntimeError('board loader failed '+name)
        body=(logs/(name+'.serial.log')).read_text(encoding='utf-8',errors='replace')
        if '# --- 程序已加载，以下才是它的输出 ---' not in body:raise RuntimeError('missing exact load boundary')
        from console_checks import clean
        if inspect:
            fresh=body.split('# --- 程序已加载，以下才是它的输出 ---',1)[1]
            if stop:
                end=re.search(r'(?m)^'+re.escape(stop)+r'\r?$',fresh)
                if not end:raise RuntimeError('complete criterion line missing '+name)
                fresh=fresh[:end.end()]+'\n'
            clean(fresh)
        return body
    common='-DSH_BOARD_CAPTURE=2 -DGFX2D_R4_JUDGE=1 -DR4_BOARD_CRC=1'
    normal=firmware('production')
    hw=firmware('capture-hw',common)
    sw=firmware('capture-sw',common+' -DSH_BOARD_SOFTWARE=1')
    mutant=firmware('capture-pixel',common+' -DR4_INJECT_PIXEL=1')
    captures={}
    for name,binary in [('hardware',hw),('software',sw),('pixel-mutation',mutant)]:
        body=run(name,binary,program=name=='hardware',stop='SHDONE 0')
        rows=[list(map(int,line.split()[1:])) for line in body.splitlines() if line.startswith('SHCRC ')]
        if len(rows)!=2 or any(row[-1] for row in rows):raise RuntimeError('missing capture frames '+name)
        captures[name]=rows
    if captures['hardware']!=captures['software']:raise RuntimeError('B7 HW/software SH pixels differ')
    if all(a[3]==b[3] for a,b in zip(captures['hardware'],captures['pixel-mutation'])):
        raise RuntimeError('injected pixel error escaped the framebuffer comparison')
    write_json(logs/'pixels.json',dict(status='PASS',captures=captures,mutation='EXPECTED_RED pixel CRC mismatch'))

    wall_flags='-DSH_BOARD_CAPTURE=4 -DSH_BOARD_WALL=1 -DGFX2D_R4_JUDGE=1 -DR4_BOARD_CRC=1'
    wall={}
    for name,flags in [('wall-hardware',wall_flags),('wall-software',wall_flags+' -DSH_BOARD_SOFTWARE=1')]:
        body=run(name,firmware(name,flags),stop='SHDONE 0')
        wall[name]=[list(map(int,line.split()[1:])) for line in body.splitlines() if line.startswith('SHCRC ')]
        if len(wall[name])!=4 or any(row[-1] for row in wall[name]):raise RuntimeError('missing wall/fire/reload capture '+name)
    if wall['wall-hardware']!=wall['wall-software']:raise RuntimeError('independent weapon Z differs at wall/fire/reload')
    if len({row[3] for row in wall['wall-hardware']})<3:raise RuntimeError('wall weapon poses did not change')
    write_json(logs/'wall-pixels.json',dict(status='PASS',captures=wall,scope='near wall, idle/fire/reload, independent Z'))

    # Frame-counted collector: UART is open before load, input heartbeat keeps
    # snapshots alive, and Escape is sent only after the required real frames.
    original_worker=board_run.serial_worker
    target_frames=120 if current.profile=='quick' else 1800
    metrics={}
    def timing(name,shortcut,app,frames,active=False):
        observed={'lines':[]};stop_event=threading.Event()
        def collect(port,baud,path,seconds,stop_on,ready,armed,result,forward=False,script=None,
                    script_after=None,stop_line=None,boundary_ready=None):
            import serial
            sp=None;nbytes=0;marked=False;partial='';started=False;finishing=False
            launch_end=0;next_send=0;start=0
            try:
                sp=serial.Serial(port,baud,timeout=.02);sp.reset_input_buffer();ready.set()
                end=time.monotonic()+seconds
                with open(path,'w',encoding='utf-8',newline='') as output:
                    output.write('# frame-counted SH timing, UART '+port+'\n')
                    while time.monotonic()<end and not stop_event.is_set():
                        now=time.monotonic()
                        if armed.is_set() and not marked:
                            sp.reset_input_buffer();partial='';marked=True
                            output.write('# --- 程序已加载，以下才是它的输出 ---\n');output.flush()
                            if boundary_ready:boundary_ready.set()
                        if marked and now>=next_send:
                            keys=shortcut if now<launch_end else 0
                            if started and active and not finishing:
                                keys=[0x11,0x18,0x12,0x14][int((now-start)/3)%4]
                            if finishing:keys=0x800
                            sp.write(packet(keys));next_send=now+.08
                        data=sp.read(4096)
                        if not data:continue
                        nbytes+=len(data);text=data.decode('utf-8','replace');output.write(text);output.flush()
                        if not marked:continue
                        partial+=text
                        while '\n' in partial:
                            line,partial=partial.split('\n',1);line=line.strip()
                            observed['lines'].append(line)
                            if line.startswith(('console error','console fatal','r4 err')):
                                raise RuntimeError('board application failed: '+line)
                            if line=='console enter menu':
                                if finishing:observed['complete']=True;stop_event.set();break
                                if started:raise RuntimeError('application unexpectedly returned to menu')
                                launch_end=time.monotonic()+.2
                            if line=='console enter '+app:started=True;start=time.monotonic()
                            if line.startswith('console status ') and ' app '+app+' ' in line:
                                m=re.search(r' frames (\d+)',line)
                                if m and int(m[1])>=frames:finishing=True
                    output.write('\n# collector complete='+str(bool(observed.get('complete')))+'\n')
            except Exception as exc:
                result['error' if not ready.is_set() else 'read_error']=str(exc)
            finally:
                if sp:
                    try:sp.write(packet(0));sp.close()
                    except Exception:pass
                result['bytes']=nbytes;result['stopped']=bool(observed.get('complete'));ready.set()
        board_run.serial_worker=collect
        try:body=run(name,normal,seconds=600,inspect=False)
        finally:board_run.serial_worker=original_worker
        body='\n'.join(observed['lines'])+'\n'
        from console_checks import clean
        clean(body)
        if not observed.get('complete'):raise IncompleteCheck('required frame window not completed '+name)
        health=[list(map(int,line.split()[1:])) for line in body.splitlines() if line.startswith('R4HEALTH ')]
        if not health or health[-1][2:]!=[0,0,0]:raise RuntimeError('display health failed '+name)
        perf=[list(map(int,line.split()[1:])) for line in body.splitlines() if line.startswith('R4PERF ')]
        if not perf or perf[-1][0]<frames-31:raise RuntimeError('timing window missing '+name)
        p=perf[-1]
        metrics[name]=dict(frames=p[0],p50_ms=p[1]/100000,p99_ms=p[2]/100000,max_ms=p[3]/100000,
            late=p[4],exact=p[5],over=p[6],fields=p[7],presents=p[8],
            measured_fps=60*p[8]/p[7] if p[7] else 0,health=health[-1])
        write_json(logs/'timing.json',metrics)
    timing('normal',0x4000,'sh',target_frames)
    timing('active',0x4000,'sh',target_frames,True)
    timing('corridor-consumer',0x380,'r4',60)
    current.verify()
    write_json(logs/'result.json',dict(status='PASS',scope='frozen B7 JTAG function and measured cadence; no Flash write',
        frames_per_sh_window=target_frames,timing=metrics,target_30hz_met=all(metrics[n]['late']==0 and
            metrics[n]['over']==0 for n in ('normal','active')),
        manual_hdmi_review='pending',physical_input_review='pending'))
    print('SH BOARD PASS pixels/input-driven windows/health; performance target reported separately')


if __name__=='__main__':
    try:main()
    except IncompleteCheck as exc:print('INCOMPLETE '+str(exc));sys.exit(3)
