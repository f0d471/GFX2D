#!/usr/bin/env python3
"""R4 physical B7: 3 scene CRCs, CPU/HW equivalence, mutation, switches and 60-second cadence."""
import argparse,json,os,shlex,sys
from pathlib import Path
from check_run import ROOT,session,command,write_json,IncompleteCheck
from check_board_smoke import uart_port
from feature_synthesis import collect
from r4_checks import build,flags,frames,compare,complete,health,performance,recording

def b7_tree(explicit):
    candidates=[Path(explicit)] if explicit else sorted(
        (ROOT/'Main/sim/build/check-runs').glob('*/build/feature-presets/B7'),reverse=True)
    for tree in candidates:
        if not (tree/'fpga/gfx2d_ti60/outflow/gfx2d_ti60.bit').exists():continue
        differences=[]
        for p in (tree/'rtl').rglob('*'):
            if p.suffix not in ('.v','.vh'):continue
            source=ROOT/'Main/rtl'/p.relative_to(tree/'rtl')
            if p.name in ('engine_feature.vh','engine_regs_offsets.vh'):continue
            if not source.is_file() or source.read_bytes()!=p.read_bytes():differences.append(str(p))
        if not differences:
            timing=collect(tree)
            config=json.loads((tree/'config.json').read_text())
            if int(str(config['caps']),0)!=0xd46f:continue
            return tree,timing
    raise IncompleteCheck('no built B7 matching current RTL; run r3-synthesis')

