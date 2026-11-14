#!/usr/bin/env python3
"""G2 正式验证入口：软件金标准、完整通路、数据面协议与对应变异。"""
import argparse
import hashlib
import json
import os
import re
import subprocess
import xml.etree.ElementTree as ET
from collections import Counter
from datetime import date
from pathlib import Path
from g2_research import replace_once

MAIN=Path(__file__).resolve().parents[2]
SIM=MAIN/'sim/engine/band'
from check_run import paths
from feature_build import write_headers
BUILD,LOGS=paths('band','band')
INCLUDE=write_headers('B3',BUILD/'include')


def compile_run(name,bench,defines=(),params=(),expect='PASS',evidence=None):
    rtl=[MAIN/'rtl/engine'/p for p in ('buffer/engine_band.v','core/engine_mover.v','core/engine_geom.v','core/engine_pix.v','core/engine_blend.v','buffer/engine_tex.v')]
    rtl += [MAIN/'rtl/vendor/verilog-axi/axi_dma_rd.v',MAIN/'rtl/vendor/verilog-axi/axi_dma_wr.v',MAIN/'sim/axi_probe/axi_ram_model.v']
    argv=['iverilog','-g2012','-I'+str(INCLUDE),'-I'+str(MAIN/'rtl/engine/include'),'-s',bench.stem,'-DCASE="'+str(BUILD/('empty' if 'EMPTY_CASE' in defines else 'scene'))+'"']
    argv += ['-D'+x for x in defines]
    argv += ['-P'+bench.stem+'.'+x for x in params]
    argv += ['-o',str(BUILD/(name+'.vvp')),str(bench),*[str(x) for x in rtl]]
    p=subprocess.run(argv,cwd=SIM,capture_output=True,text=True)
    (LOGS/('formal_'+name+'_compile.log')).write_text(p.stdout+p.stderr,encoding='utf-8')
    if p.returncode:raise RuntimeError(name+' compile failed')
    p=subprocess.run(['vvp','-n',str(BUILD/(name+'.vvp'))],cwd=SIM,capture_output=True,text=True)
    text=p.stdout+p.stderr
    (LOGS/('formal_'+name+'.log')).write_text(text,encoding='utf-8')
    if p.returncode or 'RESULT: '+expect not in text or 'WATCHDOG' in text:
        raise RuntimeError(name+' result failed; see formal_'+name+'.log')
    if evidence and not re.search(evidence,text):raise RuntimeError(name+' targeted evidence missing')
    print(name+': RESULT: '+expect+' (as expected)')
    return text


