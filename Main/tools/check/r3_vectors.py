#!/usr/bin/env python3
"""Shared R3 input scenes; expected pixels independently cross both R1 executors."""
import argparse
import ctypes as C
import json
from pathlib import Path
import struct
import subprocess
import zlib
from check_run import ROOT
from check_rast import SOURCES, Record, Target, load


def hexwords(path, data):
    data += b'\0'*(-len(data)%4)
    path.write_text(''.join(f'{n:08x}\n' for n in struct.unpack('<'+'I'*(len(data)//4),data)))


def generate(out, frames=11, textured=True):
    out=Path(out).resolve();out.mkdir(parents=True,exist_ok=True)
    libpath=out/'reference.so'
    subprocess.run(['gcc','-std=gnu11','-O2','-Wall','-Wextra','-Werror','-shared','-fPIC',
        '-IMain/sw/lib',*SOURCES,'Main/sw/apps/test/rastTest/src/r3_cases.c','-o',str(libpath)],
        cwd=ROOT,check=True)
    lib=load(libpath)
    lib.r3_scene.argtypes=[C.c_uint,C.c_uint,C.POINTER(Record),C.c_uint]
    lib.r3_texel.argtypes=[C.c_uint];lib.r3_texel.restype=C.c_uint32
    lib.rast_bin.argtypes=[C.POINTER(Record),C.c_uint,C.c_void_p,C.c_size_t]
    texture=(C.c_uint32*4096)(*[lib.r3_texel(i) for i in range(4096)])
    hexwords(out/'texture.hex',bytes(texture))
    result=[]
    for f in range(frames):
        rec=(Record*16)();assert lib.r3_scene(f,textured,rec,16)==16
        sub=(C.c_uint8*1080)();sz=lib.rast_bin(rec,16,sub,len(sub));assert sz>0
        outputs=[]
        for executor in ('rast_ref_run','rast_ref2_run'):
            image=bytearray()
            for strip in range(30):
                color=(C.c_uint32*10240)(*([0x12345678]*10240))
                depth=(C.c_uint16*10240)(*([65535]*10240))
                target=Target(color,depth,640,strip,texture,64,64,64,0xff000000)
                assert getattr(lib,executor)(rec,16,C.byref(target))==0
                for x in range(8):color[8*640+x]=0x00fedcba
                image.extend(bytes(color))
            outputs.append(bytes(image))
        assert outputs[0]==outputs[1],f
        hexwords(out/f'f{f}.records.hex',bytes(rec))
        hexwords(out/f'f{f}.sub.hex',bytes(sub[:sz]))
        hexwords(out/f'f{f}.expected.hex',outputs[0])
        refs=(sz-120)//2
        cursor=0;last_count=0
        for strip in range(30):
            last_count=struct.unpack_from('<H',bytes(sub),cursor)[0]
            cursor+=4+last_count*2
        assert cursor==sz
        hexwords(out/f'f{f}.meta.hex',struct.pack('<IIII',16,sz,refs,last_count))
        result.append(dict(frame=f,records=16,sub_bytes=sz,refs=refs,
                           crc=f'{zlib.crc32(outputs[0]):08x}'))
    (out/'stats.json').write_text(json.dumps(dict(textured=textured,frames=result),indent=2)+'\n')
    print('R3 VECTORS PASS two C executors frames='+str(frames),flush=True)
    return result


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--out',required=True)
    p.add_argument('--frames',type=int,default=11);p.add_argument('--l1',action='store_true')
    a=p.parse_args();generate(a.out,a.frames,not a.l1)
