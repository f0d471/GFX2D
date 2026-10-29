"""Shared build and capture contracts for the R4 host and physical board checks."""
import os,re,shlex,sys,struct
from pathlib import Path
from check_run import ROOT,command

APP=ROOT/'Main/sw/apps/game/gfx2dGame'
_contract=(APP/'src/r4.h').read_text(encoding='utf-8')
def contract(name):
    return int(re.search(r'^#define\s+'+name+r'\s+(0x[0-9a-fA-F]+|[0-9]+)u?\b',
                         _contract,re.M)[1],0)
SEED,SCENE_ID,ASSET_VERSION=(contract(k) for k in ('R4_SEED','R4_SCENE_ID','R4_ASSET_VERSION'))
def replay_header(current,name):
    return current.build('r4-inputs')/name/'r4_replay.h'

def build(current,name,flags,target=False,perf=True,replay_keys=None):
    if os.name=='nt':target=True
    dest=current.build('r4')/name
    replay_include=None
    if '-DGFX2D_R4_JUDGE=1' in flags or replay_keys is not None:
        def value(key,default):
            found=re.search(r'-D'+key+r'=(\d+)',flags)
            return int(found[1]) if found else default
        count=value('GFX2D_FRAMES',60)
        keys=list(replay_keys) if replay_keys is not None else [
              (4 if i%120<15 else 8 if i%120<30 else 0)|
              (1 if i%60<8 else 2 if i%60<16 else 0) for i in range(count)]
        if len(keys)!=count or any(k<0 or k>1023 for k in keys):
            raise ValueError('R4 replay key count/range')
        tape=struct.pack('<4sHH6I',b'R4RP',1,0,30,SEED,
            SCENE_ID+value('GFX2D_R4_SCENE',0),value('GFX2D_R4_LADDER',2),ASSET_VERSION,count)
        tape+=b''.join(struct.pack('<I',k) for k in keys)
        # Immutable test input is outside OBJDIR: a compile-option change may
        # legitimately remove every object and derived replay under OBJDIR.
        header=replay_header(current,name);header.parent.mkdir(parents=True,exist_ok=True)
        header.write_text('static const unsigned char r4_replay_data[]={'+','.join(map(str,tape))+'};\n')
        replay_include='-I'+os.path.relpath(header.parent,APP).replace('\\','/')
        current.artifact(header)
        flags+=' -DGFX2D_R4_REPLAY=1'
    rel=os.path.relpath(dest,APP).replace('\\','/')
    args=['make','-C',APP,'-j2','PLATFORM='+('target' if target else 'host'),
          'OBJDIR='+rel,'PERF='+('1' if perf else '0'),'EXTRA_CFLAGS='+flags]
    if replay_include:args.append('APP_CPPFLAGS='+replay_include)
    log=current.logs('r4-build')/(name+'.log')
    if os.name=='nt':
        script=('r4cc="$(find "$HOME/opt" -path "*/bin/riscv-none-elf-gcc" -print -quit)"; '
                'test -n "$r4cc" || exit 3; ')
        script+='make -C Main/sw/apps/game/gfx2dGame -j8 PLATFORM=target OBJDIR='+shlex.quote(rel)
        script+=' PERF='+('1' if perf else '0')+' EXTRA_CFLAGS='+shlex.quote(flags)+' RISCV_BIN="${r4cc%gcc}"'
        if replay_include:script+=' APP_CPPFLAGS='+shlex.quote(replay_include)
        body=command(['wsl.exe','-e','bash','-c',script],log)
    else:body=command(args,log)
    if re.search(r'\bwarning:|\berror:',body):raise RuntimeError('R4 build warnings: '+name)
    binary=dest/('gfx2dGame-O2'+('-perf' if perf else '')+'.bin' if target else 'gfx2dGame'+('-perf' if perf else ''))
    current.artifact(binary)
    return binary

def flags(ladder=2,scene=0,hw=1,frames=60,judge=True):
    return ('-DGFX2D_START_MODE=4 -DGFX2D_FRAMES='+str(frames)+
        ' -DGFX2D_R4_LADDER='+str(ladder)+' -DGFX2D_R4_SCENE='+str(scene)+
        ' -DGFX2D_R4_HW='+str(hw)+' -DGFX2D_R4_JUDGE='+str(int(judge)))

def complete(body):
    if '# serial lost' in body or re.findall(r'^fail (\d+)\s*$',body,re.M)!=['0'] or not re.search(r'^done\s*$',body,re.M):
        raise RuntimeError('R4 incomplete/failed capture')

def frames(body,count):
    complete(body)
    rows=re.findall(r'^R4CAD (\d+) (\d+) (\d+) (\d+) (\d+)  +(\d+) ([0-9a-f]{8}) ([0-9a-f]{8})\s*$',body,re.M)
    if len(rows)!=count or [int(x[0]) for x in rows]!=list(range(count)):
        raise RuntimeError('R4 frame count/order/format')
    return [dict(frame=int(x[0]),cycles=int(x[1]),raster=int(x[5]),crc=x[6],records=x[7]) for x in rows]

def compare(body,expected,negative=False):
    got=frames(body,len(expected))
    if any(a['records']!=b['records'] for a,b in zip(got,expected)):raise RuntimeError('R4 setup records differ')
    bad=[a['frame'] for a,b in zip(got,expected) if a['crc']!=b['crc']]
    if bool(bad)!=negative:raise RuntimeError('R4 pixel comparison '+str(bad[:8]))
    return dict(status='EXPECTED_RED' if negative else 'PASS',frames=len(got),mismatched=bad)

def health(body):
    rows=re.findall(r'^R4HEALTH (\d+) (\d+) (\d+) (\d+) (\d+)\s*$',body,re.M)
    if not rows or any(any(int(v) for v in r[2:]) for r in rows):raise RuntimeError('R4 display health')
    return [list(map(int,r)) for r in rows]

def recording(body,count):
    complete(body)
    headers=re.findall(r'^r4rp (.+)\s*$',body,re.M)
    if len(headers)!=1 or list(map(int,headers[0].split()))!=[1,SEED,SCENE_ID,2,ASSET_VERSION,count]:
        raise RuntimeError('R4 recording identity/length')
    keys=re.findall(r'^r4key (\d+) ([0-9a-f]{8})\s*$',body,re.M)
    if keys!=[(str(i),'00000000') for i in range(count)]:raise RuntimeError('R4 recording keys')
    return dict(status='PASS',ticks=count)

def performance(body,count):
    complete(body);health(body)
    if re.findall(r'^r4 enter ladder/hw (\d+) (\d+)\s*$',body,re.M)!=[('2','1')]:
        raise RuntimeError('R4 performance requires one hardware L2 session')
    rows=re.findall(r'^R4PERF ([\d ]+)\s*$',body,re.M)
    if len(rows)!=1:raise RuntimeError('R4 performance summary missing')
    v=list(map(int,rows[0].split()))
    if len(v)!=9 or v[0]!=count or v[3]>3333333 or v[4] or v[5]!=count or v[6] or v[7]!=2*count or v[8]!=count:
        raise RuntimeError('R4 cadence/budget failed: '+str(v))
    return dict(status='PASS',samples=count,p50=v[1],p99=v[2],maximum=v[3],late=v[4],fields=v[7],swaps=v[8])
