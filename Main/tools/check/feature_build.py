#!/usr/bin/env python3
"""引擎特性构型的构建入口：从特性清单生成该构型全部构建输入。

特性位的定义与依赖在 feature_config.py；寄存器偏移/位号的单一真源仍是 regs_gen.py。
本脚本只做“按构型把两者组合成产物”，不复制任何真源：

    include/engine_feature.vh     宏的唯一定义处（存在式宏，关闭即不定义）
    include/engine_regs_offsets.vh 按构型生成 CAPS 复位值（调 regs_gen 的生成函数）
    core/engine_config_guard.v    依赖守卫，一个模块，由 engine_top 例化一次；与构型无关
    gfx_regs.h                    同上，软件侧
    gfx_feature.h                 软件侧的“编译时预期能力”，供按构型检查
    gfx2d_ti60.xml                Efinity 工程：源列表按构型排除禁用模块
    sim-include/                  仿真侧 include 入口（只放特性宏）
    config.json                   构型名、CAPS、开关、源列表（供检查脚本消费）

用法（在仓库根）：
    python3 Main/tools/check/feature_build.py --list
    python3 Main/tools/check/feature_build.py --config B5 --out-dir <目录>
    python3 Main/tools/check/feature_build.py --config 11110-111-d --out-dir <目录>
"""
import argparse
import json
import shutil
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import feature_config as fc
import regs_gen

ROOT = Path(__file__).resolve().parents[3]
MAIN = ROOT / 'Main'
TEMPLATE_XML = MAIN / 'fpga/gfx2d_ti60/gfx2d_ti60.xml'
NS = 'http://www.efinixinc.com/enf_proj'

# 特性开关控制的源文件（工程模板里的相对路径用 ../../rtl/ 前缀）。
FEATURE_SOURCES = {
    'RAST': ('engine/rast/engine_r3.v', 'engine/rast/engine_rlst.v', 'engine/rast/engine_rast.v'),
    'TEX': ('engine/buffer/engine_tex.v',),
    'BAND_HW': ('engine/buffer/engine_band.v',),
    'INST': ('engine/frontend/engine_inst.v',),
    'PB': ('engine/frontend/engine_pb.v', 'engine/frontend/engine_pb_dma.v', 'engine/frontend/engine_bin.v',
           'engine/frontend/engine_bounds.v', 'engine/frontend/engine_packet_check.v'),
}

FIXED_RTL = (
    'top/gfx2d_top.v',
    'bus/apb_probe.v', 'bus/axi_probe_master.v', 'bus/axi_mux_guard.v', 'bus/scanout_stub.v',
    'display/display_top.v', 'display/display_fetch.v', 'display/display_fifo.v', 'display/display_out.v',
    'engine/core/engine_blend.v', 'engine/core/engine_pix.v', 'engine/core/engine_geom.v', 'engine/core/engine_mover.v',
    'engine/core/engine_regs.v', 'engine/core/engine_cmd.v', 'engine/engine_top.v',
    'engine/core/engine_config_guard.v',
    'vendor/verilog-axi/axi_dma_rd.v', 'vendor/verilog-axi/axi_dma_wr.v',
    'vendor/axi_mux/Axi_Mux.v',
)
VENDOR_DDR = (
    'vendor/ddr3_controller/ddr3_parameter.vh', 'vendor/ddr3_controller/ddr3_top.v',
    'vendor/ddr3_controller/local_bus_ctl/local_bus_ctl.v',
    'vendor/ddr3_controller/memory_bus_ctl/memory_bus_ctl.v',
    'vendor/ddr3_controller/axi4_bus_ctl/axi4_bus_ctl.v',
    'vendor/ddr3_controller/phy_ctl/ddr_mpr_rdlvl.v',
    'vendor/ddr3_controller/phy_ctl/ddr_phy_if.v',
    'vendor/ddr3_controller/phy_ctl/phy_ctl.v',
    'vendor/ddr3_controller/phy_ctl/ddr_phy_wrcal.v',
    'vendor/ddr3_controller/phy_ctl/ddr_phy_rdlvl_dqs.v',
    'vendor/ddr3_controller/phy_ctl/ddr_calibration_ini.v',
    'vendor/ddr3_controller/phy_ctl/ddr_phy_wrlvl.v',
    'vendor/ddr3_controller/common/efx_fifo_v2.3/efx_fifo_wrapper.v',
    'vendor/ddr3_controller/lb2_axi_clk.v',
)
HDMI_FILES = ('hdmi/hdmi_tx.v', 'hdmi/color_bar_rgb.v', 'hdmi/dvi_encoder.v', 'hdmi/encode.v')


