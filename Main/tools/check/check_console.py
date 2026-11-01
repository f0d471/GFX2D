#!/usr/bin/env python3
"""Shared frontend: lifecycle faults, input transport, actual host applications."""
import ctypes
import os
from pathlib import Path
import subprocess
import sys
from check_run import ROOT,command,session,write_json

APP=ROOT/'Main/sw/apps/console'
GAME=ROOT/'Main/sw/apps/game/gfx2dGame/src'

def require(condition,message):
    if not condition:raise RuntimeError(message)

def main():
    current=session();logs=current.logs('console');build=current.build('console')
    catalog_dir=build/'presentation-catalog'
    command([sys.executable,ROOT/'Main/tools/board/console_catalog.py','--catalog',APP/'catalog.json',
             '--profile','full','--out',catalog_dir],logs/'presentation-catalog.log')
    include=['-I'+str(ROOT/'Main/sw/lib'),'-I'+str(ROOT/'Main/sw/bsp/include'),
             '-I'+str(APP/'src'),'-I'+str(ROOT/'Main/sw/ports/myriad'),'-I'+str(ROOT/'Main/sw/ports/myriad'),'-I'+str(GAME),'-I'+str(ROOT/'Main/sw/api'),'-I'+str(ROOT/'Main/sw/runtime/include'),'-I'+str(ROOT/'Main/sw/system/include'),'-I'+str(ROOT/'Main/sw/services/gfx')]
    include.append('-I'+str(catalog_dir))
    include.append('-I'+str(ROOT/'Main/assets/build'))
    groups={
        'lifecycle':[APP/'test/test_appfw.c',ROOT/'Main/sw/runtime/appfw.c',APP/'test/test_registry.c',
                     ROOT/'Main/sw/lib/core/format.c',ROOT/'Main/sw/lib/core/logq.c'],
        'input':[APP/'test/test_input.c',ROOT/'Main/sw/bsp/key_packet.c',ROOT/'Main/sw/services/input/board_input.c'],
        'presentation':[APP/'test/test_console_present.c',ROOT/'Main/sw/ports/myriad/console_present.c'],
        'scene-view':[APP/'test/test_scene_view.c',*sorted((GAME/'game').glob('*.c')),
                      *sorted((ROOT/'Main/sw/lib/core').glob('*.c')),*sorted((ROOT/'Main/sw/lib/gfx').glob('*.c'))],
    }
    outputs={}
    for label,flags in [('64',['-fsanitize=address,undefined','-fno-sanitize-recover=all']),('32',['-m32'])]:
        for name,sources in groups.items():
            exe=build/(name+label)
            defines=['-DGFX_FRAME_DIAGNOSTIC=0'] if name=='presentation' else []
            if name=='scene-view':defines=['-DGFX_PRODUCT_HUD=1','-ffunction-sections','-fdata-sections','-Wl,--gc-sections']
            command(['gcc','-std=gnu11','-O2','-g','-Wall','-Wextra','-Werror',*flags,*defines,*include,
                     *sources,'-o',exe],logs/(name+label+'.build.log'))
            body=command([exe],logs/(name+label+'.log'),timeout=60)
            require('fail 0' in body,'unit tests did not finish: '+name)
            if label=='64':outputs[name]=body
            else:require(outputs[name]==body,'64/32 behavior differs: '+name)

    # A representative old enter-commit bug must be caught by the actual tests.
    original=(ROOT/'Main/sw/runtime/appfw.c').read_text(encoding='utf-8')
    mutant=build/'appfw-bad-enter.c'
    old='if(next->enter(&s_ctx)!=0)return -1;'
    require(old in original,'mutation anchor disappeared')
    mutant.write_text(original.replace(old,'if(next->enter(&s_ctx)!=0) { s_active=next;return 0; }',1),encoding='utf-8')
    exe=build/'lifecycle-mutant'
    sources=[mutant if p==ROOT/'Main/sw/runtime/appfw.c' else p for p in groups['lifecycle']]
    command(['gcc','-std=gnu11','-O2','-Wall','-Wextra','-Werror',*include,*sources,'-o',exe],logs/'mutant.build.log')
    result=subprocess.run([str(exe)],cwd=ROOT,capture_output=True,text=True,timeout=20)
    (logs/'mutant.log').write_text(result.stdout+result.stderr,encoding='utf-8')
    require(result.returncode!=0 and 'FAIL failed-enter' in result.stdout,'old lifecycle bug was not detected')

    # Python transmitter is cross-checked against the actual C BSP decoder.
    shared=build/'libkeypacket.so'
    command(['gcc','-shared','-fPIC','-O2','-I'+str(ROOT/'Main/sw/bsp/include'),
             ROOT/'Main/sw/bsp/key_packet.c','-o',shared],logs/'wire.build.log')
    lib=ctypes.CDLL(str(shared))
    class Decoder(ctypes.Structure):
        _fields_=[('state',ctypes.c_uint8),('header',ctypes.c_uint8),('lo',ctypes.c_uint8),('hi',ctypes.c_uint8)]
    lib.key_packet_feed.argtypes=[ctypes.POINTER(Decoder),ctypes.c_uint8,ctypes.POINTER(ctypes.c_uint32)]
    sys.path.insert(0,str(ROOT/'Main/tools/board'))
    from input_forward import packet
    d=Decoder();value=ctypes.c_uint32()
    for key in range(16384):
        accepted=[lib.key_packet_feed(ctypes.byref(d),byte,ctypes.byref(value)) for byte in packet(key)]
        require(sum(accepted)==1 and accepted[-1]==1 and value.value==key,'wire roundtrip: '+str(key))
    (logs/'wire.log').write_text('Python transmitter / C decoder: 16384 snapshots PASS\n',encoding='utf-8')

    env={**os.environ,'SDL_VIDEODRIVER':'dummy','SDL_AUDIODRIVER':'dummy','GFX2D_UNPACED':'1'}
    from console_checks import validate_host,check_negative_fixtures
    script='5:128,6:0,20:256,21:0,35:512,36:0,50:896,51:0,70:2048,71:0'
    for perf in [1,0]:
        out=build/('host-perf'+str(perf))
        command(['make','-C',APP,'-j2','PLATFORM=host','OBJDIR='+str(out),
                 'PERF='+str(perf),'all'],logs/('host'+str(perf)+'.build.log'))
        exe=out/('console-perf' if perf else 'console')
        body=command([exe,'--frames','80','--input-script',script],logs/('host'+str(perf)+'.log'),env=env,timeout=180)
        validate_host(body)
    # Real menu default confirm, return and system page; no hard-coded indices in the manager.
    exe=build/'host-perf1/console-perf'
    body=command([exe,'--frames','60','--input-script',
                  '3:1024,4:0,18:2048,19:0,25:1,26:0,29:1024,30:0,44:16,45:0'],
                  logs/'menu-confirm.log',env=env,timeout=180)
    require('console enter demo' in body and 'console enter about' in body,'menu/default/system page unreachable')
    require(body.count('console enter menu')>=3,'system page confirm/back did not return')
    require('console error ' not in body,'menu/system page error')
    # Held-key restart guard now uses Shift (L1/L2): the cup scene and its C toggle were removed 2026-11-02.
    body=command([exe,'--app','4','--frames','40','--input-script','5:32,20:0'],
                  logs/'held-ladder.log',env=env,timeout=180)
    require(body.count('r4 enter ladder/hw ')==2,'held ladder key retriggered during setup')
    require('console scene 0 ladder 1' in body,'ladder command was lost')
    require('console error ' not in body,'ladder restart failed')
    body=command([exe,'--app','4','--frames','40','--input-script','5:4096,20:0'],
                  logs/'scene-key-inert.log',env=env,timeout=180)
    require(body.count('r4 enter ladder/hw ')==1 and 'console scene 1' not in body,
            'scene key must not switch the 3D corridor (cup removed)')
    body=command([exe,'--app','2','--frames','30','--input-script','5:64,6:0'],
                  logs/'host-capability.log',env=env,timeout=180)
    require('console capability cpu-only' in body and 'console error ' not in body,
            'host product pretended to have a hardware backend')
    check_negative_fixtures()
    # Frozen hardware mode verifies artifact identity; it never relaxes the
    # default current-RTL requirement without an explicit selected release.
    import hashlib,json
    from check_r4_board import frozen_bit_identity
    release=build/'frozen-fixture';(release/'board').mkdir(parents=True,exist_ok=True)
    bit=release/'board/B7.bit';bit.write_bytes(b'known immutable bitstream')
    (release/'manifest.json').write_text(json.dumps({'files':[{'path':'board/B7.bit',
        'sha256':hashlib.sha256(bit.read_bytes()).hexdigest()}]}),encoding='utf-8')
    frozen_bit_identity(release)
    bit.write_bytes(b'corrupted bitstream')
    try:frozen_bit_identity(release)
    except RuntimeError:pass
    else:raise RuntimeError('frozen hardware mode accepted a changed bitstream')
    current.verify()
    write_json(logs/'result.json',dict(status='PASS',profile=current.profile,
        scope='shared app manager, protocol, lifecycle faults, actual host frontend',
        native_widths=[64,32],input_snapshots=16384,perf_variants=[0,1],mutation_detected=True))
    print('CONSOLE HOST PASS lifecycle/input/faults/menu/four-apps/perf-policy')

if __name__=='__main__':main()
