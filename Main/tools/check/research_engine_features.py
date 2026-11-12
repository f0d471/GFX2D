#!/usr/bin/env python3
"""引擎配置前期研究：穷举能力、编译守卫、混合核裁剪与冻结位流板测。"""
import argparse
import itertools
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET

from check_run import ROOT, IncompleteCheck, command, session, write_json

NAMES = ('TEX', 'BAND', 'INST', 'PB', 'EQ_EXT', 'KEY', 'ALPHA')
MACROS = ('GFX_HAS_TEX', 'GFX_HAS_BAND', 'GFX_HAS_INST', 'GFX_HAS_PB',
          'GFX_EQ_EXT', 'GFX_KEY', 'GFX_ALPHA')
PROFILES = {'A0': (0, 0, 0, 0), 'A1': (1, 0, 1, 0),
            'A2': (0, 1, 1, 0), 'A3': (1, 1, 0, 0),
            'A4': (1, 1, 1, 0), 'A5': (1, 1, 1, 1)}
BLENDS = {'full': (1, 1), 'no_alpha': (0, 1),
          'no_ext': (1, 0), 'copy_add': (0, 0)}


def valid(bits, delivery=False):
    tex, band, inst, pb, ext, key, alpha = bits
    return (not pb or band and inst) and (not delivery or key and alpha)


def caps(bits):
    tex, band, inst, pb, ext, key, alpha = bits
    return (4 << 8) | key | (alpha << 1) | (alpha << 2) | (ext << 3) | \
        (band << 5) | (tex << 6) | (inst << 12) | (pb << 13)


def guard(bits, delivery):
    # 缺失模块是编译期后备；正式构建仍须在生成输入前拒绝非法清单。
    text = ''.join('`define ' + macro + '\n' for macro, on in zip(MACROS, bits) if on)
    if delivery:
        text += '`define GFX_DELIVERY\n'
    text += 'module feature_guard;\n'
    text += '''`ifdef GFX_HAS_PB
`ifndef GFX_HAS_BAND
GFX_CONFIG_ERROR_PB_REQUIRES_BAND bad_band();
`endif
`ifndef GFX_HAS_INST
GFX_CONFIG_ERROR_PB_REQUIRES_INST bad_inst();
`endif
`endif
`ifdef GFX_DELIVERY
`ifndef GFX_ALPHA
GFX_CONFIG_ERROR_DELIVERY_REQUIRES_ALPHA bad_alpha();
`endif
`ifndef GFX_KEY
GFX_CONFIG_ERROR_DELIVERY_REQUIRES_KEY bad_key();
`endif
`endif
endmodule
'''
    return text


def attempt(args, path, cwd=None):
    result = subprocess.run(list(map(str, args)), cwd=cwd or ROOT,
                            capture_output=True, text=True, errors='replace', timeout=120)
    path.write_text(result.stdout + result.stderr, encoding='utf-8')
    return result.returncode


