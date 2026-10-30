#!/usr/bin/env python3
"""Fresh build + JTAG-only console application/clock acceptance (Windows)."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import sys
from check_run import ROOT,IncompleteCheck,session,command,identity,write_json
from console_checks import validate_board,validate_fault

MAIN=ROOT/'Main'
B7_DEFAULT=MAIN/'bit/snapshots/B7/gfx2d_ti60.bit'
BOUNDARY='# --- 程序已加载，以下才是它的输出 ---'

def input_script():
    events=[]
    def pulse(at,key):events.extend([(at,key),(at+150,0)])
    pulse(4000,128)       # menu -> demo, allow first precomposition
    pulse(24000,256)      # global game
    pulse(40000,512)      # global pressure
    pulse(56000,896)      # global 3D, never HOME
    pulse(76000,4096)     # C: private scene switch
    pulse(88000,2048)     # Back -> menu
    pulse(92000,1)        # UP wraps to system information
    pulse(93000,1024)     # Enter
    pulse(99000,16)       # Space returns from system information
    pulse(103000,2)      # DOWN -> first content
    pulse(104000,1024)   # menu confirmation -> demo
    pulse(124000,2048)   # final menu
    return ','.join('%d:%d'%x for x in sorted(events))

def judge(log_path):
    try:
        body=Path(log_path).read_text(encoding='utf-8',errors='replace')
        validate_board(body.split(BOUNDARY,1)[-1])
    except (OSError,RuntimeError) as exc:
        print('FAIL '+str(exc));return False
    print('CONSOLE LOG PASS applications/frames/errors/timer/background')
    return True

def main():
    current=session()
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--bit',type=Path,default=B7_DEFAULT)
    ap.add_argument('--port',default=os.environ.get('GFX_CHECK_PORT'))
    ap.add_argument('--no-run',action='store_true')
    ap.add_argument('--log',type=Path)
    ap.add_argument('--identity',type=Path)
    args=ap.parse_args()
    logs=current.logs('console-board');build=current.build('console-board')
    if args.no_run:
        if not args.log or not args.identity:
            raise IncompleteCheck('历史日志复核必须给出 --log 与部署身份 --identity')
        deployment=json.loads(args.identity.read_text(encoding='utf-8'))
        if identity(args.log)['sha256']!=deployment['serial_sha256']:
            raise RuntimeError('serial identity mismatch')
        if not judge(args.log):raise RuntimeError('console log failed')
        print('HISTORICAL REVIEW PASS; this does not establish live board state')
        return
    if os.name!='nt':raise IncompleteCheck('console-board 使用 Windows Efinity Python 与实体板卡')
    if not args.bit.is_file():raise IncompleteCheck('缺少明确选定的 B7 位流')
    if not args.port:
        sys.path.insert(0,str(MAIN/'tools/board'))
        from release_launch import uart
        args.port=uart()
    app=MAIN/'sw/apps/console'
    relative=os.path.relpath(build/'app',app).replace('\\','/')
    script=('toolchain="$(find "$HOME/opt" -path "*/bin/riscv-none-elf-gcc" -print -quit)"; '
            'test -n "$toolchain" || exit 3; make -C Main/sw/apps/console -j6 '
            'OBJDIR='+shlex.quote(relative)+' RISCV_BIN="${toolchain%gcc}" all')
    # Always call make, even when a binary already exists. The selected run's
    # source identity and make dependencies govern reuse, not file existence.
    command(['wsl.exe','--exec','bash','-c',script],logs/'build.log')
    binary=build/'app/console-O2-perf.bin'
    before={'bit':identity(args.bit),'bin':identity(binary)}
    serial=logs/'serial.log'
    command([sys.executable,MAIN/'tools/board/board_run.py','--bit',args.bit,'--bin',binary,
             '--port',args.port,'--precise-boundary','--seconds','155',
             '--input-script',input_script(),'--input-script-after','menu crc',
             '--log',serial],logs/'deploy.log',timeout=240)
    body=serial.read_text(encoding='utf-8',errors='replace')
    deploy=(logs/'deploy.log').read_text(encoding='utf-8',errors='replace')
    if BOUNDARY not in body:raise RuntimeError('missing precise program load boundary')
    if str(binary.stat().st_size)+' bytes written at address' not in deploy:
        raise RuntimeError('missing exact program write count')
    status=validate_board(body.split(BOUNDARY,1)[1])
    if before!={'bit':identity(args.bit),'bin':identity(binary)}:
        raise RuntimeError('deployment artifact changed during board test')
    for p in (args.bit,binary,serial):current.artifact(p)
    fault_results=[]
    for stage,key in [('frame',256),('enter',896)]:
        fault_out=build/('fault-'+stage)
        rel=os.path.relpath(fault_out,app).replace('\\','/')
        fault_script=('toolchain="$(find "$HOME/opt" -path "*/bin/riscv-none-elf-gcc" -print -quit)"; '
            'test -n "$toolchain" || exit 3; make -C Main/sw/apps/console -j6 '
            'OBJDIR='+shlex.quote(rel)+' INJECT=CONSOLE_'+stage.upper()+' RISCV_BIN="${toolchain%gcc}" all')
        command(['wsl.exe','--exec','bash','-c',fault_script],logs/('fault-'+stage+'.build.log'))
        binaries=list(fault_out.glob('console-*-inject*.bin'))
        if len(binaries)!=1:raise RuntimeError('ambiguous fault binary')
        candidate=binaries[0];fault_log=logs/('fault-'+stage+'.serial.log')
        candidate_identity=identity(candidate)
        command([sys.executable,MAIN/'tools/board/board_run.py','--no-program','--bin',candidate,
            '--port',args.port,'--precise-boundary','--seconds','28',
            '--input-script','2000:'+str(key)+',2150:0','--input-script-after','menu crc',
            '--log',fault_log],logs/('fault-'+stage+'.deploy.log'),timeout=100)
        fault_body=fault_log.read_text(encoding='utf-8',errors='replace')
        if BOUNDARY not in fault_body:raise RuntimeError('fault probe missing load boundary')
        recovered=validate_fault(fault_body.split(BOUNDARY,1)[1],stage)
        if identity(candidate)!=candidate_identity:raise RuntimeError('fault artifact changed during deployment')
        current.artifact(candidate);current.artifact(fault_log)
        fault_results.append(dict(stage=stage,status='PASS',menu_samples=len(recovered),
                                  expected_errors=1,bin=candidate_identity))
    current.verify()
    deployment=dict(bit=str(args.bit.relative_to(ROOT)),bin=str(binary.relative_to(ROOT)),
        identities=before,serial_sha256=identity(serial)['sha256'],port=args.port,
        deployment='JTAG volatile',flash_written=False,precise_boundary=True)
    write_json(logs/'deployment.json',deployment)
    write_json(logs/'result.json',dict(status='PASS',profile=current.profile,
        applications=['demo','stg','press','r4','about'],status_samples=len(status),
        injected_recovery=fault_results,
        scope='application navigation/rendering/error-free state/independent clock/background progress',
        excluded=['Flash boot','manual HDMI/physical controller observation','full N_max measurement']))
    print('CONSOLE BOARD PASS five apps, real frames, independent 1kHz and background progress')

if __name__=='__main__':
    try:main()
    except IncompleteCheck as exc:print('INCOMPLETE '+str(exc),file=sys.stderr);sys.exit(3)
    except (RuntimeError,ValueError) as exc:print('FAIL '+str(exc),file=sys.stderr);sys.exit(1)
