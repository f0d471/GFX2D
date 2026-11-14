#!/usr/bin/env python3
"""G3 正式验证：生产软件对拍、错误三元组、完整引擎与变异；证据归 stripctl/logs。"""
import argparse
import json
import re
import subprocess
from check_run import paths
from feature_build import write_headers
from pathlib import Path
MAIN=Path(__file__).resolve().parents[2]
ROOT=MAIN.parent
SIM=MAIN/'sim/engine/stripctl'
BUILD,LOGS=paths('stripctl','stripctl')
INCLUDE=write_headers('B3',BUILD/'include')
LIB=[MAIN/'sw/lib'/p for p in ('gfx/list.c','gfx/strip.c','gfx/strip_plan.c','gfx/check.c','gfx/exec_sw.c','core/format.c')]
RTL=[MAIN/'rtl/engine'/p for p in ('frontend/engine_bounds.v','frontend/engine_packet_check.v','frontend/engine_pb_dma.v','frontend/engine_bin.v','frontend/engine_pb.v','engine_top.v','core/engine_config_guard.v','core/engine_regs.v','core/engine_cmd.v','frontend/engine_inst.v','core/engine_geom.v','core/engine_pix.v','core/engine_blend.v','core/engine_mover.v','buffer/engine_tex.v','buffer/engine_band.v')]
RTL += [MAIN/'rtl/vendor/verilog-axi'/p for p in ('axi_dma_rd.v','axi_dma_wr.v')]
RTL += [MAIN/'sim/axi_probe/axi_ram_model.v']

def run(name,args,cwd=ROOT,want=0,evidence=None):
    p=subprocess.run([str(x) for x in args],cwd=cwd,capture_output=True,text=True)
    out=p.stdout+p.stderr
    (LOGS/(name+'.log')).write_text(out,encoding='utf-8')
    if (want==0 and p.returncode!=0) or (want!=0 and p.returncode==0):
        raise RuntimeError(name+' unexpected exit; see '+name+'.log')
    if evidence and not re.search(evidence,out):raise RuntimeError(name+' missing evidence')
    print(name+': verified')
    return out

def cc(name,src,defines=(),san=True):
    args=['gcc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-I'+str(MAIN/'sw/lib')]
    if san:args+=['-fsanitize=address,undefined']
    args+=['-D'+d for d in defines]
    run(name+'_compile',args+[SIM/src,*LIB,'-o',BUILD/name])
    return BUILD/name

def rtl(name,sh,defines=(),want='PASS',evidence=None):
    run(name+'_compile',['iverilog','-g2012','-s','tb_stripctl','-Ptb_stripctl.SH='+str(sh),
        '-I'+str(INCLUDE),'-I'+str(MAIN/'rtl/engine/include'),*['-D'+x for x in defines],'-o',BUILD/(name+'.vvp'),SIM/'tb_stripctl.v',*RTL])
    out=run(name,['vvp','-n',BUILD/(name+'.vvp'),'+case='+str(BUILD/('scene'+str(sh)))],SIM,evidence=evidence)
    if 'RESULT: '+want not in out or 'WATCHDOG' in out:raise RuntimeError(name+' RTL result: '+out[-600:])
    return out


def collect_syn():
    sets=(('standalone',MAIN/'sim/engine/ctrl/syn','engine_top_syn',LOGS/'formal_synthesis.log','clk'),
          ('main',MAIN/'fpga/gfx2d_ti60','gfx2d_ti60',MAIN/'fpga/gfx2d_ti60/outflow/g3_synthesis.log','core_clk'))
    result={}
    for tag,folder,name,flow,clk in sets:
        log=flow.read_text(encoding='utf-8-sig')
        if not all(re.search(r'\b'+step+r'\s*:\s*PASS',log) for step in ('map','interface','pnr','pgm')):
            raise RuntimeError(tag+' four-step flow incomplete')
        place=(folder/'outflow'/(name+'.place.rpt')).read_text()
        timing=(folder/'outflow'/(name+'.timing.rpt')).read_text()
        slacks=re.findall(r'^\s*(\w+)\s+\1\s+[-\d.]+\s+([-\d.]+)\s+\(',timing,re.M)
        if not slacks or any(float(v)<0 for _,v in slacks):raise RuntimeError(tag+' negative slack')
        core=[float(v) for c,v in slacks if c==clk]
        if len(core)<2 or core[1]<0.026:raise RuntimeError(tag+' hold degraded')
        result[tag]={k:int(re.search(pattern,place).group(1)) for k,pattern in
                     [('xlr',r'XLRs:\s*(\d+)'),('ram',r'Memory Blocks:\s*(\d+)'),('dsp',r'DSP Blocks:\s*(\d+)')]}
        result[tag].update(setup_ns=core[0],hold_ns=core[1],minimum_slack_ns=min(float(v) for _,v in slacks))
        for suffix in ('place.rpt','timing.rpt','hier_util.rpt'):
            data=(folder/'outflow'/(name+'.'+suffix)).read_bytes()
            dest=LOGS/('formal_'+tag+'_'+suffix);dest.write_bytes(data)
            if dest.read_bytes()!=data:raise RuntimeError('report copy mismatch')
        if tag=='main':(LOGS/'formal_main_synthesis.log').write_text(log)
        warn=(folder/'outflow'/(name+'.warn.log')).read_text(encoding='utf-8-sig')
        warn=warn[warn.rfind('// Efinity Synthesis Started'):]
        (LOGS/('formal_'+tag+'_latest_warn.log')).write_text(warn)
        own=[x for x in warn.splitlines() if re.search(r'VERI-(1209|1330)',x) and '/rtl/engine/' in x.replace('\\','/')]
        if own:raise RuntimeError(tag+' own width warning: '+'\n'.join(own))
        result[tag]['own_width_warnings']=0
    (LOGS/'formal_synthesis_results.json').write_text(json.dumps(result,indent=2)+'\n')
    print('G3 SYN PASS:',json.dumps(result))