def models(current):
    if os.name == 'nt':
        raise IncompleteCheck('--models 使用 WSL 的 Icarus、Verilator 与 Yosys')
    logs, build = current.logs('features'), current.build('features')
    for tool in ('iverilog', 'verilator', 'yosys'):
        if not shutil.which(tool):
            raise IncompleteCheck('缺少 ' + tool)
    versions = {}
    for tool, arg in [('iverilog', '-V'), ('verilator', '--version'), ('yosys', '-V')]:
        p = subprocess.run([tool, arg], capture_output=True, text=True, check=True)
        versions[tool] = p.stdout.splitlines()[0]
    legal = [b for b in itertools.product((0, 1), repeat=4) if valid((*b, 1, 1, 1))]
    assert len(legal) == 10
    profile_caps = {name: f'0x{caps((*bits, 1, 1, 1)):08X}' for name, bits in PROFILES.items()}
    assert list(profile_caps.values()) == ['0x0000040F', '0x0000144F', '0x0000142F',
                                           '0x0000046F', '0x0000146F', '0x0000346F']
    outcomes = []
    for delivery in (False, True):
        for bits in itertools.product((0, 1), repeat=7):
            label = ('delivery_' if delivery else 'research_') + ''.join(map(str, bits))
            src = build / (label + '.v')
            src.write_text(guard(bits, delivery), encoding='utf-8')
            rc = attempt(['iverilog', '-g2005', '-s', 'feature_guard', '-o', build / 'guard.vvp', src],
                         logs / (label + '.log'))
            assert (rc == 0) == bool(valid(bits, delivery)), label
            outcomes.append({'configuration': label, 'accepted': rc == 0})
    # 后备守卫在另外两种工具上必须也能挡住缺失依赖/交付能力。
    guard_cases = [(1, 1, 1, 1, 1, 1, 1), (1, 0, 1, 1, 1, 1, 1),
                   (1, 1, 0, 1, 1, 1, 1), (1, 1, 1, 1, 1, 0, 1),
                   (1, 1, 1, 1, 1, 1, 0)]
    for i, bits in enumerate(guard_cases):
        src = build / f'portable_guard_{i}.v'
        src.write_text(guard(bits, True), encoding='utf-8')
        for tool, args in [('verilator', ['verilator', '--lint-only', '--top-module', 'feature_guard', src]),
                           ('yosys', ['yosys', '-Q', '-T', '-p', f'read_verilog {src}; hierarchy -check -top feature_guard'])]:
            rc = attempt(args, logs / f'{tool}_guard_{i}.log')
            assert (rc == 0) == (i == 0), (tool, i)
    directive = build / 'error_directive.v'
    directive.write_text('`error "invalid configuration"\nmodule feature_guard; endmodule\n', encoding='utf-8')
    error_results = {}
    for tool, args in [('iverilog', ['iverilog', '-g2005', '-E', '-o', build / 'preprocessed.v', directive]),
                       ('verilator', ['verilator', '-E', directive]),
                       ('yosys', ['yosys', '-Q', '-T', '-p', f'read_verilog {directive}'])]:
        error_results[tool] = attempt(args, logs / (tool + '_error_directive.log'))
    zero = build / 'zero_define.v'
    zero.write_text('`define GFX_HAS_PB 0\nmodule zero_define;\n`ifdef GFX_HAS_PB\ninitial $display("DEFINED_IS_ON");\n`endif\nendmodule\n', encoding='utf-8')
    command(['iverilog', '-g2005', '-o', build / 'zero.vvp', zero], logs / 'zero_compile.log')
    assert 'DEFINED_IS_ON' in command(['vvp', build / 'zero.vvp'], logs / 'zero_run.log')
    result = {'status': 'PASS', 'scope': 'research configuration and guard probes; no production feature implementation',
              'versions': versions, 'legal_structural_configurations': [dict(zip(NAMES[:4], b)) for b in legal],
              'research_valid': sum(o['accepted'] for o in outcomes[:128]),
              'delivery_valid': sum(o['accepted'] for o in outcomes[128:]),
              'guard_configurations': outcomes, 'portable_guard_cases_per_tool': 5,
              'representative_caps': profile_caps, 'error_directive_exit_codes': error_results,
              'define_zero_still_enabled': True}
    write_json(logs / 'models.json', result)
    print('FEATURE MODELS PASS: structural=10 research=80 delivery=20 guards=256', flush=True)
    return {'log': (logs / 'models.json').relative_to(ROOT).as_posix()}


def blend_source(alpha, ext):
    source = (ROOT / 'Main/rtl/engine/core/engine_blend.v').read_text(encoding='utf-8')
    start, end = source.index('        case (eq)'), source.index('        endcase')
    body = source[start:end]
    chunks = re.split(r'(?=^            (?:3\'d[0-4]|default): begin)', body, flags=re.M)[1:]
    assert len(chunks) == 6
    active = (0, 1, 2, 3, 4, 5) if alpha and ext else ((0, 1, 2, 5) if alpha else ((0, 2, 3, 4) if ext else (0, 2)))
    selected = ''.join(chunks[i].replace('default: begin', "3'd5: begin") for i in active)
    default = "            default: begin sel_base=0; sel_m1=0; sel_m2=0; end\n"
    return source[:start] + '        case (eq)\n' + selected + default + source[end:]


