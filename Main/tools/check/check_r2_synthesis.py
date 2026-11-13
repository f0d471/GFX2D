#!/usr/bin/env python3
"""R2 L1/L2 独立四步、RAM/DSP 结构与 B0/B3 直接消费者整机综合。"""
import os
from pathlib import Path
import re
import shutil
import sys
import xml.etree.ElementTree as ET

import feature_build as fb
import feature_synthesis as fs
from check_run import ROOT, IncompleteCheck, command, session, write_json

NS='http://www.efinixinc.com/enf_proj'
ET.register_namespace('efx',NS)
ET.register_namespace('xsi','http://www.w3.org/2001/XMLSchema-instance')


def prepare(folder, l2):
    folder.mkdir(parents=True)
    for src in (ROOT/'Main/rtl/engine/rast/engine_rlst.v',
                ROOT/'Main/rtl/engine/rast/engine_rast.v',
                ROOT/'Main/rtl/engine/include/engine_regs_offsets.vh'):
        shutil.copyfile(src,folder/src.name)
    (folder/'engine_feature.vh').write_text('`define GFX_HAS_RAST\n'+
        ('`define GFX_HAS_RAST_TEX\n' if l2 else ''),encoding='utf-8')
    wrapper='''// 动态观察壳只测资源/时序；协议由真实 AXI 测试验证。
module syn_r2(input wire clk,rst,output reg [127:0] obs);
reg [2047:0] drive;
always @(posedge clk) if(rst) drive<=2048'h7352;
else drive<={drive[2046:0],drive[2047]^drive[2034]^drive[2015]^drive[2000]};
'''
    observed=[];offset=0
    for module in ('engine_rlst','engine_rast'):
        header=(folder/(module+'.v')).read_text(encoding='utf-8').split(');',1)[0]
        if not l2:header=re.sub(r'`ifdef GFX_HAS_RAST_TEX.*?`endif','',header,flags=re.S)
        connections=[]
        for direction,width,port in re.findall(r'^\s*(input|output)\s+(?:wire|reg)\s*(\[[^\]]+\])?\s*(\w+)\s*[,\n]',header,re.M):
            bits=1 if not width else int(width[1:-1].split(':')[0])+1
            if port in ('clk','rst'):value=port
            elif direction=='input':
                value=f'drive[{offset}+:{bits}]';offset+=bits
            else:
                value=module+'_'+port
                wrapper+=f'wire {width or ""} {value};\n'
                observed.append((value,bits))
            connections.append('.'+port+'('+value+')')
        wrapper+=module+' u_'+module+'('+','.join(connections)+');\n'
    assert offset<=2048
    folds=[value if bits==128 else '{'+str(128-bits)+"'d0,"+value+'}' for value,bits in observed]
    wrapper+='always @(posedge clk) if(rst)obs<=0;else obs<='+'^'.join(folds)+';\nendmodule\n'
    (folder/'syn_r2.v').write_text(wrapper,encoding='utf-8')
    template=ROOT/'Main/sim/engine/frontend/syn_bin'
    tree=ET.parse(template/'engine_bin_syn.xml');root=tree.getroot()
    def tag(n):return '{'+NS+'}'+n
    root.set('name','r2_core');root.set('last_run_state','')
    root.set('description','R2 independent dynamic observation wrapper')
    design=root.find(tag('design_info'));design.find(tag('top_module')).set('name','syn_r2')
    for node in list(design.findall(tag('design_file'))):design.remove(node)
    for i,name in enumerate(('engine_rlst.v','engine_rast.v','syn_r2.v')):
        design.insert(1+i,ET.Element(tag('design_file'),{'name':name,'version':'default','library':'default'}))
    root.find(tag('constraint_info')).find(tag('sdc_file')).set('name','r2.sdc')
    # 本目录里源码是平铺的副本，模板里指向 rtl/engine 的 include 参数在这里不成立，去掉。
    syn=root.find(tag('synthesis'))
    for node in list(syn.findall(tag('param'))):
        if node.get('name')=='include':syn.remove(node)
    tree.write(folder/'r2_core.xml',encoding='utf-8')
    peri=(template/'engine_bin_syn.peri.xml').read_text(encoding='utf-8').replace('engine_bin_syn','r2_core')
    (folder/'r2_core.peri.xml').write_text(peri,encoding='utf-8')
    (folder/'r2.sdc').write_text('create_clock -period 10.000 -name clk [get_ports clk]\n',encoding='utf-8')


