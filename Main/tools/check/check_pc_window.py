#!/usr/bin/env python3
"""Run native Windows SDL fullscreen/filter/resize/texture-reset integration."""
import os
import sys
from check_run import ROOT,IncompleteCheck,command,session,write_json


def main():
    current=session();logs=current.logs('pc-window');out=current.build('pc-window')
    if os.name!='nt':raise IncompleteCheck('pc-window requires the Windows user desktop')
    command([sys.executable,ROOT/'Main/tools/board/build_pc.py','--out',out,'--window-test'],
        logs/'build.log',timeout=600)
    for name in ('window_test.exe','SDL2.dll','build.json'):current.artifact(out/name)
    body=command([out/'window_test.exe'],logs/'window.log',timeout=120)
    required=['WINDOW WASD PASS','WINDOW POINTER PASS','WINDOW FULLSCREEN PASS','WINDOW SCALE PASS','WINDOW FILTER PASS','WINDOW FOCUS PASS','WINDOW EXIT PASS',
        'WINDOW ADAPTIVE POLICY PASS','WINDOW ADAPTIVE RESIZE PASS','WINDOW DISPLAY PASS',
        'WINDOW DISPLAY RECOVERY PASS']
    if any(marker not in body for marker in required):raise RuntimeError('incomplete native window checks')
    current.verify();write_json(logs/'result.json',dict(status='PASS',cases=required,
        scope='native Windows SDL2; real desktop fullscreen and resize; no board-display claim'))
    print('PC WINDOW PASS',flush=True)


if __name__=='__main__':
    try:main()
    except IncompleteCheck as error:print('INCOMPLETE '+str(error));sys.exit(3)