def prepare(current):
    ns = 'http://www.efinixinc.com/enf_proj'
    ET.register_namespace('efx', ns)
    ET.register_namespace('xsi', 'http://www.w3.org/2001/XMLSchema-instance')
    projects = []
    for name, (alpha, ext) in BLENDS.items():
        folder = current.build('features') / name
        folder.mkdir(exist_ok=True)
        (folder / 'engine_blend.v').write_text(blend_source(alpha, ext), encoding='utf-8')
        (folder / 'syn_feature_blend.v').write_text('''module syn_feature_blend(input wire clk, input wire en,
input wire [34:0] data, output wire [7:0] out);
reg [34:0] r;
always @(posedge clk) if(en) r<=data;
engine_blend u_blend(.clk(clk),.en(en),.src(r[7:0]),.dst(r[15:8]),
.a(r[23:16]),.c(r[31:24]),.eq(r[34:32]),.out(out));
endmodule
''', encoding='utf-8')
        tree = ET.parse(ROOT / 'Main/sim/engine/frontend/syn_bin/engine_bin_syn.xml')
        root = tree.getroot()
        root.set('name', 'feature_blend')
        root.set('description', 'isolated variable-input blend feature research')
        design = root.find(f'{{{ns}}}design_info')
        design.find(f'{{{ns}}}top_module').set('name', 'syn_feature_blend')
        for node in list(design.findall(f'{{{ns}}}design_file')):
            design.remove(node)
        for index, filename in enumerate(('engine_blend.v', 'syn_feature_blend.v'), 1):
            design.insert(index, ET.Element(f'{{{ns}}}design_file', name=filename, version='default', library='default'))
        root.find(f'{{{ns}}}constraint_info').find(f'{{{ns}}}sdc_file').set('name', 'feature_blend.sdc')
        (folder / 'feature_blend.sdc').write_text('create_clock -name clk -period 10 [get_ports clk]\n', encoding='utf-8')
        # 本目录里源码是平铺的副本，模板里指向 rtl/engine 的 include 参数在这里不成立，去掉。
        syn = root.find(f'{{{ns}}}synthesis')
        for node in list(syn.findall(f'{{{ns}}}param')):
            if node.get('name') == 'include':
                syn.remove(node)
        tree.write(folder / 'feature_blend.xml', encoding='utf-8', xml_declaration=True)
        projects.append(folder.relative_to(ROOT).as_posix())
    write_json(current.logs('features') / 'projects.json', {'projects': projects, 'scope': 'standalone variable-input mapping; not whole-SoC area or timing'})
    return projects


