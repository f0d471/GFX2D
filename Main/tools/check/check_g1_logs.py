#!/usr/bin/env python3
"""G1 的外部验收：板上固定负载、真实 N_max、游戏同帧、切换及 CRC 失败路径。"""
import argparse
import json
import re
import subprocess
import sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
from check_sw9_board import body as game_body, frames as game_frames

MAIN=Path(__file__).resolve().parents[2]
GAME=MAIN/'sw/apps/game/gfx2dGame/logs'
LOGS=MAIN/'sim/engine/tex/logs'

def need(value,message):
    if not value: raise ValueError(message)

def read(path,banner=None):
    text=path.read_text(encoding='utf-8',errors='replace')
    need('# serial lost' not in text,f'{path.name}: serial lost')
    if banner:
        need(banner in text,f'{path.name}: missing banner')
        text=text[text.rfind(banner):]
    return text

def check_tool(name,*args):
    p=subprocess.run([sys.executable,str(MAIN/'tools/check'/name),*map(str,args)],
                     text=True,encoding='utf-8',stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
    need(p.returncode==0,p.stdout.strip())
    print(p.stdout.strip())

def check_board():
    text=read(MAIN/'sw/apps/test/engineTest/logs/g1_board.log')
    need(re.search(r'^fail 0\s*$',text,re.M) and re.search(r'^done\s*$',text,re.M),'board footer')
    need('FAIL' not in text,'board failed assertion')
    golden=dict(line.split() for line in (MAIN/'sw/lib/test/golden.txt').read_text().splitlines()
                if line and not line.startswith('#'))
    rows=re.findall(r'^(TEX-)?([\w-]+) ([0-9a-f]{8}) ([0-9a-f]{8}) ok\s*$',text,re.M)
    for prefix in ('','TEX-'):
        selected=[(name,a,b) for p,name,a,b in rows if p==prefix]
        need(len(selected)==11 and {r[0] for r in selected}==set(golden),'CRC row count')
        need(all(a==b==golden[name] for name,a,b in selected),'CRC golden mismatch')
    for marker in ['TEX-CAPS','TEX-BUSY','TEX-LOAD','TEX-PERF','STRESS']:
        need(marker+' ok' in text,'missing '+marker)
    for kind in ['BAND','DST','ALIGN','OOB','NOVAL','FMT']:
        need('TEXERR-'+kind+' ok' in text,'missing error '+kind)
    ddr=int(re.search(r'^PERF busy=([0-9a-f]+)',text,re.M).group(1),16)
    tex,rd=[int(v,16) for v in re.search(r'^PERF tex busy=([0-9a-f]+) rd=([0-9a-f]+)',text,re.M).groups()]
    need(tex<=750*16 and rd==258,'TEX cycle/transaction criterion')
    wm,zero=[int(v,16) for v in re.search(r'wm=([0-9a-f]+) z=([0-9a-f]+)',text).groups()]
    need(wm>=160 and zero==0,'scan pressure criterion')
    return dict(ddr_cycles_per_sprite=ddr/16,tex_cycles_per_sprite=tex/16,
                tex_rd_burst=rd,fifo_min=wm,crc_pairs=22)

def check_nmax(path,backend):
    check_tool('check_sw9_perf.py',path,'--fields-min','3600')
    check_tool('check_sw9_nmax.py',path,'--status','OK')
    text=read(path,'gfx2dGame MODES')
    rows=[line.split(',') for line in text.splitlines() if line.startswith('csv,')]
    need(rows and all(row[3]==backend for row in rows),'wrong N_max backend')
    need(re.search(r'diag fifo_min (\d+) .* zero 0 under 0 .* faults 0',text),'N_max scan/IRQ errors')
    return [int(v) for v in re.findall(r'^nmax result OK n_max (\d+) ',text,re.M)]

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--board-only',action='store_true')
    args=ap.parse_args()
    results={'board':check_board()}
    if not args.board_only:
        results['nmax_tex']=check_nmax(GAME/'g1_hw_tex_nmax.log','hw-tex')
        results['nmax_ddr']=check_nmax(GAME/'g1_hw_ddr_nmax.log','hw')
        pc=game_frames(game_body(GAME/'g1_pc_game.log',False),360)
        hw=game_frames(game_body(GAME/'g1_tex_game_compare.log',True),360)
        need(pc==hw,'360 game reference frames differ')
        text=read(GAME/'g1_tex_game_compare.log','gfx2dGame STG')
        need('compare tex ready; scan pattern' in text,'compare source/scan setup')
        count=int(re.search(r'compare tex commands (\d+)',text).group(1))
        compares=re.findall(r'^compare (\d+) sw ([0-9a-f]{8}) hw ([0-9a-f]{8})',text,re.M)
        need(len(compares)==360 and all(int(row[0])==i and row[1]==row[2]
             for i,row in enumerate(compares)),'same-frame game CRC')
        need(re.search(r'^fail 0\s*$',text,re.M) and re.search(r'^done\s*$',text,re.M),'game footer')
        need(count>0,'real game never used TEX')
        results['game_tex_commands']=count
        check_tool('check_sw9_perf.py',GAME/'g1_switch_board.log')
        text=read(GAME/'g1_switch_board.log','gfx2dGame MODES')
        markers=['backend sw frame','backend hw frame','backend hw-tex frame',
                 'mode 1 enter frame','mode 2 enter frame']
        cursor=text.find('csvh,')
        for marker in markers:
            cursor=text.find(marker,cursor)
            need(cursor>=0,'backend/mode transition: '+marker)
            cursor+=len(marker)
        text=read(GAME/'g1_tex_crc_bad.log','gfx2dGame MODES')
        need('tex err 热集装载或 CRC 自校验失败' in text,'CRC mutation not rejected')
        need(re.search(r'^fail 1\s*$',text,re.M) and re.search(r'^done\s*$',text,re.M),'CRC mutation footer')
        rows=[line.split(',') for line in text.splitlines() if line.startswith('csv,')]
        need(rows and all(row[3]=='hw' for row in rows),'CRC failure did not fall back to DDR')
        LOGS.mkdir(parents=True,exist_ok=True)
        (LOGS/'acceptance_results.json').write_text(json.dumps(results,indent=2)+'\n',encoding='utf-8')
    print('G1 LOGS OK:',json.dumps(results))

if __name__=='__main__':
    try: main()
    except (ValueError,AttributeError) as error: raise SystemExit('G1 LOGS FAIL: '+str(error))
