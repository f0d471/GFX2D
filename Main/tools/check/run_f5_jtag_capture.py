#!/usr/bin/env python3
"""F5 功能诊断的 JTAG/DDR 日志采集；不用于性能判据，不访问串口或写 Flash。"""
import argparse
import json
from pathlib import Path
import re
import secrets
import subprocess
import sys
import time
import zlib
from run_f5_board import ROOT,MAIN,BUILD,LOGS,JOBS,register,synthesis
sys.path.insert(0,str(MAIN/'tools/board'))
import board_run
BASE=0x02c00000
CAPACITY=0x200000-32
CHOICES=['tests','v2','replay']
def binary(job):
    app,name,perf,_=JOBS[job]
    assert not perf
    return MAIN/'sw/apps'/app/('build_f5_'+job+'_ddr')/(name+'-O2.bin')
def build(job):
    app,_,_,flags=JOBS[job]
    code="import pathlib;print(next(pathlib.Path.home().glob('opt/xpack-riscv-none-elf-gcc-13.4.0-*/bin/riscv-none-elf-gcc')))"
    prefix=subprocess.check_output(['wsl.exe','-e','python3','-c',code],text=True,encoding='utf-8').strip()[:-3]
    args=['wsl.exe','-e','make','-C','Main/sw/apps/'+app,'-j8','OBJDIR=build_f5_'+job+'_ddr',
          'RISCV_BIN='+prefix,'EXTRA_CFLAGS='+flags+' -DF5_DDR_LOG=1']
    with (BUILD/(job+'_ddr_compile.log')).open('w',encoding='utf-8') as out:
        subprocess.run(args,cwd=ROOT,stdout=out,stderr=subprocess.STDOUT,check=True)
    register(binary(job));print('F5 JTAG BUILT '+job,flush=True)
def header(output):
    words={}
    for address,data in re.findall(r'^0x([0-9a-fA-F]+):\s+((?:[0-9a-fA-F]{8}\s*)+)',output,re.M):
        for i,value in enumerate(data.split()):words[int(address,16)+4*i]=int(value,16)
    return [words[BASE+4*i] for i in range(8)]
def capture(job,program):
    bit=synthesis();exe=binary(job);register(exe)
    efinity=board_run.find_efinity(None);ocd=board_run.find_openocd(efinity)
    if not ocd:raise RuntimeError('缺 OpenOCD')
    cfg=str(MAIN/'tools/board/gfx2d_debug.cfg');workspace=str(MAIN/'sdk')
    nonce=secrets.randbits(32) or 1
    control_path=BUILD/(job+'_ddr_control.log')
    with control_path.open('w',encoding='utf-8') as control:
        def command(commands,timeout=60):
            control.write('\n'.join(commands)+'\n');control.flush()
            ok,out=board_run.run_openocd(ocd,cfg,workspace,commands,timeout=timeout)
            control.write(out+'\n');control.flush()
            if not ok or re.search(r'^Error',out,re.M):raise RuntimeError(out[-2000:])
            return out
        if program:
            ok,out=board_run.program_bitstream(str(Path(efinity)/'pgm/bin/ftdi_pgm.bat'),str(bit),'0x10660a79')
            control.write(out+'\n');control.flush()
            if not ok:raise RuntimeError('JTAG 位流部署失败')
        print('F5 JTAG START '+job,flush=True)
        out=command(['reset halt',f'mww 0x{BASE:x} 0',f'mww 0x{BASE+4:x} 0x{nonce:x}',
                     f'mww 0x{BASE+8:x} 0',f'mww 0x{BASE+12:x} 0',
                     'load_image {'+exe.as_posix()+'} 0x1000 bin','resume 0x1000','shutdown'])
        assert f'{exe.stat().st_size} bytes written at address' in out,'下载字节数不符'
        begin=time.monotonic();limit=10800 if job=='replay' else 1200
        while True:
            if time.monotonic()-begin>limit:raise RuntimeError('DDR 日志等待超时')
            time.sleep(15)
            values=header(command(['halt',f'mdw 0x{BASE:x} 8','resume','shutdown']))
            magic,got_nonce,length,flags,crc,version,capacity,reserved=values
            if magic==0:continue
            assert magic==0x35474f4c and got_nonce==nonce and version==1 and capacity==CAPACITY and reserved==0,values
            assert length<=CAPACITY and not flags&2,'DDR 日志溢出'
            print(f'F5 JTAG WAIT {job} bytes={length} flags={flags} elapsed={int(time.monotonic()-begin)}',flush=True)
            if flags&1:break
        dump=BUILD/(job+'_ddr_log.bin')
        after=header(command(['halt','dump_image {'+dump.as_posix()+'} '+hex(BASE+32)+' '+str(length),
                              f'mdw 0x{BASE:x} 8','shutdown'],timeout=600))
        assert after==values,'完成后的header改变'
        payload=dump.read_bytes()
        assert len(payload)==length and zlib.crc32(payload)==crc,'JTAG回读长度/CRC32不符'
        text=payload.decode('utf-8')
        key=exe.relative_to(ROOT).as_posix()
        log=LOGS/('f5_jtag_'+job+'.log')
        log.write_bytes(('# F5 artifact '+key+'\n# JTAG DDR log: nonce='+str(nonce)+' bytes='+str(length)+' crc32='+str(crc)+'\n').encode()+payload)
        metadata={'artifact':key,'transport':'JTAG DDR','nonce':nonce,'bytes':length,'crc32':crc,'header':values,
                  'region_base':BASE,'region_bytes':CAPACITY+32,'performance_evidence':False}
        (LOGS/('f5_jtag_'+job+'.json')).write_text(json.dumps(metadata,indent=2)+'\n',encoding='utf-8')
        assert re.search(r'^fail 0\s*$',text,re.M) and re.search(r'^done\s*$',text,re.M),text[-2000:]
        assert not re.search(r'^(FAIL |err |engine err )',text,re.M),text[-2000:]
        if job=='tests':
            pairs=re.findall(r'hw_crc=(\d+) sw_crc=(\d+)',text)
            assert len(pairs)==74 and all(a==b for a,b in pairs)
            assert len(re.findall(r'^pberror test=',text,re.M))==4
        if job=='replay':
            from check_g4_integration import compare
            compare(LOGS/'f5_replay_host_flat.log',log,False,True)
    print('F5 JTAG PASS '+job+' bytes='+str(length)+' crc32=verified',flush=True)
def main():
    p=argparse.ArgumentParser();p.add_argument('--build',nargs='+',choices=CHOICES);p.add_argument('--run',nargs='+',choices=CHOICES)
    p.add_argument('--program',action='store_true');a=p.parse_args();BUILD.mkdir(parents=True,exist_ok=True);LOGS.mkdir(exist_ok=True)
    for job in a.build or []:build(job)
    for i,job in enumerate(a.run or []):capture(job,a.program and i==0)
if __name__=='__main__':main()