def blend_models(current):
    # 隔离裁剪真实混合核；有使能停顿的三拍队列与独立整数方程逐项比较。
    logs = current.logs('features')
    results = {}
    for name, (alpha, ext) in BLENDS.items():
        folder = current.build('features') / name
        equations = [0, 2] + ([1, 5] if alpha else []) + ([3, 4] if ext else [])
        tb = '''module tb;
reg clk=0; always #5 clk=~clk;
reg en=1; reg [7:0] s=0,d=0,a=0,c=0; reg [2:0] eq=0;
wire [7:0] out;
engine_blend dut(.clk(clk),.en(en),.src(s),.dst(d),.a(a),.c(c),.eq(eq),.out(out));
integer seed=12345,k,t,q0=0,q1=0,q2=0,n=0,checks=0; integer expected;
function integer reference;
input integer e,sv,dv,av,cv;
integer x;
begin
case(e)
0:x=sv*255; 1:x=sv*255+dv*(255-av); 2:x=(sv+dv)*255;
3:x=sv*cv+dv*(255-cv); 4:x=sv*dv; 5:x=sv*av+dv*(255-av);
default:x=0;
endcase
x=(x+127)/255; reference=x>255?255:x;
end endfunction
initial begin
for(k=0;k<30000;k=k+1) begin
@(negedge clk);
s=$random(seed);d=$random(seed);a=$random(seed);c=$random(seed);
en=(k%7)!=0;
case(k%EQ_COUNT)
EQ_CASES
endcase
expected=reference(eq,s,d,a,c);
@(posedge clk); #1;
if(en)begin q2=q1;q1=q0;q0=expected;n=n+1;end
if(n>=3)begin checks=checks+1;if(out!==q2)begin $display("FAIL k=%0d out=%0d expected=%0d",k,out,q2);$fatal;end end
end
$display("BLEND_FEATURE PASS checks=%0d",checks);$finish;
end endmodule
'''.replace('EQ_COUNT', str(len(equations))).replace('EQ_CASES', '\n'.join(f'{i}:eq={e};' for i, e in enumerate(equations)))
        (folder / 'tb.v').write_text(tb, encoding='utf-8')
        command(['iverilog', '-g2012', '-s', 'tb', '-o', folder / 'tb.vvp', folder / 'tb.v', folder / 'engine_blend.v'], logs / (name + '_compile.log'))
        body = command(['vvp', folder / 'tb.vvp'], logs / (name + '_pixels.log'))
        assert 'BLEND_FEATURE PASS' in body and 'FAIL' not in body
        results[name] = {'equations': equations, 'checks': int(re.search(r'checks=(\d+)', body).group(1))}
    write_json(logs / 'blend_models.json', {'status': 'PASS', 'variants': results, 'scope': 'isolated research RTL; enabled equations and pipeline stalls'})
    return {'log': (logs / 'blend_models.json').relative_to(ROOT).as_posix()}


def synthesis(current):
    if os.name != 'nt':
        raise IncompleteCheck('--synthesis 使用 Windows Efinity Python')
    install = Path(os.environ.get('EFINITY_HOME') or Path(sys.executable).parents[2])
    env = dict(os.environ)
    for key in ('EFINITY_HOME', 'PYTHONHOME', 'EFXPT_HOME', 'EFXPGM_HOME', 'EFXDBG_HOME', 'EFXIPM_HOME', 'EFXSVF_HOME', 'EFXSERDESDBG_HOME', 'EFINITY_USER_DIR_INI'):
        env.pop(key, None)
    results = {}
    for name in BLENDS:
        folder = current.build('features') / name
        if not (folder / 'feature_blend.xml').is_file():
            raise IncompleteCheck('先 --prepare-synthesis')
        log = current.logs('features') / (name + '_map.log')
        command([install / 'bin/efx_run.bat', 'feature_blend.xml', '-f', 'map'], log, cwd=folder, env=env)
        files = sorted((folder / 'outflow').glob('*'))
        # 后映射网表按原语实例数计，不从独立壳推算整机增量。
        netlists = [p for p in files if p.name.endswith('.map.v')]
        if len(netlists) != 1:
            raise RuntimeError('缺少唯一 map.v: ' + str(folder))
        net = netlists[0].read_text(encoding='utf-8', errors='replace')
        counts = {kind: len(re.findall(r'^\s*' + kind + r'\s', net, re.M))
                  for kind in ('EFX_DSP48', 'EFX_RAM10')}
        results[name] = {'primitive_instances': counts, 'scope': 'standalone mapping; not pnr or SoC resource delta'}
        current.artifact(netlists[0])
    assert [results[n]['primitive_instances']['EFX_DSP48'] for n in BLENDS] == [1, 1, 1, 0], results
    write_json(current.logs('features') / 'synthesis.json', {'status': 'PASS', 'variants': results})
    print('FEATURE SYNTHESIS PASS DSP(full/no_alpha/no_ext/copy_add)=1/1/1/0', flush=True)
    return {'log': (current.logs('features') / 'synthesis.json').relative_to(ROOT).as_posix()}


