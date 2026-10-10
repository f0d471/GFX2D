#!/usr/bin/env python3
"""G2 前期研究：地址/端口、真实 geom/pix 代理、DMA 周期与隔离 RAM 综合。"""
import argparse
from collections import Counter
import hashlib
import json
import re
import subprocess
import xml.etree.ElementTree as ET
from pathlib import Path

MAIN = Path(__file__).resolve().parents[2]
ROOT = MAIN.parent
SIM = MAIN / 'sim/engine/band'
BUILD = SIM / 'build'
LOGS = SIM / 'logs'


def run(argv, name, cwd=SIM, expect='PASS', evidence=None):
    p = subprocess.run([str(a) for a in argv], cwd=cwd, text=True,
                       encoding='utf-8', stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (BUILD / (name + '.log')).write_text(p.stdout, encoding='utf-8')
    if p.returncode or f'RESULT: {expect}' not in p.stdout:
        raise RuntimeError(f'{name}: exit={p.returncode}, expected={expect}; build/{name}.log')
    if evidence and not re.search(evidence, p.stdout):
        raise RuntimeError(f'{name}: missing targeted evidence {evidence}')
    if expect=='FAIL' and re.search(r'WATCHDOG|CMD_TIMEOUT|COMPILE-FAIL|RUNTIME-FAIL',p.stdout):
        raise RuntimeError(f'{name}: abnormal completion cannot count as targeted mutation')
    if expect=='PASS' and re.search(r'WATCHDOG|CMD_TIMEOUT|BAND-BOUNDS|BAND-COLLISION|DMA-MEM',p.stdout):
        raise RuntimeError(f'{name}: diagnostic failure despite PASS marker')
    data = p.stdout.encode()
    dest = LOGS / (name + '.log')
    dest.write_bytes(data)
    if dest.read_bytes() != data:
        raise RuntimeError(f'{name}: copied evidence mismatch')
    return p.stdout


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise RuntimeError('fixture drift: ' + old[:100])
    return text.replace(old, new)


def definitions():
    seen = set()
    for y in range(480):
        strip = y // 16
        for beat in range(160):
            address = 0x1000 + y * 4096 + beat * 16
            off = address - 0x1000
            local = ((off >> 12) - strip * 16) * 160 + ((off & 4095) >> 4)
            if not 0 <= local < 2560:
                raise RuntimeError('BAND address outside compact range')
            seen.add((strip, local))
    if len(seen) != 76800:
        raise RuntimeError('BAND address alias')
    result = dict(pixel_width=32, width=640, strip_h=16, bytes=40960,
                  words128=2560, frame_strips=30, mapped_beats=len(seen),
                  logical_stride=4096, compact_row_bytes=2560,
                  pixel_phase_i3_min_cycles_16x16=128,
                  nominal_frame_background_bursts=960,
                  warning='960 excludes command fetch and non-TEX source reads; RAM block count requires mapping')
    (LOGS / 'definition_checks.json').write_text(json.dumps(result, indent=2)+'\n', encoding='utf-8')
    return result


def prepare_syn():
    ns = '{http://www.efinixinc.com/enf_proj}'
    template = MAIN / 'sim/engine/pix/syn'
    for name, depth, mask, segmented in [('compact_masked',2560,1,0),('compact_wide',2560,0,0),
                                         ('padded_masked',4096,1,0),('segmented_masked',2560,1,1),
                                         ('segmented_wide',2560,0,1)]:
        folder = BUILD / name
        folder.mkdir(parents=True, exist_ok=True)
        tree = ET.parse(template/'engine_pix_syn.xml')
        root = tree.getroot()
        root.set('name', name)
        root.set('description', 'G2 research RAM only; no production BAND')
        design = root.find(ns+'design_info')
        design.find(ns+'top_module').set('name','syn_probe_band_ram')
        for f in list(design.findall(ns+'design_file')):
            design.remove(f)
        design.insert(1,ET.Element(ns+'design_file',name='../../probe_band_ram.v',version='default',library='default'))
        wrapper=(SIM/'syn_probe_band_ram.v').read_text(encoding='utf-8')
        wrapper=f'`define BAND_DEPTH {depth}\n`define BAND_MASK {mask}\n`define BAND_SEGMENTED {segmented}\n'+wrapper
        (folder/'syn_probe_band_ram.v').write_text(wrapper,encoding='utf-8')
        design.insert(2,ET.Element(ns+'design_file',name='syn_probe_band_ram.v',version='default',library='default'))
        root.find(ns+'constraint_info').find(ns+'sdc_file').set('name',name+'.sdc')
        for p in root.iter(ns+'param'):
            if p.get('name')=='work_dir':
                p.set('value',p.get('value').replace('engine_pix',name))
        ET.register_namespace('efx','http://www.efinixinc.com/enf_proj')
        tree.write(folder/(name+'.xml'),encoding='utf-8',xml_declaration=False)
        peri=(template/'engine_pix_syn.peri.xml').read_text(encoding='utf-8')
        (folder/(name+'.peri.xml')).write_text(peri.replace('engine_pix_syn',name),encoding='utf-8')
        (folder/(name+'.sdc')).write_text('create_clock -period 10.000 -name clk [get_ports clk]\n',encoding='utf-8')
        print('Prepared',name,'depth',depth,'pixel mask',mask,'segmented',segmented)


def prepare_pixel_fixture():
    text=(MAIN/'sim/engine/pix/tb_engine_path.v').read_text(encoding='utf-8')
    text=replace_once(text,'module tb_engine_path;','module tb_engine_path;\n    reg [15:0] strip_y0=0;\n    reg src_is_tex=0;')
    text=replace_once(text,".src_tex(1'b0)",'.src_tex(src_is_tex)')
    text=replace_once(text,".clip_y0(16'sd0)",'.clip_y0($signed(strip_y0))')
    text=replace_once(text,".clip_y1(16'sd479)",".clip_y1($signed(strip_y0+16'd15))")
    start=text.index('    engine_mover u_mover(')
    end=text.index('    axi_ram_model #(',start)
    replacement='''    probe_band_local u_local(
        .clk(clk),.rst(rst),.fb_base(dst_base),.strip_y0(strip_y0),
        .rd_addr(rd_addr),.rd_bytes(rd_bytes),.rd_buf(rd_buf),.rd_tex(rd_tex),
        .rd_cidx(rd_cidx),.rd_valid(rd_valid),.rd_ready(rd_ready),
        .wr_addr(wr_addr),.wr_bytes(wr_bytes),.wr_cidx(wr_cidx),
        .wr_valid(wr_valid),.wr_ready(wr_ready),.wr_last_row(wr_last_row),
        .rs_tdata(rs_tdata),.rs_tkeep(rs_tkeep),.rs_tvalid(rs_tvalid),
        .rs_tlast(rs_tlast),.rs_buf(rs_buf),
        .ws_tdata(ws_tdata),.ws_tkeep(ws_tkeep),.ws_tvalid(ws_tvalid),.ws_tready(ws_tready),
        .rd_stat_valid(rd_stat_valid),.rd_stat_err(rd_stat_err),.rd_stat_cidx(rd_stat_cidx),
        .wr_stat_valid(wr_stat_valid),.wr_stat_err(wr_stat_err),
        .wr_stat_cidx(wr_stat_cidx),.wr_stat_last_row(wr_stat_last_row));
    assign tmo_evt=1'b0; assign tmo_dir=1'b0;
    probe_fixture_memory u_ram();
'''
    ramend=text.index('    reg [255:0] cmd_rom',end)
    text=text[:start]+replacement+text[ramend:]
    text=text.replace('cmd_rom [0:63]','cmd_rom [0:95]')
    text=replace_once(text,'sfmt=ss[79:76];','sfmt=ss[79:76];src_is_tex=(ss[73:72]==1);')
    text=replace_once(text,'integer cycle=0;','integer cycle=0;\n    integer strip,br,bw,exec_start,total_exec=0,frag_count=0,fb_word;')
    begin=text.index('        for(ci=0;ci<64;ci=ci+1) begin')
    end=text.index('        cy=0;\n        while(done_count',begin)
    loop='''        $readmemh({`CASE,".init.hex"},u_local.fixture);
        $readmemh({`CASE,".tex.hex"},u_local.tex);
        dst_base=surf_rom[0][31:0];
        for(strip=0;strip<6;strip=strip+1) begin
            @(negedge clk);strip_y0=strip*16;
            for(br=0;br<16;br=br+1) for(bw=0;bw<160;bw=bw+1) begin
                fb_word=(surf_rom[0][31:0]>>4)+(strip*16+br)*256+bw;
                u_local.band[br*160+bw]=u_ram.mem[fb_word];
            end
            exec_start=cycle;
            for(ci=0;ci<96;ci=ci+1) begin
                if(cmd_rom[ci][7:0]==8'hff) ci=96;
                else if($signed(cmd_rom[ci][95:80])<strip*16+16 &&
                        $signed(cmd_rom[ci][95:80])+cmd_rom[ci][127:112]>strip*16) begin
                    submit(ci);expected_done=expected_done+1;frag_count=frag_count+1;
                    cy=0;
                    while(done_count<expected_done && cy<30000) begin @(negedge clk);cy=cy+1;end
                    if(cy==30000) begin errors=errors+1;$display("CMD_TIMEOUT index=%0d",ci);ci=96;end
                end
            end
            total_exec=total_exec+cycle-exec_start;
            if(row_free!==2'b11 || u_local.wb || u_local.rb) begin errors=errors+1;$display("FLUSH_TOO_EARLY");end
            for(br=0;br<16;br=br+1) for(bw=0;bw<160;bw=bw+1) begin
                fb_word=(surf_rom[0][31:0]>>4)+(strip*16+br)*256+bw;
                u_ram.mem[fb_word]=u_local.band[br*160+bw];
            end
        end
        errors=errors+u_local.collision;
        $display("BAND EXEC cycles=%0d fragments=%0d rows=%0d collision=%0d",total_exec,frag_count,row_in,u_local.collision);
'''
    text=text[:begin]+loop+text[end:]
    (BUILD/'tb_band_path.v').write_text(text,encoding='utf-8')


def pixel_experiments():
    source=(MAIN/'tools/check/pix_cases_dump.c').read_text(encoding='utf-8')
    source=replace_once(source,'for (i = 0; i < 72u; i++) {','for (i = 0; i < 16u; i++) {')
    source=replace_once(source,'int src = (i < 24u) ? 1 : 2;','int src = 1;')
    source=replace_once(source,'(int)((i / 4u % 8u) * 8u)','(int)((i / 4u % 3u) * 16u)')
    source=replace_once(source,'(int)((i / 8u) * 10u) + 1','(int)((i / 8u) * 18u) + 10')
    source=replace_once(source,'16, 8, GFX_EQ_ALPHA, 0u, 0, 0u','16, 16, GFX_EQ_ALPHA, 0u, 0, 0u')
    source=replace_once(source,'if (gfx_blit(&list, 3, 0, 0, 0, 0, 95, 640, 1,\n                 GFX_EQ_ALPHA, 0u, 0, 0u) != 0 || gfx_list_end(&list) != 0)',
                        'if (gfx_list_end(&list) != 0)')
    (BUILD/'band_cases.c').write_text(source,encoding='utf-8')
    cfiles=[MAIN/'sw/lib'/p for p in ('gfx/list.c','gfx/strip.c','gfx/strip_plan.c','gfx/check.c','gfx/exec_sw.c','gfx/cases.c','core/format.c')]
    cc=['gcc','-O1','-g','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
        '-I'+str(MAIN/'tools/check'),'-I'+str(MAIN/'sw/lib'),'-o',BUILD/'dump',BUILD/'band_cases.c',*cfiles]
    subprocess.run([str(x) for x in cc],check=True)
    p=subprocess.run([str(BUILD/'dump'),'--tex-sprites',str(BUILD/'hot16')],capture_output=True,text=True)
    (LOGS/'golden_export.log').write_text(p.stdout+p.stderr,encoding='utf-8')
    if p.returncode: raise RuntimeError('golden export sanitizer failed')
    prepare_pixel_fixture()
    rtl=[MAIN/'rtl/engine'/p for p in ('engine_geom.v','engine_pix.v','engine_blend.v')]
    argv=['iverilog','-g2012','-s','tb_engine_path','-DCASE="build/hot16"','-o',BUILD/'path.vvp',BUILD/'tb_band_path.v',SIM/'probe_band_local.v',*rtl]
    subprocess.run([str(x) for x in argv],check=True)
    out=run(['vvp','-n',BUILD/'path.vvp'],'band_path')
    cycles,fragments,rows,collision=map(int,re.search(r'BAND EXEC cycles=(\d+) fragments=(\d+) rows=(\d+) collision=(\d+)',out).groups())
    subprocess.run([str(x) for x in argv[:5]+['-DINJ_BAND_ROUTE']+argv[5:]],check=True)
    run(['vvp','-n',BUILD/'path.vvp'],'band_path_bad_address',expect='FAIL',evidence=r'MEM_MISMATCH|BAND-BOUNDS')
    variants={}
    aligned=replace_once(source,'(int)((i / 8u) * 18u) + 10','(int)((i / 8u) * 16u) + 16')
    keyed=replace_once(source,'(v & 0x003F3F3Fu) | 0x40000000u;',
                       '(i % 5u == 0u) ? 0x40201008u : ((v & 0x003F3F3Fu) | 0x40000000u);')
    keyed=replace_once(keyed,'(int)((i % 8u) * 76u) + 4','(int)((i % 4u) * 13u) - 3')
    keyed=replace_once(keyed,'(int)((i / 8u) * 18u) + 10','(int)((i / 8u) * 6u) + 10')
    keyed=replace_once(keyed,'16, 16, GFX_EQ_ALPHA, 0u, 0, 0u','16, 16, GFX_EQ_ALPHA, 0u, 1, 0x40201008u')
    for name,case_source,stall in [('aligned',aligned,False),('keyed_partial_stall',keyed,True)]:
        (BUILD/'band_cases.c').write_text(case_source,encoding='utf-8')
        subprocess.run([str(x) for x in cc],check=True)
        p=subprocess.run([str(BUILD/'dump'),'--tex-sprites',str(BUILD/name)],capture_output=True,text=True)
        (LOGS/(name+'_golden.log')).write_text(p.stdout+p.stderr,encoding='utf-8')
        if p.returncode: raise RuntimeError(name+' golden export failed')
        bench=(BUILD/'tb_band_path.v').read_text(encoding='utf-8')
        if stall: bench=replace_once(bench,'probe_band_local u_local(',
                                    'probe_band_local #(.WRITE_STALL(1)) u_local(')
        (BUILD/(name+'.v')).write_text(bench,encoding='utf-8')
        case_argv=['iverilog','-g2012','-s','tb_engine_path','-DCASE="build/'+name+'"',
                   '-o',BUILD/'variant.vvp',BUILD/(name+'.v'),SIM/'probe_band_local.v',*rtl]
        subprocess.run([str(x) for x in case_argv],check=True)
        log=run(['vvp','-n',BUILD/'variant.vvp'],'band_'+name)
        vals=list(map(int,re.search(r'BAND EXEC cycles=(\d+) fragments=(\d+) rows=(\d+) collision=(\d+)',log).groups()))
        variants[name]=dict(cycles=vals[0],fragments=vals[1],rows=vals[2],collision=vals[3],golden=p.stdout.strip())
        if stall:
            subprocess.run([str(x) for x in case_argv[:5]+['-DINJ_BAND_LOCAL_MASK']+case_argv[5:]],check=True)
            run(['vvp','-n',BUILD/'variant.vvp'],'band_local_bad_mask',expect='FAIL',evidence='MEM_MISMATCH')
            variants[name]['colored_key']=0x40201008
            variants[name]['mask_mutation_red']=True
    return dict(sprites=16,size='16x16',strips=6,render_exec_cycles=cycles,
                cycles_per_sprite=cycles/16,fragments=fragments,rows=rows,collision=collision,
                variants=variants,
                memory_bytes_compared=524288,
                scope='real geom/pix/blend; local source/BAND proxy; fill/flush not timed here; no command fetch/SoC/scan')


def record_board():
    if (LOGS/'board_artifacts.json').exists():
        raise RuntimeError('identity already registered; use --check-board for unchanged artifacts')
    entries=[]
    for name,rel in [('bit','fpga/gfx2d_ti60/outflow/gfx2d_ti60.bit'),
                     ('engineTest','sw/apps/test/engineTest/build_g2_research/engineTest-O2.bin')]:
        raw=(MAIN/rel).read_bytes()
        entries.append(dict(name=name,path='Main/'+rel,bytes=len(raw),sha256=hashlib.sha256(raw).hexdigest()))
    log=(LOGS/'g2_board_baseline.log').read_text(encoding='utf-8')
    if '# serial lost' in log or not re.search(r'^fail 0\s*$',log,re.M) or not re.search(r'^done\s*$',log,re.M):
        raise RuntimeError('board baseline incomplete/failed')
    if len(re.findall(r'^TEX-[\w-]+ [0-9a-f]{8} [0-9a-f]{8} ok',log,re.M))!=11:
        raise RuntimeError('board TEX CRC count')
    result=dict(date='2026-10-08',source_commit=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
                scope='G2 research baseline; current G1 hardware, freshly rebuilt engineTest; no BAND, Flash unchanged',
                artifacts=entries,consumer_log='g2_board_baseline.log',functional_pass=True)
    (LOGS/'board_artifacts.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print('Board baseline complete: 11 TEX CRCs, fail 0/done; identity registered once')


def check_board():
    text=(LOGS/'g2_board_baseline.log').read_text(encoding='utf-8')
    golden=dict(line.split() for line in (MAIN/'sw/lib/test/golden.txt').read_text(encoding='utf-8').splitlines()
                if line and not line.startswith('#'))
    rows=re.findall(r'^(TEX-)?([\w-]+) ([0-9a-f]{8}) ([0-9a-f]{8}) ok\s*$',text,re.M)
    for prefix in ('','TEX-'):
        selected=[(name,a,b) for p,name,a,b in rows if p==prefix]
        if len(selected)!=11 or {r[0] for r in selected}!=set(golden) or any(a!=b or a!=golden[n] for n,a,b in selected):
            raise RuntimeError('board golden mismatch')
    for name in ('BAND','DST','ALIGN','OOB','NOVAL','FMT'):
        if 'TEXERR-'+name+' ok' not in text: raise RuntimeError('missing TEXERR-'+name)
    if '# serial lost' in text or not re.search(r'^fail 0\s*$',text,re.M) or not re.search(r'^done\s*$',text,re.M):
        raise RuntimeError('board footer/serial')
    ddr=int(re.search(r'^PERF busy=([0-9a-f]+)',text,re.M).group(1),16)/16
    busy,rd=[int(x,16) for x in re.search(r'^PERF tex busy=([0-9a-f]+) rd=([0-9a-f]+)',text,re.M).groups()]
    wm,zero=[int(x,16) for x in re.search(r'wm=([0-9a-f]+) z=([0-9a-f]+)',text).groups()]
    if busy>16*750 or rd!=258 or wm<160 or zero: raise RuntimeError('board performance/scan')
    result=dict(ddr_cycles_per_sprite=ddr,tex_cycles_per_sprite=busy/16,
                tex_ddr_read_bursts=rd,crc_pairs=22,fifo_min=wm,underflow=zero,
                source_manifest='board_artifacts.json',scope='current G1 baseline, not BAND board acceptance')
    (LOGS/'board_results.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print('G2 BOARD BASELINE OK:',json.dumps(result))


def collect_syn():
    results={}
    for name in ('compact_masked','compact_wide','padded_masked','segmented_masked','segmented_wide'):
        folder=BUILD/name
        synth=(folder/'synthesis.log').read_text(encoding='utf-8-sig')
        if not all(re.search(r'\b'+stage+r'\s*:\s*PASS',synth) for stage in ('map','interface','pnr','pgm')):
            raise RuntimeError(name+' synthesis incomplete')
        for suffix in ('place.rpt','timing.rpt','hier_util.rpt'):
            raw=(folder/'outflow'/(name+'.'+suffix)).read_bytes()
            dest=LOGS/(name+'_'+suffix)
            dest.write_bytes(raw)
            if dest.read_bytes()!=raw: raise RuntimeError('report copy mismatch')
        raw=(folder/'synthesis.log').read_bytes()
        (LOGS/(name+'_synthesis.log')).write_bytes(raw)
        if (LOGS/(name+'_synthesis.log')).read_bytes()!=raw: raise RuntimeError('flow log copy mismatch')
        place=(LOGS/(name+'_place.rpt')).read_text(encoding='utf-8')
        timing=(LOGS/(name+'_timing.rpt')).read_text(encoding='utf-8')
        slacks=re.findall(r'^\s*clk\s+clk\s+[\d.]+\s+(-?[\d.]+)\s+\(',timing,re.M)
        if len(slacks)!=2: raise RuntimeError(name+' timing parse')
        hier=(LOGS/(name+'_hier_util.rpt')).read_text(encoding='utf-8')
        dut=re.search(r'\|\s*\+dut\s*\|\s*(\d+)\(',hier)
        results[name]=dict(xlr=int(re.search(r'XLRs:\s*(\d+)',place).group(1)),
                           dut_xlr=int(dut.group(1)) if dut else None,
                           ram=int(re.search(r'Memory Blocks:\s*(\d+)',place).group(1)),
                           dsp=int(re.search(r'DSP Blocks:\s*(\d+)',place).group(1)),
                           setup_ns=float(slacks[0]),hold_ns=float(slacks[1]),
                           scope='standalone research RAM and variable-observation wrapper, not full BAND/SoC')
        netlist=(folder/'outflow'/(name+'.map.v')).read_text(encoding='utf-8')
        modes=dict(Counter(re.findall(r'READ_WIDTH=(\d+)',netlist)))
        if sum(modes.values())!=results[name]['ram']: raise RuntimeError(name+' physical RAM count')
        results[name]['ram_read_width_counts']=modes
        if any(results[name][k]<0 for k in ('setup_ns','hold_ns')): raise RuntimeError(name+' negative slack')
    (LOGS/'synthesis_results.json').write_text(json.dumps(results,indent=2)+'\n',encoding='utf-8')
    (LOGS/'ram_physical_modes.json').write_text(json.dumps(
        {name:row['ram_read_width_counts'] for name,row in results.items()},indent=2)+'\n',encoding='utf-8')
    print('G2 RAM SYN OK:',json.dumps(results))


def dma_experiment():
    original=(MAIN/'sim/engine/pix/tb_engine_path.v').read_text(encoding='utf-8')
    core=original[original.index('    wire rd_stat_valid'):original.index('    reg [255:0] cmd_rom')]
    core=replace_once(core,".drain_req(1'b0)",'.drain_req(drain_req)')
    prefix='''`timescale 1ns/1ps
`default_nettype none
module tb_probe_band_dma;
    reg clk=0,rst=1;always #5 clk=~clk;
    reg [31:0] rd_addr=0,wr_addr=0;
    reg [11:0] rd_beats=160,wr_beats=160;
    wire [11:0] rd_bytes=rd_beats<<4,wr_bytes=wr_beats<<4;
    reg [1:0] rd_buf=0;
    reg [3:0] rd_cidx=0,wr_cidx=0;
    wire rd_tex=1'b0;
    reg rd_valid=0,wr_valid=0,wr_last_row=0,drain_req=0;
    wire rd_ready,wr_ready;
    wire [127:0] rs_tdata,ws_tdata;
    wire [15:0] rs_tkeep,ws_tkeep;
    wire rs_tlast,rs_tvalid,ws_tvalid,ws_tready;
    wire rs_tready=1'b1; wire [1:0] rs_buf;
    reg fill_active=0,flush_active=0,out_valid=0;
    integer fill_row=0,flush_row=0,received=0,next_word=0;
    wire [11:0] ram_wa=fill_row*160+received;
    wire [11:0] ram_ra=flush_row*160+next_word;
    wire ram_re=flush_active && (!out_valid || ws_tready) && next_word<160;
    wire [127:0] ram_q;
    probe_band_ram #(.SEGMENTED(1)) u_band(.clk(clk),.wa(ram_wa),.ra(ram_ra),.wd(rs_tdata),
        .we(fill_active && rs_tvalid ? 4'hf : 4'h0),.re(ram_re),.q(ram_q));
    assign ws_tdata=ram_q;assign ws_tkeep=16'hffff;
    assign ws_tvalid=flush_active && out_valid;
    integer rd_done=0,wr_done=0,cycle=0,ar_count=0,aw_count=0,errors=0;
    integer accepted=0,flush_stalls=0;
    always @(posedge clk) begin
        cycle=cycle+1;
        if(!rst) begin
            if(fill_active && rs_tvalid) received<=received+1;
            if(rd_stat_valid) begin rd_done<=rd_done+1;if(rd_stat_err) errors=errors+1;end
            if(wr_stat_valid) begin wr_done<=wr_done+1;if(wr_stat_err) errors=errors+1;end
            if(arvalid && arready) ar_count=ar_count+1;
            if(awvalid && awready) aw_count=aw_count+1;
            if(tmo_evt) begin errors=errors+1;$display("DMA-TIMEOUT");end
            if(flush_active) begin
                if(!out_valid || ws_tready) out_valid<=ram_re;
                if(ram_re) next_word<=next_word+1;
            end else out_valid<=0;
            if(ws_tvalid && ws_tready) accepted=accepted+1;
            if(ws_tvalid && !ws_tready) flush_stalls=flush_stalls+1;
        end
    end
'''
    suffix='''
    reg [127:0] expected[0:32767];
    integer i,p,row,beat,t0,fill_cycles,flush_cycles,waits,prefix_words,drain_cycles,drain_seen;
    task read_row;
        input integer r;
        begin
            @(negedge clk);fill_row=r;received=0;
            rd_addr=32'h1000+r*4096;rd_valid=1;
            @(posedge clk);while(!rd_ready) @(posedge clk);
            @(negedge clk);rd_valid=0;
            waits=0;
            while((received<160 || rd_done<r+1) && waits<100000) begin @(negedge clk);waits=waits+1;end
            if(waits==100000) begin errors=errors+1;$display("DMA-READ-WATCHDOG");end
        end
    endtask
    task write_row;
        input integer r;
        begin
            @(negedge clk);flush_row=r;next_word=0;out_valid=0;flush_active=1;
`ifdef INJ_BAND_FLUSH_ADDR
            wr_addr=32'h20000+r*4096+16;
`else
            wr_addr=32'h20000+r*4096;
`endif
            wr_valid=1;
            @(posedge clk);while(!wr_ready) @(posedge clk);
            @(negedge clk);wr_valid=0;
            waits=0;
            while(wr_done<r+1 && waits<100000) begin @(negedge clk);waits=waits+1;end
            if(waits==100000) begin errors=errors+1;$display("DMA-WRITE-WATCHDOG");end
            @(negedge clk);flush_active=0;
        end
    endtask
    initial begin
        #1;
        for(i=0;i<32768;i=i+1) begin
            for(p=0;p<4;p=p+1) u_ram.mem[i][32*p +:32]=(i*7+p*32'h654321)&32'h00ffffff;
            expected[i]=u_ram.mem[i];
        end
        for(row=0;row<16;row=row+1) for(beat=0;beat<160;beat=beat+1)
            expected[8192+row*256+beat]=expected[256+row*256+beat];
        repeat(5) @(negedge clk);rst=0;
        t0=cycle;fill_active=1;
        for(row=0;row<16;row=row+1) read_row(row);
        @(negedge clk);fill_active=0;fill_cycles=cycle-t0;
        t0=cycle;
        for(row=0;row<16;row=row+1) write_row(row);
        flush_cycles=cycle-t0;
        $display("BAND DMA fill=%0d flush=%0d ar=%0d aw=%0d words=2560 stalls=%0d",fill_cycles,flush_cycles,ar_count,aw_count,flush_stalls);
        // 中途停止 BAND 供流，mover 继续用不写哑拍排空，保持 DDR 协议完成。
        @(negedge clk);flush_row=0;next_word=0;out_valid=0;flush_active=1;
        accepted=0;wr_addr=32'h30000;wr_valid=1;t0=cycle;
        @(posedge clk);while(!wr_ready) @(posedge clk);
        @(negedge clk);wr_valid=0;
        while(accepted<10) @(negedge clk);
        prefix_words=accepted;flush_active=0;drain_req=1;
        waits=0;while(!drain_done && waits<100000) begin @(negedge clk);waits=waits+1;end
        if(waits==100000) begin errors=errors+1;$display("DRAIN-WATCHDOG");end
        drain_seen=drain_done;drain_cycles=cycle-t0;drain_req=0;
        for(i=0;i<prefix_words;i=i+1) expected[12288+i]=expected[256+i];
        repeat(10) @(negedge clk);
        $display("BAND DRAIN cycles=%0d prefix=%0d seen=%0d wlast_errors=%0d",drain_cycles,prefix_words,drain_seen,u_ram.wlast_err);
        if(drain_seen!=1) errors=errors+1;
        if(u_ram.wlast_err!=0) errors=errors+1;
        for(i=0;i<32768;i=i+1) if(u_ram.mem[i]!==expected[i]) begin
            errors=errors+1;if(errors<8) $display("DMA-MEM addr=%08x",i*16);
        end
        $display("BAND DMA errors=%0d",errors);
        if(errors==0 && ar_count==16 && aw_count==17) $display("RESULT: PASS");
        else $display("RESULT: FAIL");
        $finish;
    end
    initial begin #100000000; $display("DMA-WATCHDOG");$finish;end
endmodule
`default_nettype wire
'''
    bench=BUILD/'tb_probe_band_dma.v'
    bench.write_text(prefix+core+suffix,encoding='utf-8')
    rtl=[MAIN/'rtl/engine/engine_mover.v',MAIN/'rtl/vendor/verilog-axi/axi_dma_rd.v',
         MAIN/'rtl/vendor/verilog-axi/axi_dma_wr.v',MAIN/'sim/axi_probe/axi_ram_model.v',SIM/'probe_band_ram.v']
    argv=['iverilog','-g2012','-s','tb_probe_band_dma','-DTIMED','-o',BUILD/'dma.vvp',bench,*rtl]
    subprocess.run([str(x) for x in argv],check=True)
    out=run(['vvp','-n',BUILD/'dma.vvp'],'band_dma')
    fill,flush,ar,aw,stalls=map(int,re.search(r'BAND DMA fill=(\d+) flush=(\d+) ar=(\d+) aw=(\d+) words=2560 stalls=(\d+)',out).groups())
    dc,dp,ds,dw=map(int,re.search(r'BAND DRAIN cycles=(\d+) prefix=(\d+) seen=(\d+) wlast_errors=(\d+)',out).groups())
    subprocess.run([str(x) for x in argv[:5]+['-DINJ_BAND_FLUSH_ADDR']+argv[5:]],check=True)
    run(['vvp','-n',BUILD/'dma.vvp'],'band_dma_bad_address',expect='FAIL',evidence='DMA-MEM')
    return dict(fill_cycles=fill,flush_cycles=flush,read_bursts=ar,write_bursts=aw,
                flush_stalls=stalls,words128=2560,memory_bytes_compared=524288,
                drain_cycles=dc,drain_prefix_words=dp,drain_done_seen=bool(ds),wlast_errors=dw,
                ram_segmented=True,
                frame_30_strip_fixed_cycles=(fill+flush)*30,
                scope='real mover/DMA/calibrated DDR model; single BAND RAM; producer stops at soft drain; no SoC/scan')


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--prepare-syn',action='store_true')
    ap.add_argument('--record-board',action='store_true')
    ap.add_argument('--check-board',action='store_true')
    ap.add_argument('--collect-syn',action='store_true')
    args=ap.parse_args()
    BUILD.mkdir(parents=True,exist_ok=True);LOGS.mkdir(parents=True,exist_ok=True)
    if args.prepare_syn: prepare_syn();return
    if args.record_board: record_board();return
    if args.check_board: check_board();return
    if args.collect_syn: collect_syn();return
    results={'definitions':definitions()}
    for name,define,want,evidence in [('ram_normal',None,'PASS',None),
                                     ('ram_segmented',None,'PASS',None),
                                     ('ram_bad_mask','INJ_BAND_MASK','FAIL','RAM-DATA'),
                                     ('ram_bad_address','INJ_BAND_ADDR','FAIL','RAM-DATA')]:
        argv=['iverilog','-g2012','-s','tb_probe_band_ram']
        if name=='ram_segmented': argv+=['-Ptb_probe_band_ram.SEGMENTED=1']
        if define: argv+=['-D'+define]
        argv+=['-o',BUILD/'ram.vvp',SIM/'probe_band_ram.v',SIM/'tb_probe_band_ram.v']
        subprocess.run([str(x) for x in argv],check=True)
        out=run(['vvp','-n',BUILD/'ram.vvp'],name,expect=want,evidence=evidence)
        print(name, re.search(r'RESULT: .*',out).group())
    results['pixel_path']=pixel_experiments()
    results['dma']=dma_experiment()
    read_est=16*(27.41+160*13/12)
    write_est=16*(7.25+160*7/6)
    results['model']=dict(fill_estimate_cycles=read_est,flush_estimate_cycles=write_est,
                          fill_difference_percent=100*(results['dma']['fill_cycles']/read_est-1),
                          flush_difference_percent=100*(results['dma']['flush_cycles']/write_est-1),
                          note='calibrated sequential descriptor model; no command fetch/SoC/HDMI contention')
    (LOGS/'research_results.json').write_text(json.dumps(results,indent=2)+'\n',encoding='utf-8')
    print('G2 RESEARCH OK:',json.dumps(results))


if __name__=='__main__':
    main()