def template_order():
    """模板里的源文件顺序（相对 rtl/）。顺序会影响厂商映射，必须照抄不改。"""
    design = ET.parse(TEMPLATE_XML).getroot().find(f'{{{NS}}}design_info')
    order = []
    for node in design.findall(f'{{{NS}}}design_file'):
        name = node.get('name')
        prefix = '../../rtl/'
        if not name.startswith(prefix):
            raise SystemExit('模板源文件路径前缀不是 ' + prefix + ': ' + name)
        order.append(name[len(prefix):])
    return order


def enabled_rtl(config):
    """按模板顺序给出本构型的源文件；模板没有列出的文件接在后面。"""
    order = template_order()
    listed = set(order)
    extra = (set(FIXED_RTL) | set(VENDOR_DDR) | set(HDMI_FILES)
             | {s for srcs in FEATURE_SOURCES.values() for s in srcs}) - listed
    files = list(order) + sorted(extra)
    for flag, sources in FEATURE_SOURCES.items():
        if config[flag]:
            continue
        files = [name for name in files if name not in set(sources)]
    return files


def feature_header(name, config):
    lines = [
        '// 由 tools/check/feature_build.py 生成，是特性宏的唯一定义处，不要手改。',
        f'// 构型 {name}    CAPS 复位值 0x{fc.caps(config):08X}',
        '// 关闭的机制不定义对应宏。禁止用 -DGFX_HAS_x=0 表示关闭：',
        '// 存在式宏即使值为 0，`ifdef 仍然成立，实测会进入启用分支。',
        '`ifndef GFX_ENGINE_FEATURE_VH',
        '`define GFX_ENGINE_FEATURE_VH',
        '',
        '// ---- 本构型启用的机制 ----',
    ]
    macros = fc.enabled_macros(config)
    lines += [f'`define {m}' for m in macros] if macros else ['// （全部关闭）']
    lines += ['', '`endif', '']
    return '\n'.join(lines)


def guard_module(config):
    """依赖守卫：一个模块，依赖不满足时例化不存在的模块，让编译/综合硬失败。

    与构型无关：内容只由 feature_config.GUARDS 与 EXCLUDES 决定，所以不按构型生成，
    只在依赖表变化时重写，由 engine_top 例化一次。不定义任何 GFX_CONFIG_ERROR_* 模块，
    因此违规组合在 iverilog、hierarchy -check 与厂商综合（blackbox-error=1）里都是硬失败，
    报错里带着违规边的名字。
    不用 `error：本机 Icarus 的预处理对 `error 只给警告、退出码仍为 0。
    每条边一个唯一的实例名，避免同一模块里多条守卫同时展开时重名。
    """
    lines = [
        '// 由 tools/check/feature_build.py 生成：依赖守卫，由 engine_top 例化一次。',
        '// 依赖不满足时例化一个不存在的模块，仿真、hierarchy -check 与厂商综合都会硬失败，',
        '// 报错里带违规边的名字。内容只随 feature_config.py 的依赖表变化，与构型无关。',
        '`timescale 1ns/1ps',
        '`default_nettype none',
        '`include "engine_feature.vh"',
        '',
        'module gfx_config_guard();',
        '',
    ]
    for owner, need, module in fc.GUARDS:
        macros = {name: macro for name, macro, _ in fc.STRUCTURE + fc.PIXEL + (fc.DELIVERY,)}
        owner_macro = macros[owner]
        need_macro = macros[need]
        instance = 'u_config_error_' + module.replace('GFX_CONFIG_ERROR_', '').lower()
        lines += [f'`ifdef {owner_macro}',
                  f'  `ifndef {need_macro}',
                  f'    {module} {instance}();',
                  '  `endif',
                  '`endif']
    macros = {n: m for n, m, _ in fc.STRUCTURE + fc.PIXEL + (fc.DELIVERY,)}
    for first, second, module in fc.EXCLUDES:
        lines += [f'`ifdef {macros[first]}', f'  `ifdef {macros[second]}',
                  f'    {module} u_config_error_{first.lower()}_{second.lower()}();',
                  '  `endif', '`endif']
    lines += ['', 'endmodule', '', '`default_nettype wire', '']
    return '\n'.join(lines)