def board(current, bit):
    if os.name != 'nt':
        raise IncompleteCheck('--board 使用 Windows Efinity Python')
    from check_board_smoke import uart_port, validate_engine
    logs = current.logs('features')
    binary = ROOT / 'Main/sw/apps/test/engineTest/build_feature_research/engineTest-O2.bin'
    build = 'prefix="$(find "$HOME/opt" -path "*/bin/riscv-none-elf-gcc" -print -quit)"; test -n "$prefix" || exit 3; make -C Main/sw/apps/test/engineTest -j8 OBJDIR=build_feature_research RISCV_BIN="${prefix%gcc}"'
    command(['wsl.exe', '-e', 'bash', '-c', build], logs / 'board_build.log')
    bit = (ROOT / bit).resolve()
    current.artifact(bit)
    current.artifact(binary)
    port = uart_port()
    runner = ROOT / 'Main/tools/board/board_run.py'
    command([sys.executable, runner, '--check-only', '--bit', bit], logs / 'board_debug.log')
    command([sys.executable, runner, '--no-program', '--bin', binary, '--port', port,
             '--stop-line', 'done', '--seconds', '180', '--log', logs / 'board_engine.log'], logs / 'board_control.log', timeout=240)
    result = validate_engine((logs / 'board_engine.log').read_text(encoding='utf-8-sig', errors='replace'))
    control = (logs / 'board_control.log').read_text(encoding='utf-8', errors='replace')
    assert 'Target successfully examined' in control
    assert str(binary.stat().st_size) + ' bytes written at address' in control
    # 读取真实能力位；全功能被冻结为本轮明确基线，不将研究壳当成部署对象。
    raw = command([sys.executable, runner, '--no-program', '--cmd', 'mdw 0xf8100000 2'], logs / 'board_caps.log')
    assert re.search(r'0xf8100000:\s+47465801\s+0000346f', raw, re.I), raw[-1000:]
    result.update({'port': port, 'caps': '0x0000346F', 'bit': bit.relative_to(ROOT).as_posix(),
                   'scope': 'frozen full-feature baseline only; no cut-down bitstream, no Flash write, no Nmax claim'})
    write_json(logs / 'board.json', result)
    print('FEATURE BOARD PASS: CAPS=0x0000346F DDR/TEX golden=22', flush=True)
    return {'log': (logs / 'board.json').relative_to(ROOT).as_posix()}


def main():
    current = session()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--models', action='store_true')
    parser.add_argument('--prepare-synthesis', action='store_true')
    parser.add_argument('--blend-models', action='store_true')
    parser.add_argument('--synthesis', action='store_true')
    parser.add_argument('--board', action='store_true')
    parser.add_argument('--bit', type=Path)
    parser.add_argument('--collect', action='store_true')
    args = parser.parse_args()
    if args.models:
        current.perform('features.models', lambda: models(current))
    if args.prepare_synthesis:
        prepare(current)
    if args.blend_models:
        current.perform('features.blend-models', lambda: blend_models(current))
    if args.synthesis:
        current.perform('features.synthesis', lambda: synthesis(current))
    if args.board:
        if not args.bit:
            parser.error('--board 必须显式给 --bit，不能暗用生产默认位流')
        current.perform('features.board', lambda: board(current, args.bit), reusable=False)
    if args.collect:
        current.require(['features.models', 'features.blend-models', 'features.synthesis', 'features.board'])
        write_json(current.path / 'result.json', {'status': 'PASS', 'profile': current.profile,
                   'scope': 'engine optional-feature preliminary research; production implementation not started',
                   'checks': current.read()['checks']})
        print('FEATURE RESEARCH PASS ' + current.path.relative_to(ROOT).as_posix(), flush=True)


if __name__ == '__main__':
    try:
        main()
    except IncompleteCheck as error:
        print('INCOMPLETE ' + str(error), file=sys.stderr)
        sys.exit(3)
