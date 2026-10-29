#!/usr/bin/env python3
"""Deploy one frozen game release, verify its identity, then attach its panel."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import time


def uart():
    import winreg
    from serial.tools import list_ports
    present = {p.device for p in list_ports.comports()}
    result = []
    with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r'SYSTEM\CurrentControlSet\Enum\FTDIBUS') as parent:
        for i in range(winreg.QueryInfoKey(parent)[0]):
            name = winreg.EnumKey(parent, i)
            if not name.upper().startswith('VID_0403+PID_6011+') or not name.upper().endswith('&3'):
                continue
            try:
                with winreg.OpenKey(parent, name + r'\0000\Device Parameters') as key:
                    port = winreg.QueryValueEx(key, 'PortName')[0]
                    if port in present:
                        result.append(port)
            except FileNotFoundError:
                pass
    if len(result) != 1:
        raise RuntimeError('Cannot uniquely locate FT4232H C UART; use --port.')
    return result[0]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--root', type=Path, required=True)
    ap.add_argument('--port')
    ap.add_argument('--no-panel', action='store_true')
    ap.add_argument('--check-only', action='store_true')
    args = ap.parse_args()
    root = args.root.resolve()
    print('Verifying frozen game files...',flush=True)
    manifest = json.loads((root / 'manifest.json').read_text(encoding='utf-8'))
    for entry in manifest['files']:
        path = root / entry['path']
        if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != entry['sha256']:
            raise RuntimeError('Release file changed: ' + entry['path'])
    if args.check_only:
        print('RELEASE FILES PASS ' + manifest['name'])
        return
    port = args.port or uart()
    output = root / 'runtime' / time.strftime('%Y%m%d_%H%M%S')
    output.mkdir(parents=True, exist_ok=False)
    serial = output / 'serial.log'
    frontend=manifest.get('frontend','legacy')
    marker='console status ' if frontend=='console' else 'R4STAT '
    command = [sys.executable, str(root / 'tools/board/board_run.py'), '--bit',
        str(root / 'board/B7.bit'), '--bin', str(root / 'board/gfx2dGame.bin'),
        '--workspace', str(root / 'sdk'), '--port', port, '--seconds', '120',
        '--stop-on', marker, '--log', str(serial)]
    if frontend=='console':command.append('--precise-boundary')
    print('Loading B7 and '+frontend+' over JTAG on '+port+'; waiting for live feedback...',flush=True)
    with (output / 'deploy.log').open('w', encoding='utf-8') as log:
        process = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT)
    body = serial.read_text(encoding='utf-8-sig') if serial.exists() else ''
    deploy = (output / 'deploy.log').read_text(encoding='utf-8')
    boundary = '# --- 程序已加载，以下才是它的输出 ---'
    if process.returncode or boundary not in body or marker not in body.split(boundary, 1)[1]:
        raise RuntimeError('Game startup incomplete; inspect ' + str(output))
    if str((root / 'board/gfx2dGame.bin').stat().st_size) + ' bytes written at address' not in deploy:
        raise RuntimeError('Missing program byte-count identity')
    if '# serial lost' in body or re.search(r'^fail [1-9]|^console (?:error|fatal)\b', body, re.M):
        raise RuntimeError('Board reported a failure')
    print('GAME RUNNING ' + port + '; ' + str(output))
    if not args.no_panel:
        subprocess.Popen([sys.executable, '-B', str(root / 'tools/board/game_control.py'),
            '--port', port, '--ensure-hw', '--log', str(output / 'panel.log'),
            '--status-file', str(output / 'panel_status.json')], creationflags=0x08000000)


if __name__ == '__main__':
    main()