def software_header(name, config):
    caps = fc.caps(config)
    lines = [
        '/* 由 tools/check/feature_build.py 生成：本构型的编译时预期能力。',
        ' * 用于把交付物与硬件构型对上；运行时探测仍以 CAPS 寄存器为准。',
        ' * 能力位表示这块硬件具备什么，不表示默认走哪条后端路径——',
        f' * 本构型 {name} 的默认后端由 renderer 的 GFX_FRONTEND_* 决定，与能力位无关。 */',
        '#ifndef GFX_FEATURE_H',
        '#define GFX_FEATURE_H',
        '',
        f'#define GFX_FEATURE_CONFIG   "{name}"',
        f'#define GFX_FEATURE_CAPS     0x{caps:08X}u',
    ]
    for struct, _, _ in fc.STRUCTURE:
        lines.append(f'#define GFX_FEATURE_{struct}   {1 if config[struct] else 0}')
    for pixel, _, _ in fc.PIXEL:
        lines.append(f'#define GFX_FEATURE_{pixel}   {1 if config[pixel] else 0}')
    lines += [
        f'#define GFX_FEATURE_DELIVERY   {1 if config["DELIVERY"] else 0}',
        '',
        '/* 编译时预期与运行时 CAPS 的一致性：预期为 1 的能力位必须真的为 1，',
        ' * 且设备不得报告本构型没有的能力位。 */',
        '#define GFX_FEATURE_CAPS_MATCH(caps) \\',
        '    ((((caps) & GFX_FEATURE_CAPS) == GFX_FEATURE_CAPS) && \\',
        '     (((caps) & ~(GFX_FEATURE_CAPS)) == 0u))',
        '',
        '#endif',
        '',
    ]
    return '\n'.join(lines)


def project_xml(config, xml_path, tree_root):
    """在构型自己的源树里生成工程 XML：保留命名空间与节点顺序，只换源文件列表。

    命名空间与节点顺序被厂商 map 依赖，首轮研究曾因丢失它们导致解析失败；
    这里在解析前注册 efx 前缀，否则 ElementTree 会改写成 ns0:，Interface Designer 拒绝。
    源树是 rtl/ 的完整副本，所以路径前缀与模板一致（../../rtl/ 指回树内的 rtl/）。
    """
    ET.register_namespace('efx', NS)
    ET.register_namespace('xsi', 'http://www.w3.org/2001/XMLSchema-instance')
    tree = ET.parse(TEMPLATE_XML)
    root = tree.getroot()
    design = root.find(f'{{{NS}}}design_info')
    for node in list(design.findall(f'{{{NS}}}design_file')):
        design.remove(node)
    top = design.find(f'{{{NS}}}top_module')
    index = list(design).index(top) + 1
    for offset, relative in enumerate(enabled_rtl(config)):
        design.insert(index + offset, ET.Element(
            f'{{{NS}}}design_file', name='../../rtl/' + relative,
            version='default', library='default'))
    mode = root.find(f'{{{NS}}}synthesis/{{{NS}}}param[@name="mode"]')
    if mode is None:
        raise ValueError('模板缺少 synthesis.mode')
    mode.set('value', fc.synthesis_mode(config))
    tree.write(xml_path, encoding='utf-8', xml_declaration=True)


def populate_tree(config, out):
    """把 rtl/ 复制成该构型自己的源树，并把生成头放进它该在的位置。

    生成的 engine_feature.vh 与 engine_regs_offsets.vh 写进 rtl/engine/include/，
    守卫模块 core/engine_config_guard.v 按当前依赖表重写进 rtl/engine/core/
    （`.v` 分布在 core/、buffer/、frontend/、rast/ 子目录），
    工程 XML 里的 include 参数指向 include/，RTL 才能找到 `include。
    SoC IP（ip/soc）、外设接口（peri.xml）与约束（sdc/）从现有工程原样复制：
    这三样与特性构型无关，重生成会覆盖 SDK，不能在本工作点里动。
    """
    shutil.copytree(MAIN / 'rtl', out / 'rtl', ignore=shutil.ignore_patterns('*.log', '__pycache__'))
    project = out / 'fpga' / 'gfx2d_ti60'
    project.mkdir(parents=True)
    source = TEMPLATE_XML.parent
    sdc = source / 'sdc' / 'gfx2d_ti60.sdc'
    if not sdc.is_file():
        raise SystemExit('缺少约束文件: ' + str(sdc))
    (project / 'sdc').mkdir()
    shutil.copyfile(sdc, project / 'sdc' / 'gfx2d_ti60.sdc')
    peri = source / 'gfx2d_ti60.peri.xml'
    if not peri.is_file():
        raise SystemExit('缺少外设接口文件: ' + str(peri))
    shutil.copyfile(peri, project / 'gfx2d_ti60.peri.xml')
    ip = source / 'ip'
    if not ip.is_dir():
        raise SystemExit('缺少 SoC IP 目录: ' + str(ip))
    shutil.copytree(ip, project / 'ip')
    engine = out / 'rtl' / 'engine'
    include = engine / 'include'
    include.mkdir(exist_ok=True)
    for name_ in ('engine_feature.vh', 'engine_regs_offsets.vh'):
        shutil.copyfile(out / name_, include / name_)
    # 守卫模块与构型无关，仍按当前依赖表重写一遍，避免仓库副本过期被带进树里
    (engine / 'core' / 'engine_config_guard.v').write_text(
        guard_module(config), encoding='utf-8', newline='\n')