def golden():
    src=(MAIN/'tools/check/pix_cases_dump.c').read_text(encoding='utf-8')
    src=replace_once(src,'for (i = 0; i < 72u; i++) {','for (i = 0; i < 16u; i++) {')
    src=replace_once(src,'int src = (i < 24u) ? 1 : 2;','int src = (i < 8u) ? 1 : 2;')
    src=replace_once(src,'(v & 0x003F3F3Fu) | 0x40000000u;',
                     '(i % 5u == 0u) ? 0x40201008u : ((v & 0x003F3F3Fu) | 0x40000000u);')
    src=replace_once(src,'(int)((i / 4u % 8u) * 8u)','(int)((i / 4u % 3u) * 16u)')
    src=replace_once(src,'(int)((i % 8u) * 76u) + 4','(int)((i % 4u) * 13u) - 3')
    src=replace_once(src,'(int)((i / 8u) * 10u) + 1','(int)((i / 8u) * 6u) + 10')
    src=replace_once(src,'16, 8, GFX_EQ_ALPHA, 0u, 0, 0u','16, 16, GFX_EQ_ALPHA, 0u, 1, 0x40201008u')
    src=replace_once(src,'(((i >> 10) & 0xFFu) << 16)',
                     '((i % 3u) << 30) | (((i >> 10) & 0xFFu) << 16)')
    src=replace_once(src,'gfx_list_begin(&list, cmds, 96u, &table);',
                     's.base=TWIN_BASE+0x2000u; s.width=640u; s.height=2u; s.stride_log2=12u; s.space=GFX_SPACE_DDR;\n'
                     '    if(gfx_surface_set(&table,4,&s)!=0) return 2;\n'
                     '    gfx_list_begin(&list, cmds, 96u, &table);\n#ifndef G2_EMPTY')
    src=replace_once(src,'if (gfx_blit(&list, 3, 0, 0, 0, 0, 95, 640, 1,',
                     'if (gfx_blit(&list, 4, 0, 0, 0, 0, 94, 640, 2,')
    src=replace_once(src,'GFX_EQ_ALPHA, 0u, 0, 0u) != 0 || gfx_list_end(&list) != 0)',
                     'GFX_EQ_ALPHA, 0u, 0, 0u) != 0)')
    src=replace_once(src,'m.dev_base = 0; m.host = buf; m.bytes = MEM_BYTES;\n    m.tex_host = texbuf;',
                     '#endif\n    if(gfx_list_end(&list)!=0) return 2;\n'
                     '    m.dev_base = 0; m.host = buf; m.bytes = MEM_BYTES;\n    m.tex_host = texbuf;')
    p=BUILD/'golden.c';p.write_text(src,encoding='utf-8')
    files=[MAIN/'sw/lib'/x for x in ('gfx/list.c','gfx/strip.c','gfx/strip_plan.c','gfx/check.c','gfx/exec_sw.c','gfx/cases.c','core/format.c')]
    for name,defs in [('scene',[]),('empty',['-DG2_EMPTY'])]:
        argv=['gcc','-O1','-g','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',*defs,
              '-I'+str(MAIN/'tools/check'),'-I'+str(MAIN/'sw/lib'),'-o',str(BUILD/'dump'),str(p),*[str(x) for x in files]]
        subprocess.run(argv,check=True)
        r=subprocess.run([str(BUILD/'dump'),'--tex-sprites',str(BUILD/name)],capture_output=True,text=True)
        (LOGS/('formal_'+name+'_golden.log')).write_text(r.stdout+r.stderr,encoding='utf-8')
        if r.returncode:raise RuntimeError(name+' golden sanitizer failed')


def prepare_syn():
    ns='{http://www.efinixinc.com/enf_proj}'
    folder=BUILD/'syn';folder.mkdir(parents=True,exist_ok=True)
    template=MAIN/'sim/engine/pix/syn'
    tree=ET.parse(template/'engine_pix_syn.xml');root=tree.getroot()
    root.set('name','engine_band_syn');root.set('description','G2 formal BAND standalone')
    design=root.find(ns+'design_info');design.find(ns+'top_module').set('name','syn_engine_band')
    for f in list(design.findall(ns+'design_file')):design.remove(f)
    for i,p in enumerate([MAIN/'rtl/engine/buffer/engine_band.v',SIM/'syn_engine_band.v']):
        design.insert(1+i,ET.Element(ns+'design_file',name=os.path.relpath(p,folder).replace('\\','/'),version='default',library='default'))
    root.find(ns+'constraint_info').find(ns+'sdc_file').set('name','engine_band_syn.sdc')
    for p in root.iter(ns+'param'):
        if p.get('name')=='work_dir':p.set('value',p.get('value').replace('engine_pix','engine_band'))
    ET.register_namespace('efx','http://www.efinixinc.com/enf_proj')
    tree.write(folder/'engine_band_syn.xml',encoding='utf-8',xml_declaration=False)
    peri=(template/'engine_pix_syn.peri.xml').read_text(encoding='utf-8')
    (folder/'engine_band_syn.peri.xml').write_text(peri.replace('engine_pix_syn','engine_band_syn'),encoding='utf-8')
    (folder/'engine_band_syn.sdc').write_text('create_clock -period 10.000 -name clk [get_ports clk]\n',encoding='utf-8')
    print('Prepared formal standalone BAND synthesis:',folder)


