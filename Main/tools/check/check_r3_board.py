#!/usr/bin/env python3
"""R3 current B7 board: two-reference CRC, display health, physical fault/recovery and expected red."""
import argparse
import json
import os
from pathlib import Path
import re
import shlex
import sys
from check_run import ROOT, session, command, write_json, IncompleteCheck
from check_board_smoke import uart_port
from feature_synthesis import collect


def validate(body,expected,negative=False):
    marker='# --- 程序已加载，以下才是它的输出 ---'
    if marker not in body:raise RuntimeError('missing current load boundary')
    body=body.split(marker,1)[1]
    if '# serial lost' in body or not re.search(r'^done\s*$',body,re.M):
        raise RuntimeError('incomplete serial capture')
    if re.findall(r'^caps ([0-9a-f]{8})\s*$',body,re.M)!=['0000d46f']:
        raise RuntimeError('not the intended B7')
    if 'R3 id 52415301 caps 0000c000' not in body or 'DRIVER reject ok' not in body:
        raise RuntimeError('missing CSR/driver checks')
    rows=re.findall(r'^R3 frame ([0-9a-f]{8}) sw ([0-9a-f]{8}) hw ([0-9a-f]{8}) '
                    r'bad ([0-9a-f]{8}) cycles ([0-9a-f]{8}) code ([0-9a-f]{8})\s*$',body,re.M)
    if len(rows)!=(1 if negative else 11) or len({r[0] for r in rows})!=len(rows):
        raise RuntimeError('missing/duplicate frames')
    for frame,sw,hw,bad,cycles,code in rows:
        if int(frame,16)>=len(expected):raise RuntimeError('frame outside frozen cases')
        golden=expected[int(frame,16)]['crc']
        if sw!=golden or code!='00000000' or not int(cycles,16):
            raise RuntimeError('software golden/engine status failed')
        if negative:
            if hw==sw or int(bad,16)==0:raise RuntimeError('injected pixel escaped CRC/compare')
        elif hw!=sw or int(bad,16):raise RuntimeError('hardware CRC/pixel mismatch')
    health=re.findall(r'^HEALTH (before|mixed|after) field ([0-9a-f]{8}) min ([0-9a-f]{8}) '
                      r'zero ([0-9a-f]{8}) under ([0-9a-f]{8}) resp ([0-9a-f]{8})\s*$',body,re.M)
    if len(health)!=len(rows)+2 or health[0][0]!='before' or health[-1][0]!='after':
        raise RuntimeError('missing live display observations')
    if any(any(int(v,16) for v in r[3:]) for r in health):
        raise RuntimeError('display zero/underflow/response errors')
    fields=[int(r[1],16) for r in health[1:]]
    if any(a==b for a,b in zip(fields,fields[1:])):raise RuntimeError('stale display field')
    if not negative and ('ERROR index code 0000000d index 0000ffff tag 00000000 stage 00000007' not in body
                         or 'RESET recovery code 00000000' not in body):
        raise RuntimeError('missing physical error/reset/recovery')
    wanted='fail 1' if negative else 'fail 0'
    if [x.strip() for x in re.findall(r'^fail \d+\s*$',body,re.M)]!=[wanted]:
        raise RuntimeError('unexpected application result')
    return dict(status='EXPECTED_RED' if negative else 'PASS',frames=len(rows),
                cycles=[int(r[4],16) for r in rows],display_samples=len(health))


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--tree',help='B7 tree; default uses this run synthesis outputs')
    a=p.parse_args();current=session()
    if os.name!='nt':raise IncompleteCheck('R3 board uses Windows Efinity Python')
    logs=current.logs('r3-board');build=current.build('r3-board')
    tree=Path(a.tree).resolve() if a.tree else current.build('feature-presets')/'B7'
    timing=collect(tree)
    from check_feature_synthesis import compiler_inputs
    for pth in compiler_inputs(tree).values():current.artifact(pth)
    core=[x['slack_ns'] for x in timing['slacks'] if x['kind']=='setup' and
          x['clock']=='core_clk' and x['to_clock']=='core_clk']
    if not core or min(core)<0.3:raise RuntimeError('B7 core setup below +0.3ns')
    bit=tree/'fpga/gfx2d_ti60/outflow/gfx2d_ti60.bit'
    current.artifact(bit)
    port=uart_port();vectors=build/'vectors'
    command(['wsl.exe','-e','python3','Main/tools/check/r3_vectors.py','--out',
        '/mnt/'+vectors.drive[0].lower()+vectors.as_posix()[2:], '--frames','11'],logs/'golden.log')
    expected=json.loads((vectors/'stats.json').read_text())['frames']
    app=ROOT/'Main/sw/apps/test/rastTest';results=[]
    for name,flags,negative in [('pixel-injection','-DR3_INJECT_PIXEL -DR3_TEST_FRAMES=1',True),('normal','',False)]:
        target=build/name
        relative=os.path.relpath(target,app).replace('\\','/')
        script=('r3_gcc="$(find "$HOME/opt" -path "*/bin/riscv-none-elf-gcc" -print -quit)"; '
                'test -n "$r3_gcc" || exit 3; make -C Main/sw/apps/test/rastTest -j8 PLATFORM=target '
                'OBJDIR='+shlex.quote(relative)+' EXTRA_CFLAGS='+shlex.quote(flags)+
                ' RISCV_BIN="${r3_gcc%gcc}"')
        command(['wsl.exe','-e','bash','-c',script],logs/(name+'.build.log'))
        binary=target/'rastTest-O2.bin';current.artifact(binary)
        serial=logs/(name+'.serial.log')
        control=command([sys.executable,ROOT/'Main/tools/board/board_run.py',
            '--bit',bit,'--bin',binary,'--port',port,'--stop-line','done','--seconds','240',
            '--log',serial],logs/(name+'.control.log'),timeout=360)
        if str(binary.stat().st_size)+' bytes written at address' not in control:
            raise RuntimeError('missing BIN deployment identity')
        result=validate(serial.read_text(encoding='utf-8-sig'),expected,negative)
        if not negative:
            raw=serial.read_text(encoding='utf-8-sig')
            first=re.search(r'^R3 frame .*$',raw,re.M)[0]
            changes={
                'no-boundary':raw.replace('# --- 程序已加载，以下才是它的输出 ---',''),
                'lost-serial':raw+'\n# serial lost\n',
                'missing-frame':raw.replace(first,''),
                'duplicate-frame':raw+'\n'+first+'\n',
                'pixel-crc':raw.replace(' hw ',' hw f',1),
                'underflow':raw.replace('under 00000000','under 00000001',1),
                'wrong-result':raw.replace('fail 0','fail 1'),
                'no-recovery':raw.replace('RESET recovery code 00000000',''),
            }
            for label,changed in changes.items():
                try:validate(changed,expected)
                except RuntimeError:continue
                raise RuntimeError('collector accepted '+label)
            write_json(logs/'parser-negatives.json',dict(rejected=list(changes)))
        results.append(dict(case=name,port=port,**result))
        print('R3 BOARD '+result['status']+' '+name,flush=True)
    current.verify();write_json(logs/'result.json',dict(profile=current.profile,results=results,
        deployment_tree=tree.relative_to(ROOT).as_posix(),external_tree=bool(a.tree),
        scope='JTAG named B7 tree and source identities; no Flash or human HDMI observation'))


if __name__=='__main__':
    try:main()
    except IncompleteCheck as e:print('INCOMPLETE '+str(e),file=sys.stderr);sys.exit(3)
