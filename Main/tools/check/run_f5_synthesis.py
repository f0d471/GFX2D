#!/usr/bin/env python3
"""F5 独立/整机综合及资源、全域时序、自产 RTL 位宽告警收录。"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import xml.etree.ElementTree as ET
ROOT=Path(__file__).resolve().parents[3];MAIN=ROOT/'Main'
from check_run import session
LOGS=session().logs('frontend')
PROJECTS={'bin':(MAIN/'sim/engine/frontend/syn_bin','engine_bin_syn','clk'),
          'main':(MAIN/'fpga/gfx2d_ti60','gfx2d_ti60','core_clk')}

def timing_slacks(timing):
    begin=timing.index('Setup (Max) Clock Relationship')
    section=timing[begin:timing.index('NOTE: Values',begin)]
    hold=section.index('Hold (Min) Clock Relationship')
    result=[]
    for kind,body in [('setup',section[:hold]),('hold',section[hold:])]:
        rows=re.findall(r'^\s*(\S+)\s+(\S+)\s+[-\d.]+\s+([-\d.]+)\s+\(',body,re.M)
        if not rows:raise RuntimeError('missing '+kind+' clock relationships')
        for source,destination,value in rows:
            result.append({'clock':source,'to_clock':destination,'kind':kind,'slack_ns':float(value)})
    if any(row['slack_ns']<0 for row in result):raise RuntimeError('negative slack')
    return result
def build(tag):
    folder,name,_=PROJECTS[tag]
    installation=os.environ.get('EFINITY_HOME')
    if not installation:raise RuntimeError('先按开发环境设置 EFINITY_HOME')
    # 只传安装位置会让厂商bat跳过setup，缺少其DLL/PATH；让子进程自行完整初始化。
    env=dict(os.environ)
    for key in ['EFINITY_HOME','PYTHONHOME','EFXPT_HOME','EFXPGM_HOME','EFXDBG_HOME','EFXIPM_HOME',
                'EFXSVF_HOME','EFXSERDESDBG_HOME','EFINITY_USER_DIR_INI']:
        env.pop(key,None)
    p=subprocess.run([str(Path(installation)/'bin/efx_run.bat'),name+'.xml'],cwd=folder,env=env,capture_output=True,text=True,encoding='utf-8',errors='replace')
    body=p.stdout+p.stderr;(LOGS/('f5_'+tag+'_synthesis_flow.log')).write_text(body,encoding='utf-8')
    if p.returncode or not all(re.search(r'\b'+s+r'\s*:\s*PASS',body) for s in ['map','interface','pnr','pgm']):
        raise RuntimeError(tag+' four-step flow failed')
def collect(tag):
    folder,name,clock=PROJECTS[tag];out=folder/'outflow'
    root=ET.parse(folder/(name+'.xml')).getroot()
    # 命令行独立工程可保留空的 GUI 状态属性；以实际产物新鲜度和 STA 为准。
    if root.get('last_run_state') not in (None,'','pass'):raise RuntimeError(tag+' project not PASS')
    bit=out/(name+'.bit')
    inputs=[*(MAIN/'rtl').rglob('*.v'),*(MAIN/'rtl').rglob('*.vh'),
            *(folder/'sdc').glob('*.sdc')]
    newest=max(p.stat().st_mtime_ns for p in inputs)
    if not bit.exists() or bit.stat().st_mtime_ns<newest:raise RuntimeError(tag+' stale/missing bitstream')
    flow=(out/(name+'.log')).read_text(encoding='utf-8',errors='replace')
    if not all('Stage completed: '+stage in flow for stage in ['map','interface','pnr','pgm']):
        raise RuntimeError(tag+' incomplete raw flow')
    place=(out/(name+'.place.rpt')).read_text(encoding='utf-8')
    timing=(out/(name+'.timing.rpt')).read_text(encoding='utf-8')
    slacks=timing_slacks(timing)
    result={k:int(re.search(p,place).group(1)) for k,p in
        [('xlr',r'XLRs:\s*(\d+)'),('ram',r'Memory Blocks:\s*(\d+)'),('dsp',r'DSP Blocks:\s*(\d+)')]}
    result['slacks']=slacks
    result['setup_ns']=next(s['slack_ns'] for s in slacks if s['clock']==s['to_clock']==clock and s['kind']=='setup')
    result['hold_ns']=next(s['slack_ns'] for s in slacks if s['clock']==s['to_clock']==clock and s['kind']=='hold')
    if tag=='bin' and result['ram']>8:raise RuntimeError('分桶增量超过 8 RAM')
    warn=(out/(name+'.warn.log')).read_text(encoding='utf-8',errors='replace')
    start=warn.rfind('// Efinity Synthesis Started')
    if start>=0:warn=warn[start:]
    own=[s for s in warn.splitlines() if re.search(r'VERI-(1209|1330)',s) and
        ('/rtl/engine/' in s.replace('\\','/') or '/syn_bin/' in s.replace('\\','/'))]
    if own:raise RuntimeError('F5 位宽告警: '+'\n'.join(own[:20]))
    for ext in ['place.rpt','timing.rpt','hier_util.rpt']:
        (LOGS/('f5_final_'+tag+'_'+ext)).write_bytes((out/(name+'.'+ext)).read_bytes())
    (LOGS/('f5_final_'+tag+'_flow.log')).write_text(flow,encoding='utf-8')
    (LOGS/('f5_final_'+tag+'_warnings.log')).write_text(warn,encoding='utf-8')
    result['scope']='dynamic observation wrapper; not protocol validation' if tag=='bin' else 'complete SoC; 100MHz; unchanged SDK/cache/scan clocks'
    return result
def main():
    p=argparse.ArgumentParser();p.add_argument('--build',nargs='+',choices=PROJECTS);p.add_argument('--collect',action='store_true');p.add_argument('--projects',nargs='+',choices=PROJECTS)
    a=p.parse_args();LOGS.mkdir(exist_ok=True)
    for tag in a.build or []:build(tag)
    if a.collect or a.build:
        result={tag:collect(tag) for tag in (a.projects or a.build or PROJECTS)}
        (LOGS/'f5_synthesis.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
        print('F5 SYNTHESIS PASS '+json.dumps(result,ensure_ascii=False),flush=True)
if __name__=='__main__':main()