# 主工程构型跟随配置真源；同步头、寄存器与综合模式后重建位流。
CANONICAL_CONFIG = fc.CANONICAL


def write_engine_headers(config, name, engine_dir):
    """把生成物按仓库布局写进 rtl/engine：声明头进 include/，守卫模块进 core/。"""
    engine_dir = Path(engine_dir)
    include = engine_dir / 'include'
    include.mkdir(parents=True, exist_ok=True)
    core = engine_dir / 'core'
    core.mkdir(parents=True, exist_ok=True)
    (include / 'engine_feature.vh').write_text(feature_header(name, config),
                                               encoding='utf-8', newline='\n')
    (include / 'engine_regs_offsets.vh').write_text(regs_gen.gen_vh(fc.caps(config)),
                                                    encoding='utf-8', newline='\n')
    (core / 'engine_config_guard.v').write_text(guard_module(config),
                                                encoding='utf-8', newline='\n')


def write_headers(config_name, out_dir):
    """单元套件使用隔离构型头，不改生产默认，也不复制整机 SDK。"""
    config = fc.parse_name(config_name)
    reasons = fc.check(config)
    if reasons:
        raise ValueError('INVALID '+config_name+': '+'；'.join(reasons))
    out = Path(out_dir).resolve()
    out.mkdir(parents=True, exist_ok=True)
    (out/'engine_feature.vh').write_text(feature_header(fc.name_of(config), config),
                                         encoding='utf-8', newline='\n')
    (out/'engine_regs_offsets.vh').write_text(regs_gen.gen_vh(fc.caps(config)), encoding='utf-8', newline='\n')
    return out


def sync_canonical():
    """把默认构型的特性头与综合模式同步到主工程。

    仓库内的 RTL、仿真与应用构建使用配置真源指定的默认构型：
    两份声明头写进 rtl/engine/include/，守卫模块写进 rtl/engine/core/，
    工程 XML 的 include 参数指向 include/，RTL 才能找到 `include；
    软件侧另给一份到 sw/bsp/include，使 engineTest 一类程序默认就能做
    「编译时预期能力 vs 运行时 CAPS」的配对判据。
    按构型构建时这四份文件被构型自己的副本覆盖。
    """
    config = fc.parse_name(CANONICAL_CONFIG)
    assert not fc.check(config), fc.check(config)
    name = fc.name_of(config)
    out = MAIN / 'rtl' / 'engine'
    write_engine_headers(config, name, out)
    sw = MAIN / 'sw' / 'bsp' / 'include'
    if sw.is_dir():
        (sw / 'gfx_feature.h').write_text(software_header(name, config),
                                          encoding='utf-8', newline='\n')
    ET.register_namespace('efx', NS)
    ET.register_namespace('xsi', 'http://www.w3.org/2001/XMLSchema-instance')
    project = ET.parse(TEMPLATE_XML)
    mode = project.find(f'{{{NS}}}synthesis/{{{NS}}}param[@name="mode"]')
    if mode is None:
        raise ValueError('主工程缺少 synthesis.mode')
    if mode.get('value') != fc.synthesis_mode(config):
        mode.set('value', fc.synthesis_mode(config))
        project.write(TEMPLATE_XML, encoding='utf-8', xml_declaration=True)
    return name


def check_canonical():
    """校验 rtl/engine/ 与 sw/bsp/include 里的特性头与默认构型一致（挂提交前检查）。"""
    config = fc.parse_name(CANONICAL_CONFIG)
    name = fc.name_of(config)
    out = MAIN / 'rtl' / 'engine'
    expected = {
        'include/engine_feature.vh': feature_header(name, config),
        'include/engine_regs_offsets.vh': regs_gen.gen_vh(fc.caps(config)),
        'core/engine_config_guard.v': guard_module(config),
    }
    bad = [f for f, text in expected.items()
           if (out / f).read_text(encoding='utf-8') != text]
    mode = ET.parse(TEMPLATE_XML).find(f'{{{NS}}}synthesis/{{{NS}}}param[@name="mode"]')
    if mode is None or mode.get('value') != fc.synthesis_mode(config):
        bad.append('fpga/gfx2d_ti60/gfx2d_ti60.xml synthesis.mode')
    sw = MAIN / 'sw' / 'bsp' / 'include' / 'gfx_feature.h'
    if sw.is_file() and sw.read_text(encoding='utf-8') != software_header(name, config):
        bad.append('sw/bsp/include/gfx_feature.h')
    if bad:
        print('feature_build --check: 与默认构型不一致：' + ', '.join(bad))
        print('运行 Main/tools/check/feature_build.py --sync 重新生成。')
        return 1
    print('feature_build --check: 通过。')
    return 0


