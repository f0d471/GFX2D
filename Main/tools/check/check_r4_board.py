#!/usr/bin/env python3
"""R4 physical B7: 2 scene CRCs (corridor L1/L2; cup removed 2026-11-02), CPU/HW equivalence, mutation, switches and 60-second cadence."""
import argparse,hashlib,json,os,re,shlex,sys
from pathlib import Path,PurePosixPath,PureWindowsPath
from check_run import ROOT,session,command,write_json,IncompleteCheck
from check_board_smoke import uart_port
from feature_synthesis import collect
from r4_checks import build,flags,frames,compare,complete,health,performance,recording,replay_header

def frozen_bit_identity(release):
    release=Path(release).resolve()
    manifest=release/'manifest.json'
    try:data=json.loads(manifest.read_text(encoding='utf-8-sig'))
    except (OSError,ValueError) as exc:raise RuntimeError('invalid frozen manifest: '+str(exc)) from exc
    if not isinstance(data,dict):raise RuntimeError('frozen manifest must be an object')
    if 'config' in data or 'bit_sha256' in data and 'files' not in data:
        try:valid=data.get('config')=='B7' and int(str(data.get('caps')),0)==0xd46f
        except ValueError:valid=False
        if not valid:raise RuntimeError('frozen snapshot must identify the B7 raster configuration')
        record=dict(path='gfx2d_ti60.bit',sha256=data.get('bit_sha256'),bytes=data.get('bit_bytes'))
    else:
        files=data.get('files')
        if not isinstance(files,list):raise RuntimeError('frozen release has no file identity list')
        selected=data.get('board_bit')
        names=(selected,) if selected else ('board/B7.bit','B7.bit')
        records=[x for x in files if isinstance(x,dict) and x.get('path') in names]
        if len(records)!=1:raise RuntimeError('frozen release must identify exactly one B7 bitstream')
        record=records[0]
        hardware=data.get('hardware')
        if hardware is not None:
            try:valid=isinstance(hardware,dict) and hardware.get('config')=='B7' and int(str(hardware.get('caps')),0)==0xd46f
            except ValueError:valid=False
            if not valid:raise RuntimeError('frozen package hardware is not B7 raster')
    name=record.get('path');expected=record.get('sha256')
    if not isinstance(name,str) or not name:raise RuntimeError('missing frozen bitstream path')
    relative=PurePosixPath(name.replace('\\','/'))
    if relative.is_absolute() or PureWindowsPath(name).is_absolute() or '..' in relative.parts:
        raise RuntimeError('frozen bitstream path escapes its manifest directory')
    bit=(release/Path(*relative.parts)).resolve()
    if not bit.is_relative_to(release) or bit.suffix!='.bit':raise RuntimeError('invalid frozen bitstream path')
    if not isinstance(expected,str) or not re.fullmatch(r'[0-9a-fA-F]{64}',expected):
        raise RuntimeError('invalid frozen bitstream SHA256')
    if not bit.is_file():raise RuntimeError('frozen bitstream is missing')
    if record.get('bytes') is not None and record['bytes']!=bit.stat().st_size:
        raise RuntimeError('frozen B7 bitstream byte count mismatch')
    actual=hashlib.sha256(bit.read_bytes()).hexdigest()
    if actual!=expected.lower():raise RuntimeError('frozen B7 bitstream identity mismatch')
    if data.get('files') and data.get('bit_sha256') and str(data['bit_sha256']).lower()!=actual:
        raise RuntimeError('package and file-list bitstream identities differ')
    return actual,manifest