def collect_syn():
    folder=BUILD/'syn';name='engine_band_syn'
    synth=(folder/'synthesis.log').read_text(encoding='utf-8-sig')
    if not all(re.search(r'\b'+s+r'\s*:\s*PASS',synth) for s in ('map','interface','pnr','pgm')):
        raise RuntimeError('formal synthesis incomplete')
    for suffix in ('place.rpt','timing.rpt','hier_util.rpt'):
        raw=(folder/'outflow'/(name+'.'+suffix)).read_bytes()
        dest=LOGS/('formal_'+suffix);dest.write_bytes(raw)
        if dest.read_bytes()!=raw:raise RuntimeError('report copy mismatch')
    raw=(folder/'synthesis.log').read_bytes()
    dest=LOGS/'formal_synthesis.log';dest.write_bytes(raw)
    if dest.read_bytes()!=raw:raise RuntimeError('flow log copy mismatch')
    place=(LOGS/'formal_place.rpt').read_text(encoding='utf-8')
    timing=(LOGS/'formal_timing.rpt').read_text(encoding='utf-8')
    slacks=re.findall(r'^\s*clk\s+clk\s+[\d.]+\s+(-?[\d.]+)\s+\(',timing,re.M)
    if len(slacks)!=2:raise RuntimeError('formal timing parse')
    netlist=(folder/'outflow'/(name+'.map.v')).read_text(encoding='utf-8')
    modes=dict(Counter(re.findall(r'READ_WIDTH=(\d+)',netlist)))
    result=dict(xlr=int(re.search(r'XLRs:\s*(\d+)',place).group(1)),
                ram=int(re.search(r'Memory Blocks:\s*(\d+)',place).group(1)),
                dsp=int(re.search(r'DSP Blocks:\s*(\d+)',place).group(1)),
                setup_ns=float(slacks[0]),hold_ns=float(slacks[1]),ram_read_width_counts=modes,
                scope='formal engine_band plus variable observation wrapper; not full SoC or enabled BAND')
    if result['ram']!=36 or sum(modes.values())!=36:
        raise RuntimeError('RAM removed/unexpected mapping or timing parse')
    if result['setup_ns']<0 or result['hold_ns']<0.026:
        raise RuntimeError('formal timing below acceptance threshold')
    (LOGS/'formal_synthesis_results.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print('G2 FORMAL SYN OK:',json.dumps(result))


def collect_main():
    folder=MAIN/'fpga/gfx2d_ti60';name='gfx2d_ti60'
    synth=(folder/'outflow/g2_synthesis.log').read_text(encoding='utf-8-sig')
    if not all(re.search(r'\b'+s+r'\s*:\s*PASS',synth) for s in ('map','interface','pnr','pgm')):
        raise RuntimeError('main synthesis incomplete')
    for suffix in ('place.rpt','timing.rpt','hier_util.rpt'):
        raw=(folder/'outflow'/(name+'.'+suffix)).read_bytes()
        dest=LOGS/('formal_main_'+suffix);dest.write_bytes(raw)
        if dest.read_bytes()!=raw:raise RuntimeError('main report copy mismatch')
    place=(LOGS/'formal_main_place.rpt').read_text(encoding='utf-8')
    timing=(LOGS/'formal_main_timing.rpt').read_text(encoding='utf-8')
    tables=timing[timing.index('Setup (Max) Clock Relationship'):timing.index('NOTE: Values')]
    rows=re.findall(r'^\s*(\w+)\s+(\w+)\s+(-?[\d.]+)\s+(-?[\d.]+)\s+\(',tables,re.M)
    if len(rows)!=16 or any(float(x[3])<0 for x in rows):raise RuntimeError('main clock timing incomplete/negative')
    expected=['jtag_inst1_DRCK','jtag_inst1_TMS','jtag_inst1_RESET','jtag_inst1_RUNTEST','jtag_inst1_UPDATE','hdmi_pll_fb']
    missing=place[place.index('| Missing Interface Pins'):place.index('IO Interface Summary (end)')]
    if any(x not in missing for x in expected) or len(re.findall(r'\|\s*\w+\s*\|\s*(?:Input|Output)\s*\|',missing))!=6:
        raise RuntimeError('main interface changed')
    warn_checks={}
    for tag,out in [('band',BUILD/'syn/outflow/engine_band_syn.warn.log'),('main',folder/'outflow/gfx2d_ti60.warn.log')]:
        warn=out.read_text(encoding='utf-8-sig').split('// Efinity Synthesis Started')[-1]
        records=re.findall(r'\[EFX-.*?(?=\[EFX-|\Z)',warn,re.S)
        own=[r for r in records if 'rtl' in r and re.search(r'engine_(?:band|mover|geom)\.v',r)]
        bad=[r for r in own if 'VERI-WARNING' in r or 'truncated' in r.lower()]
        if bad:raise RuntimeError(tag+' new own RTL width/compile warning')
        (LOGS/('formal_'+tag+'_latest_warn.log')).write_text(warn,encoding='utf-8')
        warn_checks[tag]=dict(own_warning_records=len(own),own_compile_or_width_warnings=0,
                              note='latest synthesis section; EFX-0200 redundant signals are expected')
    raw=(folder/'outflow/g2_synthesis.log').read_bytes();dest=LOGS/'formal_main_synthesis.log';dest.write_bytes(raw)
    if dest.read_bytes()!=raw:raise RuntimeError('main flow log copy mismatch')
    result=dict(xlr=int(re.search(r'XLRs:\s*(\d+)',place).group(1)),
                ram=int(re.search(r'Memory Blocks:\s*(\d+)',place).group(1)),
                dsp=int(re.search(r'DSP Blocks:\s*(\d+)',place).group(1)),
                core_setup_ns=float(rows[0][3]),core_hold_ns=float(rows[8][3]),
                minimum_hold_ns=min(float(r[3]) for r in rows[8:]),
                clocks=[dict(launch=r[0],capture=r[1],constraint_ns=float(r[2]),slack_ns=float(r[3])) for r in rows],
                missing_interface_pins=expected,warnings=warn_checks,
                scope='G2 wiring present, frame BAND controls tied zero and CAPS.BAND=0; no enabled BAND RAM')
    (LOGS/'formal_main_results.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print('G2 MAIN SYN OK:',json.dumps({k:result[k] for k in ('xlr','ram','dsp','core_setup_ns','core_hold_ns','minimum_hold_ns')}))


def record_board():
    manifest=LOGS/'formal_artifacts.json'
    if manifest.exists():raise RuntimeError('formal artifacts already registered; do not rehash unchanged files')
    raw_log=(LOGS/'formal_board_legacy.log').read_text(encoding='utf-8')
    marker='# --- 程序已加载，以下才是它的输出 ---'
    if marker not in raw_log:raise RuntimeError('board program load marker missing')
    log=raw_log.split(marker)[-1]
    golden=dict(line.split() for line in (MAIN/'sw/lib/test/golden.txt').read_text(encoding='utf-8').splitlines()
                if line and not line.startswith('#'))
    rows=re.findall(r'^(TEX-)?([\w-]+) ([0-9a-f]{8}) ([0-9a-f]{8}) ok\s*$',log,re.M)
    for prefix in ('','TEX-'):
        selected=[(n,a,b) for p,n,a,b in rows if p==prefix]
        if len(selected)!=11 or {r[0] for r in selected}!=set(golden) or any(a!=b or a!=golden[n] for n,a,b in selected):
            raise RuntimeError('board CRC mismatch')
    for name in ('BAND','DST','ALIGN','OOB','NOVAL','FMT'):
        if 'TEXERR-'+name+' ok' not in log:raise RuntimeError('missing error '+name)
    if '# serial lost' in log or not re.search(r'^fail 0\s*$',log,re.M) or not re.search(r'^done\s*$',log,re.M):
        raise RuntimeError('board footer/serial failure')
    ddr=int(re.search(r'^PERF busy=([0-9a-f]+)',log,re.M).group(1),16)/16
    tex,rd=[int(x,16) for x in re.search(r'^PERF tex busy=([0-9a-f]+) rd=([0-9a-f]+)',log,re.M).groups()]
    wm,z=[int(x,16) for x in re.search(r'wm=([0-9a-f]+) z=([0-9a-f]+)',log).groups()]
    if tex>16*750 or rd!=258 or wm<160 or z:raise RuntimeError('board perf/scan failure')
    entries=[]
    for name,rel in [('bit','fpga/gfx2d_ti60/outflow/gfx2d_ti60.bit'),('engineTest','sw/apps/test/engineTest/build_g2_formal/engineTest-O2.bin')]:
        raw=(MAIN/rel).read_bytes()
        entries.append(dict(name=name,path='Main/'+rel,bytes=len(raw),sha256=hashlib.sha256(raw).hexdigest()))
    base=subprocess.check_output(['git','rev-parse','HEAD'],cwd=MAIN,text=True).strip()
    manifest.write_text(json.dumps(dict(date=date.today().isoformat(),base_commit=base,source='working tree G2 implementation; source diff delivered in same commit',
        scope='new full bit legacy DDR/TEX regression; BAND disabled; no Flash or 9208 replay',
        artifacts=entries,consumer_log='formal_board_legacy.log'),indent=2)+'\n',encoding='utf-8')
    result=dict(crc_pairs=22,ddr_cycles_per_sprite=ddr,tex_cycles_per_sprite=tex/16,tex_read_bursts=rd,fifo_min=wm,underflow=z,
                functional_pass=True,identity_manifest='formal_artifacts.json',scope='legacy board regression, not BAND board performance')
    (LOGS/'formal_board_results.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print('G2 LEGACY BOARD OK:',json.dumps(result))


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--case',default='all')
    ap.add_argument('--prepare-syn',action='store_true')
    ap.add_argument('--collect-syn',action='store_true')
    ap.add_argument('--collect-main',action='store_true')
    ap.add_argument('--record-board',action='store_true')
    args=ap.parse_args();BUILD.mkdir(parents=True,exist_ok=True);LOGS.mkdir(parents=True,exist_ok=True)
    if args.prepare_syn:
        prepare_syn();return
    if args.collect_syn:
        collect_syn();return
    if args.collect_main:
        collect_main();return
    if args.record_board:
        record_board();return
    golden()
    bench=SIM/'tb_engine_band_path.v'
    cases=[('path_random',(),(),'PASS',None),('path_timed',('TIMED',),(),'PASS',None),
           ('path_overlap',('NO_FETCH',),(),'PASS',None),
           ('path_seed7',(),('MODEL_SEED=7',),'PASS',None),('empty',('EMPTY_CASE',),(),'PASS',None),
           ('height8',(),('STRIP_H=8',),'PASS',None),('height32',(),('STRIP_H=32',),'PASS',None)]
    # 八个变异体（BAND 五个、mover 三个）已移到 Main/sim/mutation/mutants.yaml，
    # 由 path_random / empty / control 三条正常用例承担发现责任。
    if args.case!='all' and args.case not in {c[0] for c in cases}:
        ap.error('unknown --case '+args.case)
    summary={}
    if args.case=='all':
        for name,unit,defs,want,reason in [
            ('unit',SIM/'tb_engine_band.v',(),'PASS',None),
            ('control',SIM/'tb_engine_band_mover.v',(),'PASS',None)]:
            compile_run(name,unit,defs,expect=want,evidence=reason)
            summary[name]=dict(expected=want,evidence=reason,passed=True)
        print('G2 变异体已移到 Main/sim/mutation/mutants.yaml（drain_mask/raw_mask 由 control/empty 承担）')
    for name,defs,params,want,reason in cases:
        if args.case!='all' and name!=args.case:continue
        text=compile_run(name,bench,defs,params,want,reason)
        summary[name]=dict(expected=want,evidence=reason,passed=True)
        if want=='PASS':
            summary[name]['phases']=[dict(strip=int(i),height=int(h),load=int(l),exec=int(e),flush=int(f)) for i,h,l,e,f in
                re.findall(r'PHASE strip=(\d+) height=(\d+) load=(\d+) exec=(\d+) flush=(\d+)',text)]
            summary[name]['total_cycles']=int(re.search(r'BAND FORMAL cycles=(\d+)',text).group(1))
    (LOGS/'formal_results.json').write_text(json.dumps(summary,indent=2)+'\n',encoding='utf-8')
    print('G2 FORMAL OK:',len(summary),'checks as expected')


if __name__=='__main__':
    main()