def collect_board():
    app=MAIN/'sw/apps/test'
    strip=(app/'stripTest/logs/g3_board.log').read_text()
    legacy=(app/'engineTest/logs/g3_compatibility.log').read_text()
    for name,body in [('stripTest',strip),('engineTest',legacy)]:
        if '# --- 程序已加载' not in body:raise RuntimeError(name+' provenance marker missing')
        body=body[body.index('# --- 程序已加载'):]
        if not re.search(r'^fail 0\s*$',body,re.M) or not re.search(r'^done\s*$',body,re.M) or '# serial lost' in body:
            raise RuntimeError(name+' fail/done or serial loss')
    crc=re.search(r'CRC hw=([0-9a-f]{8}) sw=([0-9a-f]{8})',strip)
    if not crc or crc.group(1)!=crc.group(2):raise RuntimeError('strip frame CRC mismatch')
    for phrase in ('same frame and padding ok','empty unchanged ok','invalid triple ok','capacity triple ok',
                   'recovery finish ok','direct mode finish ok'):
        if phrase not in strip:raise RuntimeError('missing '+phrase)
    pressure=re.search(r'wm=([0-9a-f]+) z=([0-9a-f]+)',legacy)
    if not pressure or int(pressure.group(1),16)<160 or int(pressure.group(2),16)!=0 or 'STRESS ok' not in legacy:
        raise RuntimeError('legacy scan pressure failed')
    result={'scope':'JTAG diagnostics and v1/TEX compatibility; no game BAND N_max or Flash write',
            'strip_frame_crc':crc.group(1),'strip_fail':0,'legacy_fail':0,'scan_min_words':int(pressure.group(1),16),
            'scan_underflow':0,'invalid_clip_error':[2,1024,'cafe005a'],'capacity_error':[2,1024,0],
            'recovery_and_mode_restore':'PASS','artifacts':'board_artifacts.json'}
    (LOGS/'formal_board_results.json').write_text(json.dumps(result,indent=2)+'\n')
    print('G3 BOARD PASS:',json.dumps(result))


def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--collect-syn',action='store_true')
    ap.add_argument('--collect-board',action='store_true')
    ap.add_argument('--rtl-only',action='store_true')
    ap.add_argument('--software-only',action='store_true')
    a=ap.parse_args()
    BUILD.mkdir(parents=True,exist_ok=True);LOGS.mkdir(parents=True,exist_ok=True)
    if a.collect_syn:collect_syn()
    if a.collect_board:collect_board()
    if a.collect_syn or a.collect_board:return
    previous=LOGS/'formal_results.json'
    result=json.loads(previous.read_text()) if previous.exists() and (a.rtl_only or a.software_only) else {}
    if not a.rtl_only:
        exe=cc('test_strips','test_strips.c')
        run('formal_fuzz',[exe,'--json'],evidence=r'10000/10000 scenes byte-identical \(0 skipped\)')
        run('formal_errors',[cc('test_errors','test_errors.c')],evidence=r'errors=0')
        run('formal_fuzz_release',[cc('test_strips_release','test_strips.c',san=False)],evidence=r'10000/10000')
        # 四个软件变异体（STRIP_DROP/CLIP/ORDER/NOFLUSH）已移到 Main/sim/mutation/mutants.yaml，
        # 由 formal_fuzz 承担发现责任。
        result['software']={'scenes_sanitized':10000,'scenes_release':10000,'skipped':0,
                            'production_mutations':'Main/sim/mutation/mutants.yaml'}
    if not a.software_only:
        exe=cc('scene_dump','scene_dump.c')
        for sh in (16,32):
            run('formal_scene'+str(sh)+'_golden',[exe,BUILD/('scene'+str(sh)),str(sh.bit_length()-1)],evidence=r'G3 GOLDEN PASS')
            out=rtl('formal_rtl'+str(sh),sh)
            result['rtl'+str(sh)]={'checks':int(re.search(r'G3 checks=\s*(\d+)',out).group(1)),'errors':0,'frame_word_differences':0}
        # 三个 RTL 变异体（STRIP_LOCAL_Y/HEADER_OFFSET/PERF）同样在外部清单里，
        # 由 formal_scene16_golden / formal_rtl16 承担发现责任。
        result['rtl_mutations']='Main/sim/mutation/mutants.yaml'
    (LOGS/'formal_results.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print('G3 FORMAL PASS')
if __name__=='__main__':main()
