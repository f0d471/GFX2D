#!/usr/bin/env python3
"""SW16 fresh B7/JTAG acceptance. No Flash programming or RTL build."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import struct
import sys
import zlib
from check_run import ROOT,IncompleteCheck,session,command,identity,write_json

MAIN=ROOT/'Main'
BOUNDARY='# --- 程序已加载，以下才是它的输出 ---'
WIDTH,HEIGHT,STRIDE=640,480,4096
DISPLAY=0xf810e000

def require(value,message):
    if not value:raise RuntimeError(message)

def mdw_values(body):
    values={}
    for line in body.splitlines():
        match=re.match(r'\s*(0x[0-9a-fA-F]+):\s*(.*)$',line)
        if not match:continue
        words=match[2].split()
        if not words or any(not re.fullmatch(r'(?:0x)?[0-9a-fA-F]{8}',word) for word in words):continue
        base=int(match[1],16)
        for i,word in enumerate(words):values[base+i*4]=int(word,16)
    return values

def expected_pixels(name):
    if name=='pixels':
        return b''.join(struct.pack('<I',0xff000000|((x+15)&255)<<16|((y+15)&255)<<8)
                        for y in range(HEIGHT) for x in range(WIDTH))
    require(name=='bounce','unknown SDK example')
    rows=[]
    for y in range(HEIGHT):
        rows.append(b''.join(struct.pack('<I',0xffffa020 if 80<=x<96 and 58<=y<74 else 0xff081020)
                             for x in range(WIDTH)))
    return b''.join(rows)

def check_frame(name,raw):
    require(len(raw)==STRIDE*HEIGHT,'framebuffer readback length mismatch')
    visible=b''.join(raw[y*STRIDE:y*STRIDE+WIDTH*4] for y in range(HEIGHT))
    expected=expected_pixels(name)
    require(visible==expected,'SDK framebuffer differs from independent pixel model: '+name)
    return visible,zlib.crc32(visible)

def save_png(path,visible):
    def chunk(kind,data):return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data))
    rows=[]
    for y in range(HEIGHT):
        row=visible[y*WIDTH*4:(y+1)*WIDTH*4]
        rgb=bytearray()
        for i in range(0,len(row),4):rgb.extend((row[i+2],row[i+1],row[i]))
        rows.append(b'\0'+rgb)
    path.write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',WIDTH,HEIGHT,8,2,0,0,0))+
                     chunk(b'IDAT',zlib.compress(b''.join(rows)))+chunk(b'IEND',b''))

def selftest():
    parsed=mdw_values('Info: target\n0xf810e000: 44495350 00000001\n0xf810e0dc: 00000010\n')
    require(parsed[DISPLAY]==0x44495350 and parsed[DISPLAY+0xdc]==16,'MDW parser')
    for name in ['pixels','bounce']:
        expected=expected_pixels(name);raw=bytearray(STRIDE*HEIGHT)
        for y in range(HEIGHT):raw[y*STRIDE:y*STRIDE+WIDTH*4]=expected[y*WIDTH*4:(y+1)*WIDTH*4]
        check_frame(name,raw)
        raw[128]^=1
        try:check_frame(name,raw)
        except RuntimeError:pass
        else:raise RuntimeError('pixel corruption was not detected')
    print('SW16 BOARD PARSER/MODEL SELFTEST PASS')

def build(current,application,label,feature_header=None,extra_flags=''):
    app=MAIN/'sw/apps'/application
    output=current.build('sw16-target')/label
    relative=os.path.relpath(output,app).replace('\\','/')
    script=('set -eu; compiler="$(find "$HOME/opt" -path "*/bin/riscv-none-elf-gcc" -print -quit)"; '
            'test -n "$compiler"; make -C '+shlex.quote(app.relative_to(ROOT).as_posix())+
            ' -j2 PLATFORM=target OBJDIR='+shlex.quote(relative)+' RISCV_BIN="${compiler%gcc}" all')
    if feature_header:
        include=os.path.relpath(feature_header,app).replace('\\','/')
        extra_flags+=' -include '+include
    if extra_flags:script+=' EXTRA_CFLAGS='+shlex.quote(extra_flags)
    command(['wsl.exe','-e','bash','-c',script],current.logs('sw16-board')/(label+'.build.log'),timeout=600)
    candidates=list(output.glob('*.bin'))
    require(len(candidates)==1,'ambiguous target binary: '+label)
    current.artifact(candidates[0]);return candidates[0]

def deploy(current,bit,binary,port,label,seconds,stop=None,program=True):
    logs=current.logs('sw16-board');serial=logs/(label+'.serial.log')
    args=[sys.executable,MAIN/'tools/board/board_run.py','--bin',binary,'--port',port,
          '--precise-boundary','--seconds',str(seconds),'--log',serial]
    args+=['--bit',bit] if program else ['--no-program']
    if stop:args+=['--stop-on',stop]
    before=identity(binary)
    output=command(args,logs/(label+'.deploy.log'),timeout=seconds+120)
    require(str(binary.stat().st_size)+' bytes written at address' in output,'missing exact BIN load count')
    body=serial.read_text(encoding='utf-8',errors='replace')
    require(BOUNDARY in body,'missing precise UART boundary')
    require(identity(binary)==before,'BIN changed during capture')
    current.artifact(serial);return body.split(BOUNDARY,1)[1]

def main():
    if '--selftest' in sys.argv:selftest();return
    current=session()
    parser=argparse.ArgumentParser()
    parser.add_argument('--bit',type=Path,default=MAIN/'bit/snapshots/B7/gfx2d_ti60.bit')
    parser.add_argument('--tree',type=Path)
    args=parser.parse_args()
    if os.name!='nt':raise IncompleteCheck('SW16 physical acceptance requires Windows vendor Python')
    logs=current.logs('sw16-board')
    manifest=args.bit.parent/'manifest.json'
    data=json.loads(manifest.read_text(encoding='utf-8'))
    require(data['config']=='B7' and data['bit_sha256']==identity(args.bit)['sha256'],'B7 snapshot identity mismatch')
    from check_r4_board import b7_tree
    tree,timing=b7_tree(args.tree)
    built_bit=tree/'fpga/gfx2d_ti60/outflow/gfx2d_ti60.bit'
    require(identity(built_bit)['sha256']==data['bit_sha256'],'built B7 and selected snapshot differ')
    from check_board_smoke import uart_port,validate_engine
    port=uart_port()
    os.environ['GFX_CHECK_PORT']=port
    current.artifact(args.bit);current.artifact(manifest)
    normal=build(current,'console','console-normal')
    sys.path.insert(0,str(MAIN/'tools/board'))
    import board_run
    installation=board_run.find_efinity();ocd=board_run.find_openocd(installation)
    require(ocd is not None,'OpenOCD unavailable')
    cfg=str(MAIN/'tools/board/gfx2d_debug.cfg');workspace=str(MAIN/'sdk')
    def debug(label,commands,timeout=120):
        ok,text=board_run.run_openocd(ocd,cfg,workspace,commands+['shutdown'],timeout)
        (logs/(label+'.openocd.log')).write_text(text,encoding='utf-8')
        require(ok and 'Error:' not in text and '[TIMEOUT' not in text,'OpenOCD readback failed: '+label)
        return text
    results={};restore=None
    try:
        def engine():
            binary=build(current,'test/engineTest','engine',tree/'gfx_feature.h')
            body=deploy(current,args.bit,binary,port,'engine',180,'done')
            result=validate_engine(BOUNDARY+'\n'+body)
            for i in range(2):debug('debug-'+str(i),['reg pc'])
            return result
        results['engine']=current.perform('sw16.engine',engine,reusable=False)
        for name,count in [('pixels',16),('bounce',24)]:
            def sdk_case(name=name,count=count):
                binary=build(current,'probe/sdkBoardTest','sdk-'+name,
                             extra_flags='-DSDK_BOARD_SAMPLE='+str(1 if name=='pixels' else 2))
                body=deploy(current,args.bit,binary,port,'sdk-'+name,55,'SDKCHECK '+name)
                require('SDK '+name+' frames '+str(count)+' presents '+str(count)+' PASS' in body,'SDK completion missing')
                match=re.search(r'SDKCHECK '+name+r' crc0 ([0-9a-f]{8}) crc1 ([0-9a-f]{8}) swaps (\d+) PASS',body)
                require(match is not None and int(match[3])==count,'SDK independent DDR CRC missing')
                regs=mdw_values(debug('sdk-'+name+'-status',['halt',f'mdw {DISPLAY:#x} 4',f'mdw {DISPLAY+0xd8:#x} 2']))
                require(regs.get(DISPLAY)==0x44495350,'display identity missing')
                require(regs.get(DISPLAY+0xdc)==count,'SDK swap count differs')
                front=regs.get(DISPLAY+0xc)
                require(front in (0x02000000,0x02400000),'unexpected scan buffer')
                expected=expected_pixels(name);crc=zlib.crc32(expected)
                require(int(match[1 if front==0x02000000 else 2],16)==crc,'actual DDR checksum mismatch')
                points=[(0,0),(17,29),(80,58),(95,73),(96,74),(639,479)]
                commands=[f'mdw {front+y*STRIDE+x*4:#x} 1' for x,y in points]
                observed=mdw_values(debug('sdk-'+name+'-samples',commands))
                for x,y in points:
                    value=struct.unpack_from('<I',expected,(y*WIDTH+x)*4)[0]
                    require(observed.get(front+y*STRIDE+x*4)==value,'JTAG pixel sample mismatch')
                return dict(status='PASS',frames=count,presents=count,crc32=f'{crc:08x}',front_base=f'{front:08x}',
                            crc_source='board CPU uncached DDR full-frame read',jtag_pixel_samples=len(points),
                            bin_sha256=identity(binary)['sha256'])
            results['sdk-'+name]=current.perform('sw16.sdk.'+name,sdk_case,reusable=False)
        def console():
            command([sys.executable,MAIN/'tools/check/check_console_board.py','--bit',args.bit],logs/'console-runner.log',timeout=1800)
            out=current.logs('console-board')
            from console_checks import validate_board
            states=validate_board((out/'serial.log').read_text().split(BOUNDARY,1)[1])
            require(all(int(x.get('health','-1'))==0 and int(x.get('stack_free','0'))>=256 for x in states),
                    'normal RTOS health or stack watermark failed')
            return dict(status='PASS',states=len(states),health_faults=0,
                        minimum_stack_free=min(int(x['stack_free']) for x in states))
        results['console']=current.perform('sw16.console',console,reusable=False)
        def raster():
            command([sys.executable,MAIN/'tools/check/check_r4_board.py','--tree',tree],logs/'r4-runner.log',timeout=3600)
            return json.loads((current.logs('r4-board')/'result.json').read_text())
        results['r4']=current.perform('sw16.r4',raster,reusable=False)
    finally:
        try:
            body=deploy(current,args.bit,normal,port,'restore-menu',60,program=True)
            from console_checks import validate_board
            states=validate_board(body,full=False)
            require(states[-1]['app']=='menu' and all(int(x.get('health','-1'))==0 for x in states),'normal menu not restored')
            restore=dict(status='PASS',app='menu',backend=states[-1]['backend'],
                fps_milli=int(states[-1]['fps_milli']),serial_released=True,bin_sha256=identity(normal)['sha256'])
        except Exception as exc:
            write_json(logs/'restore-failure.json',dict(status='FAIL',error=str(exc)))
            raise
    current.verify()
    write_json(logs/'result.json',dict(status='PASS',profile=current.profile,port=port,
        bit=dict(path=args.bit.relative_to(ROOT).as_posix(),sha256=data['bit_sha256']),
        results=results,restored=restore,flash_written=False,
        excluded=['Flash boot and persistence','physical HDMI/controller observation','hardware watchdog']))
    print('SW16 BOARD PASS engine/SDK pixels/RTOS/faults/R4; normal menu restored, UART released')

if __name__=='__main__':main()
