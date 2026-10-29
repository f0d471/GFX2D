#!/usr/bin/env python3
"""PC-only public SDK, real driver/port probes, persistence and fault contracts."""
import ctypes
import os
from pathlib import Path
import struct
import subprocess
import sys
import zlib
from check_run import ROOT,session,command,write_json

SW=ROOT/'Main/sw'
TEST=SW/'services/test'

def require(value,message):
    if not value:raise RuntimeError(message)

def main():
    current=session();build=current.build('platform-sdk');logs=current.logs('platform-sdk')
    include=['-I'+str(SW/'api'),'-I'+str(SW/'lib'),'-I'+str(SW/'bsp/include'),
             '-I'+str(SW/'system/include'),'-I'+str(TEST/'fakes')]
    groups={
        'ownership':[TEST/'test_platform.c',SW/'services/platform/platform.c'],
        'records':[TEST/'test_records.c',*[SW/'lib/core'/x for x in ('record.c','firmware_image.c','health.c','crc32.c')]],
        'driver':[TEST/'test_driver_wait.c',SW/'bsp/ti60/gfx_engine.c'],
        'scheduler':[TEST/'test_scheduler_port.c',SW/'ports/ti60/app_port.c',SW/'lib/core/health.c'],
        'irq':[TEST/'test_irq_bridge.c',SW/'rtos/rtos_irq.c'],
    }
    expected={}
    for width,flags in [('64',['-fsanitize=address,undefined','-fno-sanitize-recover=all']),('32',['-m32'])]:
        for name,sources in groups.items():
            exe=build/(name+width)
            command(['gcc','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror',
                     *flags,*include,'-I'+str(SW/'rtos'),'-DGFX_ENGINE_TEST_BACKEND',*sources,'-o',exe],logs/(name+width+'.build.log'))
            modes=['normal','closed','rejected','timeout','no-swap'] if name=='ownership' else ['normal']
            for mode in modes:
                body=command([exe,mode],logs/(name+width+'-'+mode+'.log'),timeout=20)
                require('PASS' in body,name+' did not finish')
                key=(name,mode)
                if width=='64':expected[key]=body
                else:require(expected[key]==body,name+' differs at 32 bits')

    # C encodings must equal independently assembled little-endian/zlib vectors.
    so=build/'formats.so'
    command(['gcc','-shared','-fPIC','-O2','-I'+str(SW/'lib'),
             *[SW/'lib/core'/x for x in ('record.c','firmware_image.c','crc32.c')],'-o',so],logs/'formats.build.log')
    lib=ctypes.CDLL(str(so))
    for n in [0,1,2,7,31,64,257,1024]:
        payload=bytes((i*37+11)&255 for i in range(n));out=ctypes.create_string_buffer(n+32)
        seq=0xfedcba98
        require(lib.record_encode(out,n+32,payload,n,ctypes.c_uint32(seq))==0,'record encoding')
        header=struct.pack('<5I',0x3152564e,1,seq,n,zlib.crc32(payload))
        require(out.raw[:24+n]==header+struct.pack('<I',zlib.crc32(header))+payload,'independent record vector')
        if n:
            require(lib.firmware_image_encode(out,n+32,payload,n,0x60,7)==0,'firmware encoding')
            header=struct.pack('<7I',0x31495746,32,n,0x60,7,zlib.crc32(payload),0)
            require(out.raw==header+struct.pack('<I',zlib.crc32(header))+payload,'independent image vector')
    (logs/'independent-formats.log').write_text('8 record / 7 firmware Python-zlib vectors PASS\n')

    store=build/'store'
    command(['gcc','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror',
             '-fsanitize=address,undefined',*include,TEST/'test_store.c',SW/'services/platform/store_host.c',
             SW/'lib/core/record.c',SW/'lib/core/crc32.c','-o',store],logs/'store.build.log')
    storage=build/'persistent';storage.mkdir(exist_ok=True)
    env={**os.environ,'GFX2D_STORE_DIR':str(storage)}
    command([store,'write','first'],logs/'store-first.log',env=env)
    command([store,'read','first'],logs/'store-restart.log',env=env)
    # Every interrupted prefix is a separate process writing the real inactive slot.
    for cut in range(0,24+len('second')):
        result=subprocess.run([str(store),'write','second'],env={**env,'GFX2D_STORE_FAIL_AFTER':str(cut)},
                              cwd=ROOT,capture_output=True,text=True,timeout=20)
        require(result.returncode==1,'interrupted write did not report IO failure')
        command([store,'read','first'],logs/('store-cut-'+str(cut)+'.log'),env=env)
    command([store,'write','second'],logs/'store-second.log',env=env)
    command([store,'read','second'],logs/'store-committed.log',env=env)
    # Damage the newer slot and require the previous committed record.
    slots=[(storage/f'save.{i}').read_bytes() for i in range(2)]
    latest=max(range(2),key=lambda i:struct.unpack_from('<I',slots[i],8)[0])
    broken=bytearray(slots[latest]);broken[-1]^=1;(storage/f'save.{latest}').write_bytes(broken)
    command([store,'read','first'],logs/'store-corrupt-fallback.log',env=env)
    command([store,'invalid','x'],logs/'store-path-rejection.log',env=env)

    # A real public build cannot include private headers, even though services can.
    app=SW/'apps/examples/pixels'
    forbidden=build/'forbidden.c';forbidden.write_text('#include "board.h"\nint x;\n')
    for header in ['board.h','soc.h','gfx_engine.h','stg_runtime.h','renderer.h']:
        forbidden.write_text('#include "'+header+'"\nint main(void){return 0;}\n')
        result=subprocess.run(['make','-s','-C',str(app),'PUBLIC_APP=1','APP_SRCS='+os.path.relpath(forbidden,app),
            'OBJDIR='+str(build/('deny-'+header))],cwd=ROOT,capture_output=True,text=True,timeout=90)
        (logs/('deny-'+header+'.log')).write_text(result.stdout+result.stderr)
        require(result.returncode!=0 and header in result.stderr and 'No such file' in result.stderr,
                'actual SDK build did not reject '+header)
    for p in (SW/'api').glob('*.h'):
        require(not any(x in p.read_text() for x in ('board.h','soc.h','stg_runtime.h','FreeRTOS.h')),'public header leaked internals')
    framework=(SW/'runtime/appfw.c').read_text()
    require(not any(x in framework for x in ('stg_','r4_','gfx_engine','board_','APPFW_R4')),'generic manager has content dependency')
    for backend in ['host','ti60']:
        code=(SW/'bsp'/backend/'board.c').read_text()
        require('board_product_mode(' not in code and 'board_map_physical_keys(' not in code,
                'BSP calls upper-layer input policy')
    import re
    for directory in [SW/'apps/console/src',SW/'apps/game/gfx2dGame/src']:
        for source in directory.rglob('*.c'):
            code=re.sub(r'/\*.*?\*/|//[^\n]*','',source.read_text(),flags=re.S)
            require(not re.search(r'\b(board_\w+|gfx_engine_\w+|g_mem_regions|REGION_\w+)\b',code),
                    'content still owns board mechanisms: '+str(source))
    env={**os.environ,'SDL_VIDEODRIVER':'dummy','SDL_AUDIODRIVER':'dummy','GFX2D_UNPACED':'1'}
    for name in ['pixels','bounce']:
        app=SW/'apps/examples'/name;dest=build/name
        command(['make','-C',app,'-j2','PLATFORM=host','OBJDIR='+str(dest)],logs/(name+'.build.log'))
        body=command([dest/('sdk-'+name)],logs/(name+'.log'),env=env,timeout=60)
        require('PASS' in body,'independent SDK consumer failed')
        require(not list((dest/'app').rglob('*gfx2dGame*')),'SDK consumer linked a game')
    current.verify()
    write_json(logs/'result.json',dict(status='PASS',profile=current.profile,scope='PC SDK and fault contracts',
        widths=[64,32],sanitizers=['ASan','UBSan'],independent_consumers=2,interrupted_writes=30,
        board_access=False,target_runtime='NOT_RUN',rtl='NOT_RUN',flash_boot='NOT_RUN'))
    print('PLATFORM SDK PC PASS public-build/ownership/driver-deadline/scheduler/persistence/health/image')

if __name__=='__main__':main()