def build(config_name, out_dir):
    config = fc.parse_name(config_name)
    reasons = fc.check(config)
    if reasons:
        raise SystemExit('INVALID ' + config_name + ': ' + '；'.join(reasons))
    name = fc.name_of(config)
    out = Path(out_dir).resolve()
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    caps = fc.caps(config)

    (out / 'engine_feature.vh').write_text(feature_header(name, config), encoding='utf-8', newline='\n')
    (out / 'engine_regs_offsets.vh').write_text(regs_gen.gen_vh(caps), encoding='utf-8', newline='\n')
    (out / 'gfx_regs.h').write_text(regs_gen.gen_h(caps), encoding='utf-8', newline='\n')
    (out / 'gfx_feature.h').write_text(software_header(name, config), encoding='utf-8', newline='\n')
    populate_tree(config, out)
    project_xml(config, out / 'fpga/gfx2d_ti60/gfx2d_ti60.xml', out)

    sim_include = out / 'sim-include'
    sim_include.mkdir()
    shutil.copyfile(out / 'engine_feature.vh', sim_include / 'engine_feature.vh')
    # 树根不再留这两份的副本：它们已经进了 rtl/engine/include/，
    # 留着会让按 CWD 找 include 的工具读到树根那份，掩盖真正被编译的头。
    for name_ in ('engine_feature.vh', 'engine_regs_offsets.vh'):
        (out / name_).unlink()

    record = {
        'config': name,
        'preset': config_name if config_name in fc.PRESETS else None,
        'caps': f'0x{caps:08X}',
        'synthesis_mode': fc.synthesis_mode(config),
        'flags': {k: bool(config[k]) for k in fc.ALL_NAMES},
        'sources': list(enabled_rtl(config)),
        'excluded_sources': sorted({s for flag, srcs in FEATURE_SOURCES.items()
                                    if not config[flag] for s in srcs}),
        'guards': [{'owner': o, 'needs': n, 'module': m} for o, n, m in fc.GUARDS],
        'excludes': [{'first': a, 'second': b, 'module': m} for a,b,m in fc.EXCLUDES],
    }
    (out / 'config.json').write_text(json.dumps(record, ensure_ascii=False, indent=2) + '\n',
                                     encoding='utf-8')
    return record


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--config', help='构型名（代表档如 B5）或位串（如 11110-111-d）')
    ap.add_argument('--out-dir', help='产物目录；存在时先清空')
    ap.add_argument('--list', action='store_true', help='列出代表档与合法结构组合')
    ap.add_argument('--sync', action='store_true', help='把默认构型的特性头写进 rtl/engine/（声明头进 include/）')
    ap.add_argument('--check', action='store_true', help='校验 rtl/engine/ 的特性头与默认构型一致')
    ap.add_argument('--headers-only', action='store_true', help='只向隔离目录写构型/寄存器头，不清目录或修改默认构型')
    args = ap.parse_args()
    if args.sync:
        print('SYNC ' + sync_canonical())
        return 0
    if args.check:
        return check_canonical()
    if args.list:
        print('代表档：')
        for name in fc.PRESETS:
            config = fc.parse_name(name)
            print(f'  {name}  {fc.name_of(config)}  CAPS=0x{fc.caps(config):08X}')
        structures = fc.legal_configurations(with_pixel=False)
        print(f'\n合法结构组合 {len(structures)} 种：')
        for config in structures:
            bits = ''.join('1' if config[n] else '0' for n in fc.STRUCT_NAMES)
            print(f'  {bits}  CAPS=0x{fc.caps(config):08X}')
        return 0
    if not args.config or not args.out_dir:
        ap.error('需要 --config 与 --out-dir')
    if args.headers_only:
        write_headers(args.config, args.out_dir)
        print('HEADERS '+args.config)
        return 0
    record = build(args.config, args.out_dir)
    print(f"BUILT {record['config']} CAPS={record['caps']} sources={len(record['sources'])} "
          f"excluded={len(record['excluded_sources'])}")
    return 0


if __name__ == '__main__':
    sys.exit(main())
