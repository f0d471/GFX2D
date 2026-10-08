#!/usr/bin/env python3
"""复现 G1 前期研究。WSL 中从仓库根运行；产物放 tex/build，证据放 tex/logs。"""
import json
import os
import re
import subprocess
import sys
from pathlib import Path

MAIN = Path(__file__).resolve().parents[2]
SIM = MAIN / 'sim/engine/tex'
BUILD = SIM / 'build'
LOGS = SIM / 'logs'


def run(name, argv, cwd=SIM):
    argv = [os.path.relpath(a, cwd) if isinstance(a, Path) else str(a) for a in argv]
    p = subprocess.run(argv, cwd=cwd, text=True,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (BUILD / (name + '.log')).write_text(p.stdout, encoding='utf-8')
    if p.returncode:
        raise RuntimeError(f'{name}: exit {p.returncode}; see build/{name}.log')
    return p.stdout


def replace_once(s, old, new):
    if s.count(old) != 1:
        raise RuntimeError(f'fixture drift: {old[:80]!r}')
    return s.replace(old, new)


def main():
    BUILD.mkdir(parents=True, exist_ok=True)
    LOGS.mkdir(parents=True, exist_ok=True)
    if '--record-board' in sys.argv:
        import hashlib
        artifacts = []
        # 对照对象为 engine 第 06 章 5.3、5.4 节登记的 B6 产物。
        for rel, expected in [
            ('fpga/gfx2d_ti60/outflow/gfx2d_ti60.bit',
             'b2118f54501cde5d6f3c8c8143aa45a578fa04aa32a797e37f4a2268d915cf63'),
            ('sw/apps/test/engineTest/build/engineTest-O2.bin',
             '482aaccf10cc2d0683b7d0a74891a4c8fa2818044bb86f79e152c436005ef013')]:
            data = (MAIN / rel).read_bytes()
            digest = hashlib.sha256(data).hexdigest()
            if digest != expected:
                raise RuntimeError(f'{rel}: differs from archived B6 artifact; reassess board baseline')
            artifacts.append(dict(path='Main/' + rel, bytes=len(data), sha256=digest,
                                  matches_archived_b6=True))
        (LOGS / 'artifact_manifest.json').write_text(json.dumps(dict(
            date='2026-10-07', scope='P1 baseline; no G1 implementation', artifacts=artifacts,
            comparison='Engineering/TechReports/engine/06, sections 5.3 and 5.4',
            board_log='Main/sw/apps/test/engineTest/logs/g1_research_p1_board.log'),
            indent=2) + '\n', encoding='utf-8')
        print('board artifacts match archived B6 identities (2/2); see artifact_manifest.json')
        return
    if '--prepare-syn' in sys.argv or '--prepare-tex-syn' in sys.argv:
        import xml.etree.ElementTree as ET
        ns = '{http://www.efinixinc.com/enf_proj}'
        template = MAIN / 'sim/engine/pix/syn'
        tree = ET.parse(template / 'engine_pix_syn.xml')
        root = tree.getroot()
        root.set('name', 'tex_ram_research')
        root.set('description', 'G1 four-bank RAM research only')
        design = root.find(ns + 'design_info')
        design.find(ns + 'top_module').set('name', 'syn_tex_ram')
        for f in list(design.findall(ns + 'design_file')):
            design.remove(f)
        # Efinity 的工程 schema 要求 design_file 在 top_vhdl_arch 之前。
        design.insert(1, ET.Element(ns + 'design_file', name='../probe_tex_ram.v',
                                    version='default', library='default'))
        root.find(ns + 'constraint_info').find(ns + 'sdc_file').set('name', 'tex_ram_research.sdc')
        for p in root.iter(ns + 'param'):
            if p.get('name') == 'work_dir':
                p.set('value', p.get('value').replace('engine_pix', 'tex_ram'))
        ET.register_namespace('efx', 'http://www.efinixinc.com/enf_proj')
        tree.write(BUILD / 'tex_ram_research.xml', encoding='utf-8', xml_declaration=False)
        if '--prepare-tex-syn' in sys.argv:
            root.set('name', 'engine_tex_syn')
            root.set('description', 'G1 engine_tex standalone')
            design.find(ns + 'top_module').set('name','syn_engine_tex')
            design.find(ns + 'design_file').set('name','../syn_engine_tex.v')
            design.insert(2, ET.Element(ns+'design_file',name='../../../../rtl/engine/engine_tex.v',version='default',library='default'))
            root.find(ns+'constraint_info').find(ns+'sdc_file').set('name','engine_tex_syn.sdc')
            tree.write(BUILD/'engine_tex_syn.xml',encoding='utf-8',xml_declaration=False)
        peri = (template / 'engine_pix_syn.peri.xml').read_text(encoding='utf-8')
        (BUILD / 'tex_ram_research.peri.xml').write_text(peri.replace('engine_pix_syn', 'tex_ram_research'), encoding='utf-8')
        (BUILD / 'tex_ram_research.sdc').write_text('create_clock -period 10.000 -name clk [get_ports clk]\n', encoding='utf-8')
        if '--prepare-tex-syn' in sys.argv:
            (BUILD/'engine_tex_syn.peri.xml').write_text(peri.replace('engine_pix_syn','engine_tex_syn'),encoding='utf-8')
            (BUILD/'engine_tex_syn.sdc').write_text('create_clock -period 10.000 -name clk [get_ports clk]\n',encoding='utf-8')
        print('prepared tex/build/tex_ram_research.xml (isolated research project)')
        return
    # 穷举整个 APB Slave0；地址译码采用高位常量，避免改宽 engine_regs。
    old_eng = [a for a in range(65536) if (a >> 12) == 0]
    new_eng = [a for a in range(65536) if (a >> 12) == 0 or (a >> 14) == 1]
    tex = [a for a in range(65536) if (a >> 14) == 1]
    assert len(old_eng) == 4096 and len(new_eng) == 20480
    assert tex == list(range(0x4000, 0x8000))
    assert not any(a >= 0xe000 for a in new_eng)
    facts = dict(apb_addresses_checked=65536, register_bytes=len(old_eng),
                 tex_bytes=len(tex), aligned_tex_words=sum(a % 4 == 0 for a in tex))
    manifest = MAIN / 'assets/manifest.txt'
    hot = []
    offset = 0
    for line in manifest.read_text(encoding='utf-8').splitlines():
        fields = line.split('\t')
        if len(fields) != 9 or fields[2] not in ('BUL16', 'BUL8', 'SPARK'):
            continue
        w, h = map(int, fields[5].split('x'))
        stride = 1 << max(2, (w * 4 - 1).bit_length())
        size = stride * h
        hot.append(dict(name=fields[2], width=w, height=h, stride=stride,
                        offset=offset, bytes=size))
        offset += size
    assert offset == 10240 and len(hot) == 3
    facts.update(hot_layout=hot, hot_bytes=offset, free_bytes=16384-offset)
    (LOGS / 'definition_checks.json').write_text(json.dumps(facts, indent=2)+'\n', encoding='utf-8')
    run('ram_compile', ['iverilog', '-g2012', '-s', 'tb_probe_tex_ram', '-o',
        BUILD / 'ram.vvp', SIM / 'probe_tex_ram.v', SIM / 'tb_probe_tex_ram.v'])
    ram_out = run('ram_sim', ['vvp', '-n', BUILD / 'ram.vvp'])
    if 'RAM RESULT: PASS' not in ram_out:
        raise RuntimeError('RAM functional experiment failed')
    (LOGS / 'ram_sim.log').write_text(ram_out, encoding='utf-8')
    lib = MAIN / 'sw/lib'
    export = BUILD / 'pix_cases_dump'
    run('export_compile', ['gcc', '-O2', '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-I' + str(lib), '-o', export, MAIN / 'tools/check/pix_cases_dump.c',
        *[lib / p for p in ['gfx/list.c', 'gfx/check.c', 'gfx/exec_sw.c',
                            'gfx/cases.c', 'core/format.c']]])
    run('export', [export, '--sprites', BUILD / 'sprites'])
    rtl = MAIN / 'rtl'
    common = [rtl / 'engine' / p for p in
              ['engine_geom.v', 'engine_pix.v', 'engine_blend.v', 'engine_mover.v']]
    common += [rtl / 'vendor/verilog-axi' / p for p in ['axi_dma_rd.v', 'axi_dma_wr.v']]
    common += [MAIN / 'sim/axi_probe/axi_ram_model.v']
    original = (MAIN / 'sim/engine/pix/tb_engine_path.v').read_text(encoding='utf-8')
    results = []
    for name, latency, inject in [('ddr', 0, 0), ('tex_l1', 1, 0),
                                  ('tex_l2', 2, 0), ('tex_bad_valid', 2, 1)]:
        tb = original
        if latency:
            # 固定 fixture 全部 BLIT 源走探针；目的读写仍用真实 mover/DMA/校准 DDR。
            # 此处以 rd_buf 的源/目的位选路，只适用于研究；正式实现必须传 space 标记。
            extra = f'''
    wire mover_rd_ready, tex_ready, tex_busy;
    wire [127:0] mover_rs_data, tex_data;
    wire [15:0] mover_rs_keep;
    wire mover_rs_last, mover_rs_valid, tex_last, tex_valid;
    wire [1:0] mover_rs_buf, tex_buf;
    wire mover_stat_valid, mover_stat_err, tex_stat_valid;
    wire [3:0] mover_stat_cidx, tex_stat_cidx;
    wire [1:0] mover_stat_buf;
    wire pick_tex = !rd_buf[1];
    assign rd_ready = pick_tex ? (tex_ready && !u_mover.rd_busy)
                              : (mover_rd_ready && !tex_busy);
    assign rs_tdata = tex_busy ? tex_data : mover_rs_data;
    assign rs_tkeep = tex_busy ? 16'hffff : mover_rs_keep;
    assign rs_tlast = tex_busy ? tex_last : mover_rs_last;
    assign rs_tvalid = tex_busy ? tex_valid : mover_rs_valid;
    assign rs_buf = tex_busy ? tex_buf : mover_rs_buf;
    assign rd_stat_valid = tex_stat_valid || mover_stat_valid;
    assign rd_stat_err = mover_stat_err && mover_stat_valid;
    assign rd_stat_cidx = tex_stat_valid ? tex_stat_cidx : mover_stat_cidx;
    assign rd_stat_buf = tex_stat_valid ? tex_buf : mover_stat_buf;
    probe_tex_stream #(.LATENCY({latency}), .INJECT({inject})) u_tex(
        .clk(clk), .rst(rst), .addr(rd_addr), .beats(rd_beats),
        .buf_id(rd_buf), .cidx(rd_cidx),
        .valid(rd_valid && pick_tex && !u_mover.rd_busy), .ready(tex_ready),
        .busy(tex_busy), .data(tex_data), .last(tex_last), .stream_valid(tex_valid),
        .stream_buf(tex_buf), .status_valid(tex_stat_valid), .status_cidx(tex_stat_cidx));
'''
            tb = replace_once(tb, '    engine_mover u_mover(', extra + '\n    engine_mover u_mover(')
            a, rest = tb.split('    engine_mover u_mover(', 1)
            mover, b = rest.split('`ifdef TIMED', 1)
            mover = replace_once(mover, '.rd_valid(rd_valid),.rd_ready(rd_ready)',
                '.rd_valid(rd_valid && !pick_tex && !tex_busy),.rd_ready(mover_rd_ready)')
            for old, new in [('rs_tdata', 'mover_rs_data'), ('rs_tkeep', 'mover_rs_keep'),
                             ('rs_tlast', 'mover_rs_last'), ('rs_tvalid', 'mover_rs_valid'),
                             ('rs_buf', 'mover_rs_buf'), ('rd_stat_valid', 'mover_stat_valid'),
                             ('rd_stat_err', 'mover_stat_err'), ('rd_stat_cidx', 'mover_stat_cidx'),
                             ('rd_stat_buf', 'mover_stat_buf')]:
                mover = mover.replace(f'({old})', f'({new})')
            tb = a + '    engine_mover u_mover(' + mover + '`ifdef TIMED' + b
            tb = replace_once(tb, '        #1 $readmemh({`CASE,".init.hex"},u_ram.mem);',
                '        #1 $readmemh({`CASE,".init.hex"},u_ram.mem);\n'
                '        $readmemh({`CASE,".init.hex"},u_tex.mem);')
        tb = replace_once(tb, '        $display("PERF cycles=%0d commands=%0d",cycle-t_start,done_count);',
            '        $display("PERF cycles=%0d commands=%0d",cycle-t_start,done_count);\n'
            '        $display("BUS source_ar=%0d dst_ar=%0d", source_ar, dst_ar);')
        tb = replace_once(tb, '    integer cycle=0;',
            '    integer source_ar=0, dst_ar=0;\n'
            '    always @(posedge clk) if (arvalid && arready) begin\n'
            '        if (araddr >= 32\'h30000) source_ar=source_ar+1;\n'
            '        else dst_ar=dst_ar+1;\n'
            '    end\n    integer cycle=0;')
        fixture = BUILD / (name + '_tb.v')
        fixture.write_text(tb, encoding='utf-8')
        exe = BUILD / (name + '.vvp')
        run(name + '_compile', ['iverilog', '-g2012', '-s', 'tb_engine_path', '-DTIMED',
            '-DCASE="build/sprites"', '-o', exe, fixture,
            *common, SIM / 'probe_tex_stream.v'])
        out = run(name, ['vvp', '-n', exe])
        passed = 'RESULT: PASS' in out and 'RESULT: FAIL' not in out
        if passed != (inject == 0):
            raise RuntimeError(f'{name}: unexpected outcome')
        cycles, commands = map(int, re.search(r'PERF cycles=(\d+) commands=(\d+)', out).groups())
        src_ar, dst_ar = map(int, re.search(r'BUS source_ar=(\d+) dst_ar=(\d+)', out).groups())
        mem_bad = int(re.search(r'mem_bad=(\d+)', out).group(1))
        if inject == 0 and (mem_bad or src_ar != (256 if latency == 0 else 0) or dst_ar != 256):
            raise RuntimeError(f'{name}: data or transaction count mismatch')
        if inject and mem_bad == 0:
            raise RuntimeError('bad_valid: must fail on actual memory comparison')
        result = dict(name=name, latency=latency, inject=inject, cycles=cycles,
                      commands=commands, cycles_per_blit=cycles / commands,
                      source_ar=src_ar, dst_ar=dst_ar, mem_bad=mem_bad,
                      expected_failure=bool(inject))
        results.append(result)
        (LOGS / (name + '.log')).write_text(out, encoding='utf-8')
        print(json.dumps(result), flush=True)
    (LOGS / 'research_results.json').write_text(json.dumps(results, indent=2) + '\n', encoding='utf-8')
    print('G1 RESEARCH OK: 3 normal cases + valid/data misalignment mutation')


if __name__ == '__main__':
    main()