def main():
    current=session();p=argparse.ArgumentParser();p.add_argument('--tree');a=p.parse_args()
    if os.name!='nt':raise IncompleteCheck('r4-board uses Windows Efinity Python')
    logs=current.logs('r4-board');tree,timing=b7_tree(a.tree)
    bit=tree/'fpga/gfx2d_ti60/outflow/gfx2d_ti60.bit';current.artifact(bit)
    port=uart_port();count=12 if current.profile=='quick' else 60
    env=dict(os.environ)
    for key in ('EFINITY_HOME','PYTHONHOME','EFXPGM_HOME','EFXPT_HOME','EFXDBG_HOME',
                'EFXIPM_HOME','EFXSVF_HOME','EFXSERDESDBG_HOME','EFINITY_USER_DIR_INI'):
        env.pop(key,None)
    usb=command([Path(sys.executable).parents[2]/'pgm/bin/ftdi_pgm.bat','--scan_usb'],logs/'usb.log',env=env)
    if '10660a79' not in usb.lower():raise IncompleteCheck('Ti60 JTAG absent')
    sample_count=120 if current.profile=='quick' else 1800
    expected_dir=current.build('r4-reference');expected_dir.mkdir(exist_ok=True)
    # Generate the same current-source host references in WSL, without an external copied PASS.
    wsl_out='/mnt/'+expected_dir.drive[0].lower()+expected_dir.as_posix()[2:]
    results=[];expected={}
    for name,ladder,scene in [('l1',1,0),('l2',2,0),('cup',1,1)]:
        scene_count=240 if name=='l2' and current.profile!='quick' else count
        host_dir=current.build('r4')/('reference-'+name)
        rel=os.path.relpath(host_dir,ROOT/'Main/sw/apps/game/gfx2dGame').replace('\\','/')
        # build() creates the exact R4RP fixture; compile it on host with the same flags.
        binary=build(current,'target-'+name,flags(ladder,scene,frames=scene_count))
        tape_dir=current.build('r4')/('target-'+name)/'replay'
        host_dir.joinpath('replay').mkdir(parents=True,exist_ok=True)
        import shutil
        shutil.copyfile(tape_dir/'r4_replay.h',host_dir/'replay/r4_replay.h')
        script='make -C Main/sw/apps/game/gfx2dGame -j8 PLATFORM=host PERF=1 OBJDIR='+shlex.quote(rel)
        script+=' EXTRA_CFLAGS='+shlex.quote(flags(ladder,scene,frames=scene_count)+' -DGFX2D_R4_REPLAY=1')
        command(['wsl.exe','-e','bash','-c',script],logs/(name+'.host-build.log'))
        exe=host_dir/'gfx2dGame-perf';current.artifact(exe)
        wsl_exe='/mnt/'+exe.drive[0].lower()+exe.as_posix()[2:]
        body=command(['wsl.exe','-e','bash','-c','SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy GFX2D_UNPACED=1 '+shlex.quote(wsl_exe)],logs/(name+'.host.log'))
        expected[name]=frames(body,scene_count)
        def capture(binary,label,program=False,seconds=180):
            serial=logs/(label+'.serial.log')
            args=[sys.executable,ROOT/'Main/tools/board/board_run.py','--bin',binary,'--port',port,
                  '--stop-line','done','--seconds',str(seconds),'--log',serial]
            args+=['--bit',bit] if program else ['--no-program']
            control=command(args,logs/(label+'.control.log'),timeout=seconds+180)
            if str(binary.stat().st_size)+' bytes written at address' not in control:raise RuntimeError('R4 deployment identity')
            text=serial.read_text(encoding='utf-8-sig')
            marker='# --- 程序已加载，以下才是它的输出 ---'
            if marker not in text:raise RuntimeError('missing current load boundary')
            return text.split(marker,1)[1]
        text=capture(binary,name,program=name=='l1');health(text)
        if name=='l1':
            command([sys.executable,ROOT/'Main/tools/board/board_run.py','--no-program','--check-only'],
                    logs/'debug-twice.log')
        results.append(dict(case=name,**compare(text,expected[name])))
        sw=build(current,'software-'+name,flags(ladder,scene,hw=0,frames=2))
        text=capture(sw,'software-'+name)
        health(text);results.append(dict(case='software-'+name,**compare(text,expected[name][:2])))
        print('R4 BOARD PASS '+name,flush=True)
    negative=build(current,'pixel-injection',flags(frames=1)+' -DR4_INJECT_PIXEL')
    text=capture(negative,'pixel-injection')
    results.append(dict(case='pixel-injection',**compare(text,expected['l2'][:1],True)))
    perf=build(current,'performance',flags(frames=sample_count+31,judge=False))
    text=capture(perf,'performance',seconds=240)
    results.append(dict(case='performance',**performance(text,sample_count)))
    stress_keys=[(4 if i%120<30 else 8 if i%120<60 else 0)|
        (1 if i%160<40 else 2 if i%160<80 else 0)|(16 if i in (120,240) else 0)
        for i in range(sample_count+31)]
    stress=build(current,'performance-input',flags(frames=sample_count+31,judge=False),
        replay_keys=stress_keys)
    text=capture(stress,'performance-input',seconds=240)
    results.append(dict(case='performance-input',**performance(text,sample_count)))
    switches=build(current,'switches','-DGFX2D_FRAMES=60 -DGFX2D_START_BACKEND=2 '
        '-DGFX2D_PRESS_KNOB=64 -DGFX2D_R4_SWITCH_JUDGE=1')
    text=capture(switches,'switches',seconds=240);complete(text)
    if text.count('R4 LEGACY OFFSCREEN preserved')<5:raise RuntimeError('missing legacy offscreen ownership checks')
    for m in (1,2,3,4):
        if 'mode '+str(m)+' enter frame' not in text:raise RuntimeError('mode switch missing '+str(m))
    if 'r4 enter ladder/hw 1 0' not in text:raise RuntimeError('software toggle missing')
    if 'r4 enter ladder/hw 1 1' not in text or 'r4 enter ladder/hw 2 1' not in text:raise RuntimeError('ladder toggle missing')
    results.append(dict(case='switches',status='PASS'))
    record=build(current,'recording',flags(frames=8,judge=False)+' -DGFX2D_R4_RECORD=1')
    results.append(dict(case='recording',**recording(capture(record,'recording'),8)))
    game=build(current,'legacy-game','-DGFX2D_START_MODE=2 -DGFX2D_START_BACKEND=2 -DGFX2D_FRAMES=240')
    text=capture(game,'legacy-game',seconds=180);complete(text)
    import re
    legacy_rows=[r.split(',') for r in text.splitlines() if r.startswith('csv,')]
    if not legacy_rows or any(r[2:4]!=['game','hw-tex'] for r in legacy_rows):
        raise RuntimeError('legacy production backend changed')
    if not re.search(r'^csvsum,240,',text,re.M):raise RuntimeError('incomplete legacy production game')
    command([sys.executable,ROOT/'Main/tools/check/check_sw9_perf.py',logs/'legacy-game.serial.log',
        '--fields-min','240','--warmup-rows','1','--steady-drops-max','0'],logs/'legacy-game.validation.log')
    results.append(dict(case='legacy-game',status='PASS',frames=240,backend='hw-tex'))
    command([sys.executable,ROOT/'Main/tools/check/check_r3_board.py','--tree',tree],
            logs/'r3-error-recovery.log',timeout=720)
    results.append(dict(case='r3-physical-fault-recovery',status='PASS'))
    current.verify()
    write_json(logs/'expected.json',expected)
    write_json(logs/'result.json',dict(status='PASS',profile=current.profile,port=port,
        b7_tree=tree.relative_to(ROOT).as_posix(),timing=timing,results=results,
        scope='current B7 JTAG, current BINs; no Flash or human HDMI observation'))

if __name__=='__main__':
    try:main()
    except IncompleteCheck as e:print('INCOMPLETE '+str(e));sys.exit(3)
