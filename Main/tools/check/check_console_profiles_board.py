#!/usr/bin/env python3
"""Fresh four-snapshot application-set validation; JTAG only, no Flash writes."""
import json
import os
from pathlib import Path
import sys
from check_run import ROOT,IncompleteCheck,session,command,identity,write_json
from console_checks import validate_board

BOUNDARY='# --- 程序已加载，以下才是它的输出 ---'


def script(full):
    events=[]
    def pulse(at,key):events.extend([(at,key),(at+150,0)])
    pulse(4000,128);pulse(20000,256);pulse(36000,512)
    if full:
        pulse(52000,896);pulse(68000,4096);back=82000
    else:
        pulse(48000,896) # A pruned shortcut must not enter a hidden R4.
        back=52000
    pulse(back,2048);pulse(back+4000,1);pulse(back+5000,1024);pulse(back+11000,16)
    return ','.join(f'{t}:{k}' for t,k in sorted(events))


def main():
    current=session();logs=current.logs('console-profiles-board');build=current.build('console-profiles-board')
    if os.name!='nt':raise IncompleteCheck('four-snapshot board checks require Windows and the physical Ti60')
    sys.path.insert(0,str(ROOT/'Main/tools/board'))
    from release_launch import uart
    port=os.environ.get('GFX_CHECK_PORT') or uart()
    binaries={}
    for profile in ('2d','full'):
        out=build/profile
        command([sys.executable,ROOT/'Main/tools/board/build_console.py','--out',out,'--app-profile',profile],logs/(profile+'.build.log'),timeout=900)
        binaries[profile]=out/'console-O2-perf.bin'
    results=[]
    for preset in ('B0','B3','B5','B7'):
        selected='full' if preset=='B7' else '2d'
        folder=ROOT/'Main/bit/snapshots'/preset;bit=folder/'gfx2d_ti60.bit'
        manifest=json.loads((folder/'manifest.json').read_text(encoding='utf-8'))
        if identity(bit)['sha256']!=manifest['bit_sha256']:raise RuntimeError('snapshot bit identity mismatch: '+preset)
        binary=binaries[selected];content=json.loads((binary.parent/'content.json').read_text())
        before=dict(bit=identity(bit),bin=identity(binary),assets=identity(ROOT/'Main/assets/build/assets.pak'))
        serial=logs/(preset+'.serial.log');deploy=logs/(preset+'.deploy.log')
        command([sys.executable,ROOT/'Main/tools/board/board_run.py','--bit',bit,'--bin',binary,
            '--port',port,'--precise-boundary','--seconds','120' if selected=='full' else '90',
            '--input-script',script(selected=='full'),'--input-script-after','menu crc','--log',serial],deploy,timeout=240)
        body=serial.read_text(encoding='utf-8-sig',errors='replace')
        if BOUNDARY not in body:raise RuntimeError('missing exact loading boundary')
        if str(binary.stat().st_size)+' bytes written at address' not in deploy.read_text(encoding='utf-8'):
            raise RuntimeError('missing binary write-count evidence')
        body=body.split(BOUNDARY,1)[1]
        status=validate_board(body,expected=content,caps=int(manifest['caps'],16))
        if selected=='2d' and 'console enter r4' in body:raise RuntimeError('2D input bypassed pruning')
        after=dict(bit=identity(bit),bin=identity(binary),assets=identity(ROOT/'Main/assets/build/assets.pak'))
        if before!=after:raise RuntimeError('deployment inputs changed')
        for p in (bit,binary,serial,deploy,binary.parent/'firmware.json',binary.parent/'content.json'):current.artifact(p)
        results.append(dict(preset=preset,content_profile=selected,content_hash=content['catalog_sha256'],caps=manifest['caps'],
            identities=before,serial_sha256=identity(serial)['sha256'],status_samples=len(status),
            completed_apps=sorted({s['app'] for s in status}),
            bin=str(binary.relative_to(ROOT)),status='PASS'))
        write_json(logs/'deployments.json',dict(port=port,deployments=results,flash_written=False))
        print('SNAPSHOT CONSOLE PASS '+preset+' '+selected,flush=True)
    current.verify()
    write_json(logs/'result.json',dict(status='PASS',profile=current.profile,deployments=results,port=port,
        scope='four unchanged snapshot bitstreams with selected firmware, real frames, capabilities, RTOS clock and background progress',
        excluded=['RTL synthesis','Flash writes/boot','manual HDMI and physical-input observation','N_max search']))
    print('CONSOLE PROFILE BOARD PASS B0/B3/B5=2d B7=full; JTAG only')


if __name__=='__main__':
    try:main()
    except IncompleteCheck as exc:print('INCOMPLETE '+str(exc),file=sys.stderr);sys.exit(3)