def b7_tree(explicit,frozen_release=None):
    expected=frozen_bit_identity(frozen_release)[0] if frozen_release else None
    candidates=[Path(explicit)] if explicit else sorted(
        (ROOT/'Main/sim/build/check-runs').glob('*/build/feature-presets/B7'),reverse=True)
    for tree in candidates:
        if not (tree/'fpga/gfx2d_ti60/outflow/gfx2d_ti60.bit').exists():continue
        if expected:
            bit=tree/'fpga/gfx2d_ti60/outflow/gfx2d_ti60.bit'
            if hashlib.sha256(bit.read_bytes()).hexdigest()!=expected:continue
        differences=[]
        for p in (tree/'rtl').rglob('*'):
            if p.suffix not in ('.v','.vh'):continue
            source=ROOT/'Main/rtl'/p.relative_to(tree/'rtl')
            if p.name in ('engine_feature.vh','engine_regs_offsets.vh'):continue
            if not source.is_file() or source.read_bytes()!=p.read_bytes():differences.append(str(p))
        if not differences or expected:
            try:config=json.loads((tree/'config.json').read_text())
            except FileNotFoundError:continue
            if int(str(config['caps']),0)!=0xd46f:continue
            try:timing=collect(tree)
            except FileNotFoundError:continue
            return tree,timing
    raise IncompleteCheck('no built B7 with complete reports matching selected frozen identity' if expected else
                          'no built B7 with complete reports matching current RTL; run r3-synthesis')

def main():
    current=session();p=argparse.ArgumentParser();p.add_argument('--tree')
    p.add_argument('--frozen-release',type=Path,
        help='explicit software-only acceptance against an identity-verified frozen B7; does not accept current RTL')
    a=p.parse_args()
    if os.name!='nt':raise IncompleteCheck('r4-board uses Windows Efinity Python')
    logs=current.logs('r4-board');tree,timing=b7_tree(a.tree,a.frozen_release)
    if a.frozen_release:current.artifact(frozen_bit_identity(a.frozen_release)[1])
    bit=tree/'fpga/gfx2d_ti60/outflow/gfx2d_ti60.bit';current.artifact(bit)
    for report in ('gfx2d_ti60.place.rpt','gfx2d_ti60.timing.rpt','gfx2d_ti60.warn.log'):
        current.artifact(bit.parent/report)
    current.artifact(tree/'config.json')
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
    for name,ladder,scene in [('l1',1,0),('l2',2,0)]:
        scene_count=240 if name=='l2' and current.profile!='quick' else count
        host_dir=current.build('r4')/('reference-'+name)
        rel=os.path.relpath(host_dir,ROOT/'Main/sw/apps/game/gfx2dGame').replace('\\','/')
        # build() creates the exact R4RP fixture; compile it on host with the same flags.
        binary=build(current,'target-'+name,flags(ladder,scene,frames=scene_count))
        tape_dir=replay_header(current,'target-'+name).parent
        tape_relative=os.path.relpath(tape_dir,ROOT/'Main/sw/apps/game/gfx2dGame').replace('\\','/')
        script='make -C Main/sw/apps/game/gfx2dGame -j8 PLATFORM=host PERF=1 OBJDIR='+shlex.quote(rel)
        script+=' EXTRA_CFLAGS='+shlex.quote(flags(ladder,scene,frames=scene_count)+' -DGFX2D_R4_REPLAY=1')
        script+=' APP_CPPFLAGS='+shlex.quote('-I'+tape_relative)
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
    if not a.frozen_release:
        command([sys.executable,ROOT/'Main/tools/check/check_r3_board.py','--tree',tree],
                logs/'r3-error-recovery.log',timeout=720)
        results.append(dict(case='r3-physical-fault-recovery',status='PASS'))
    else:
        results.append(dict(case='current-RTL-r3-acceptance',status='NOT_APPLICABLE',
            reason='software-only work point uses unchanged, manifest-bound B7; current RTL is not rebuilt'))
    current.verify()
    write_json(logs/'expected.json',expected)
    write_json(logs/'result.json',dict(status='PASS',profile=current.profile,port=port,
        b7_tree=tree.relative_to(ROOT).as_posix(),timing=timing,results=results,
        frozen_release=str(a.frozen_release) if a.frozen_release else None,
        frozen_bit_sha256=frozen_bit_identity(a.frozen_release)[0] if a.frozen_release else None,
        scope=('frozen B7 / current software' if a.frozen_release else 'current B7 / current software')+
              '; no Flash or human HDMI observation'))

if __name__=='__main__':
    try:main()
    except IncompleteCheck as e:print('INCOMPLETE '+str(e));sys.exit(3)