def main():
    current=session()
    if os.name!='nt':raise IncompleteCheck('R2 综合使用 Windows Efinity Python')
    install=Path(os.environ.get('EFINITY_HOME') or Path(sys.executable).parents[2])
    env=dict(os.environ)
    for key in ('EFINITY_HOME','PYTHONHOME','EFXPT_HOME','EFXPGM_HOME','EFXDBG_HOME','EFXIPM_HOME',
                'EFXSVF_HOME','EFXSERDESDBG_HOME','EFINITY_USER_DIR_INI'):env.pop(key,None)
    build,logs=current.build('r2-synthesis'),current.logs('r2-synthesis')
    summary={}
    for name,l2 in [('L1',False),('L2',True)]:
        folder=build/name;prepare(folder,l2)
        destination=logs/name;destination.mkdir()
        body=command([install/'bin/efx_run.bat','r2_core.xml'],destination/'flow.log',cwd=folder,env=env,timeout=3600)
        assert all(re.search(r'\b'+step+r'\s*:\s*PASS',body) for step in fs.STEPS),body[-1000:]
        out=folder/'outflow'
        place=(out/'r2_core.place.rpt').read_text(encoding='utf-8')
        timing=(out/'r2_core.timing.rpt').read_text(encoding='utf-8')
        result={key:int(re.search(pattern,place).group(1)) for key,pattern in
            [('xlr',r'XLRs:\s*(\d+)'),('ram',r'Memory Blocks:\s*(\d+)'),('dsp',r'DSP Blocks:\s*(\d+)')]}
        result['slacks']=fs.timing_slacks(timing)
        assert all(r['slack_ns']>=0 for r in result['slacks']),result
        netlist=(out/'r2_core.map.v').read_text(encoding='utf-8')
        zram=re.findall(r'^\s*EFX_RAM10\s+([^\s]+g_zbank[^\s]+)',netlist,re.M)
        assert len(zram)==20 and result['ram']==21 and result['dsp']==(7 if l2 else 4),result
        warn=(out/'r2_core.warn.log').read_text(encoding='utf-8')
        assert not re.search(r'VERI-(1209|1330)',warn),warn[-3000:]
        result['z_ram']=len(zram)
        for suffix in ('place.rpt','timing.rpt','hier_util.rpt','warn.log'):
            shutil.copyfile(out/('r2_core.'+suffix),destination/suffix)
        shutil.copyfile(folder/'syn_r2.v',destination/'syn_r2.v')
        current.artifact(out/'r2_core.bit')
        summary[name]=result;write_json(logs/'summary.json',summary)
        print('R2 SYNTHESIS PASS '+name+' '+str(result),flush=True)
    for preset in ('B0','B3'):
        folder=build/preset
        assert folder.resolve().is_relative_to(build.resolve()) and not folder.exists()
        record=fb.build(preset,folder)
        fs.run_flow(folder,env)
        result=fs.collect(folder)
        summary[preset]={**result,'config':record['config'],'caps':record['caps']}
        destination=logs/preset;destination.mkdir()
        shutil.copyfile(folder/'flow.log',destination/'flow.log')
        out=folder/'fpga/gfx2d_ti60/outflow'
        for suffix in ('place.rpt','timing.rpt','warn.log'):
            shutil.copyfile(out/('gfx2d_ti60.'+suffix),destination/suffix)
        current.artifact(out/'gfx2d_ti60.bit')
        write_json(logs/'summary.json',summary)
        print('R2 CONSUMER SYNTHESIS PASS '+preset+' '+str(result),flush=True)


if __name__=='__main__':
    try:main()
    except IncompleteCheck as error:print('INCOMPLETE '+str(error),file=sys.stderr);sys.exit(3)
