#!/usr/bin/env python3
"""R3 前期研究的可执行核实：页选、CAPS/依赖图、FSM 插入点、错误与寄存器、板上对拍通路。

对每一步先读真实源码做正则断言，再导入 feature_config/regs_gen 做枚举与数学；
任何断言失败即非零退出。不是 RTL 验证，结论只覆盖集成边界的事实。

复现（从仓库根，输出换到新的忽略目录）：
  python3 Engineering/TechReports/engine/evidence/18-r3-research-20261023/research.py \
    --out Main/sim/build/r3-research-repro/research.json
"""
import argparse
import itertools
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[5]
sys.path.insert(0, str(ROOT / 'Main/tools/check'))
import feature_config as fc          # noqa: E402
import regs_gen                       # noqa: E402

CHECKS = []


def check(name, ok, detail):
    CHECKS.append({'name': name, 'pass': bool(ok), 'detail': detail})
    print(('PASS ' if ok else 'FAIL ') + name + ' ' + detail, flush=True)


def source(relpath):
    return (ROOT / relpath).read_text(encoding='utf-8')


def need(pattern, text, flags=re.M):
    found = re.findall(pattern, text, flags)
    if not found:
        raise AssertionError('源码中未找到: ' + pattern)
    return found


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()

    # ---- 1 顶层页选：第 1 页现落默认应答，R3 须扩 apb_sel_eng ----
    top = source('Main/rtl/top/gfx2d_top.v')
    sel = need(r"^wire\s+apb_sel_eng\s*=\s*(.+?);", top)[0]
    sel = re.sub(r'\s+', '', sel)
    expected_sel = "(apb_paddr[15:12]==4'h0)||(apb_paddr[15:14]==2'b01)"
    route = {}
    for page in range(16):
        hi = page >> 2
        if page == 0:
            route[page] = 'engine_regs'
        elif hi == 1:
            route[page] = 'engine_tex'
        elif page == 0xE:
            route[page] = 'display'
        elif page == 0xF:
            route[page] = 'axi_mux_guard'
        else:
            route[page] = 'default(PSLVERR)'
    check('page_decode',
          sel == expected_sel and route[1].startswith('default') and
          sorted(p for p, r in route.items() if r == 'engine_tex') == [4, 5, 6, 7],
          'apb_sel_eng=' + sel + '; 第1页=' + route[1] +
          '; TEX页={4,5,6,7}; R3须增 4\'h1 支路')

    # ---- 2 engine_top 内分选：现只有第 0 页寄存器与第 4~7 页 TEX ----
    etop = source('Main/rtl/engine/engine_top.v')
    hit_regs = need(r"apb_hit_regs\s*=\s*\(paddr\[15:12\]\s*==\s*4'h0\)", etop)
    hit_tex = need(r"apb_hit_tex\s*=\s*\(paddr\[15:14\]\s*==\s*2'b01\)", etop)
    no_rast_hit = not re.search(r"apb_hit_\w+\s*=\s*.*4'h1", etop)
    mux = need(r"assign\s+prdata\s*=\s*apb_hit_regs\s*\?\s*regs_prdata\s*:\s*tex_prdata", etop)
    check('engine_top_dispatch', bool(hit_regs and hit_tex and no_rast_hit and mux),
          'hit_regs(页0)/hit_tex(页4~7)在；页1无支路(须新增 hit_rast 与三路 mux)')

    # ---- 3 条带 FSM 插入点：S_BSTART/S_BLOAD/…/S_BWAIT 与退出条件 ----
    cmd = source('Main/rtl/engine/engine_cmd.v')
    states = need(r"S_BSTART=(\d+),\s*S_BLOAD=(\d+),\s*\n?\s*S_BFLUSH=(\d+),\s*S_BWAIT=(\d+)", cmd)
    band_start = need(r"assign band_start\s*=\s*st\s*==\s*S_BSTART\s*&&\s*!soft_rst;", cmd)
    band_flush = need(r"assign band_flush\s*=\s*st\s*==\s*S_BFLUSH\s*&&\s*!soft_rst;", cmd)
    empty_exit = need(r"if\(count_r==0\)\s*st<=S_BFLUSH;", cmd)
    hdr8 = need(r"if\(hdr_beat==7\)\s*st<=S_BNEXT;", cmd)
    strip_rng = need(r"lstrips==0\s*\|\|\s*lstrips>30", cmd)
    stride = need(r"\*32768", cmd)
    check('fsm_insertion',
          bool(states and band_start and band_flush and empty_exit and hdr8 and strip_rng
               and len(stride) >= 2),
          'S_BSTART/S_BLOAD/S_BFLUSH/S_BWAIT 在；band_start/flush 由状态直译；'
          '空表直接 FLUSH 先例在(S_BLOAD 出口)；表头 8 拍；条带 1..30、步长 32KB；'
          'RAST 相插在 S_BLOAD 出口与 2D 派发之间')

    # ---- 4 门铃锁存纪律：kick 一拍，LIST_* 同拍落 ----
    regs = source('Main/rtl/engine/engine_regs.v')
    kick = need(r"assign kick\s*=\s*door_accepted;", regs)
    latch = need(r"S_IDLE:\s*if\s*\(kick\)\s*begin", cmd)
    latch2 = need(r"base_r\s*<=\s*list_base;\s*\n\s*count_r\s*<=\s*list_count;", cmd)
    check('doorbell_discipline', bool(kick and latch and latch2),
          'kick=door_accepted 一拍；cmd 在 S_IDLE 同拍锁存 base/count/strips/table_base；'
          'R3 的 TRI_BASE/SUB_BASE/SUB_BYTES/TRI_COUNT/RAS_KEY/TEXCFG 须同一拍锁存')

    # ---- 5 CAPS 数学：B7 = B3 | bit14 | bit15 = 0xD46F ----
    preset_caps = {name: fc.caps(fc.parse_name(name))
                   for name in ('B0', 'B1', 'B2', 'B3', 'B4', 'B5', 'B6')}
    no_hi = all(v & 0xC000 == 0 for v in preset_caps.values())
    caps_bit7 = dict(fc.CAPS_BIT, RAST=14, RAST_TEX=15)
    all_names7 = fc.ALL_NAMES + ('RAST', 'RAST_TEX')

    def caps7(config):
        config = {n: bool(config.get(n, False)) for n in all_names7}
        value = fc.CAPS_LANES_VALUE << fc.CAPS_LANES_LO
        for name, bit in caps_bit7.items():
            on = config['ALPHA'] if name in ('ALPHA_PM', 'ALPHA_ST') else \
                config['BAND_HW'] if name == 'BAND_HW' else config[name]
            if on:
                value |= 1 << bit
        return value

    b7 = dict(fc.PRESETS['B3'], RAST=1, RAST_TEX=1)
    check('caps_math',
          preset_caps['B3'] == 0x146F and no_hi and caps7(b7) == 0xD46F and
          caps7(dict(fc.PRESETS['B5'])) == preset_caps['B5'],
          'B3=0x146F；既有预设 bit14/15 全 0；B7=0x%04X；旧预设值不因新增位改变'
          % caps7(b7))

    # ---- 6 依赖图：7 结构位的合法组合数（_self_check 更新依据） ----
    names7 = fc.STRUCT_NAMES + ('RAST', 'RAST_TEX')
    depends7 = dict(fc.DEPENDS, RAST=('STRIP',), RAST_TEX=('RAST', 'TEX'))

    def illegal(config):
        return [f'{n} 需 {d}' for n, needs in depends7.items() if config[n]
                for d in needs if not config[d]]

    structs7 = [dict(zip(names7, bits)) for bits in
                itertools.product((True, False), repeat=len(names7))]
    structs7 = [c for c in structs7 if not illegal(c)]
    research7, delivery7 = [], []
    for s in structs7:
        for p in itertools.product((True, False), repeat=len(fc.PIXEL_NAMES)):
            c = {**s, **dict(zip(fc.PIXEL_NAMES, p))}
            c['DELIVERY'] = c['ALPHA'] and c['KEY']
            (delivery7 if c['DELIVERY'] else research7).append(c)
    old_structs = [c for c in
                   [dict(zip(fc.STRUCT_NAMES, bits)) for bits in
                    itertools.product((True, False), repeat=len(fc.STRUCT_NAMES))]
                   if fc.is_legal(c)]
    check('dep_graph',
          len(old_structs) == 16 and len(structs7) == 25 and
          (len(research7), len(delivery7)) == (150, 50),
          '旧 5 位合法结构 16（对上 _self_check）；新 7 位合法结构 %d；'
          'research/delivery = %d/%d，共 %d（旧 128/32=160）'
          % (len(structs7), len(research7), len(delivery7),
             len(research7) + len(delivery7)))

    # ---- 7 synthesis_mode：显式表对旧预设是纯扩展 ----
    old_modes = {n: fc.synthesis_mode(fc.parse_name(n)) for n in
                 ('B0', 'B1', 'B2', 'B3', 'B4', 'B5', 'B6')}
    explicit = {'B5': 'speed2', 'B7': 'speed2'}
    table = {n: explicit.get(n, 'speed') for n in old_modes}
    check('synthesis_mode',
          old_modes == {'B0': 'speed', 'B1': 'speed', 'B2': 'speed', 'B3': 'speed',
                        'B4': 'speed', 'B5': 'speed2', 'B6': 'speed'} and
          table == old_modes,
          '现行规则 speed2 仅 B5；显式表 {B5,B7}→speed2 对 B0~B6 逐项等价（纯扩展）')

    # ---- 8 寄存器真源：错误码、CAPS 位、第 1 页窗口 ----
    codes = dict(regs_gen.ERR_CODES)
    free_codes = [v for v in range(16) if v not in codes.values()]
    reg_hi = regs_gen.PERF0 + 4 * regs_gen.PERF_COUNT
    check('regs_map',
          codes.get('BAD_INDEX') == 13 and free_codes == [14, 15] and
          regs_gen.TEX_WINDOW == 0x4000 and reg_hi == 0x138 and
          not any(getattr(regs_gen, n) >= 0x1000 for n in
                  ('ID', 'CAPS', 'CTRL', 'STATUS', 'IRQ_EN', 'IRQ_ST', 'ERR_IDX',
                   'ERR_TAG', 'LBASE', 'LCNT', 'DOORBELL', 'LSTRIPS', 'STRIP_H',
                   'BAND_SURF', 'SURF0', 'PERF0')),
          'BAD_INDEX=13 已登记；4 位码剩 14/15；第 0 页最高偏移 0x%03X < 0x1000；'
          '第 1 页窗口 0x1000~0x1FFF 空闲（TEX 窗口 0x4000 不变）' % reg_hi)

    # ---- 9 错误通路：stage 7 空闲，tag 位形，错误 mux 位置 ----
    pb = source('Main/rtl/engine/engine_pb.v')
    pb_stages = {int(v) for v in
                 re.findall(r"error_stage\s*<=\s*(\d+)\s*;", pb)}
    stages_in_top = need(r"\.err_stage\(pb_active \? pb_error_stage : 4'd0\)", etop)
    tag_bits = 11 + 5 + 16
    check('error_path',
          7 not in pb_stages and max(pb_stages) < 7 and bool(stages_in_top) and
          tag_bits == 32,
          "PB 现用 stage=%s，7 空闲；engine_top 错误 mux 在 err_code/err_stage 处扩"
          " rast 三元组；ERROR_TAG={11'd0,strip[4:0],ordinal[15:0]}=32b"
          % sorted(pb_stages))

    # ---- 10 z 清零预算与新口存在性 ----
    rast = source('Main/rtl/engine/engine_rast.v')
    zport = need(r"input\s+wire\s+z_clear,\s*\n\s*output wire\s+z_clear_busy,\s*\n\s*output reg\s+z_clear_done,", rast)
    per_strip = 640 * 16 // 4
    total = per_strip * 30
    us = total / 100e6 * 1e6
    check('z_clear_budget', bool(zport) and per_strip == 2560 and total == 76800
          and abs(us - 768.0) < 1e-9,
          'rast 有 z_clear/z_clear_busy/z_clear_done；2560 拍/带×30=76800 拍=768.0µs@100MHz')

    # ---- 11 BAND 租约与 TEX 采样口 gating ----
    band = source('Main/rtl/engine/engine_band.v')
    lease = need(r"assign ras_wr_ready\s*=\s*ras_lease\s*&&\s*phase\s*==\s*EXEC\s*&&", band)
    tex = source('Main/rtl/engine/engine_tex.v')
    samp = need(r"assign samp_ready\s*=\s*!busy_r\s*&&\s*!req_valid\s*&&\s*!apb_access\s*&&\s*!apb_rd_setup", tex)
    check('band_tex_ports', bool(lease and samp),
          'band 光栅写口=租约∧EXEC∧界内∧无保持；tex 采样口与 legacy/APB 互斥已实现；'
          '帧层只须在 RAST 相拉 ras_lease')

    # ---- 12 板上对拍通路：R1 侧、驱动侧、判据侧的事实 ----
    rasttest = source('Main/sw/apps/test/rastTest/src/main.c')
    driver = source('Main/sw/bsp/ti60/gfx_engine.c')
    disptest = source('Main/sw/apps/test/displayTest/src/main.c')
    fixture = json.loads(source('Main/sim/engine/rast/r1-records.json'))
    golden = (ROOT / 'Main/sw/lib/test/golden.txt').exists()
    perf_wait = 'EC_PERF_WAIT_DDR' in regs
    check('board_path',
          '由 R3 承接' in rasttest and
          'GFX_DOORBELL' in driver and 'GFX_LIST_BASE' in driver and
          'underflow_beats' in disptest and perf_wait and golden and
          isinstance(fixture, list) and len(fixture) >= 16,
          'rastTest 显式留有 R3 对拍接口；驱动经 gfx_engine.c 写 LIST_BASE/DOORBELL；'
          'displayTest 欠载判据在(D_UNDER_BEATS=0)；PERF_WAIT_DDR 计数在；'
          'CRC golden 机制在；R1 冻结 %d 条记录可作对拍种子' % len(fixture))

    # ---- 13 帧容量与 TEXCFG 边界 ----
    tri = 1100
    main_bytes = tri * 128
    per_strip_refs = -(-tri // 30)
    sub_bytes = 30 * (4 + 2 * per_strip_refs)
    texcfg = (64 - 1) * 64 + 64
    check('frame_capacity',
          main_bytes == 140800 and sub_bytes == 30 * 78 and texcfg == 4096,
          '目标场景 ~1100 tri：主表 %dB、子表 %dB（<4KB DDR 无压力）；'
          'TEXCFG v1 64×64 边界 (h-1)*stride+w=4096≤4096 过 rast 检查；slog2=6'
          % (main_bytes, sub_bytes))

    failed = [c for c in CHECKS if not c['pass']]
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps({
        'research': 'R3 前期研究（集成边界事实核实，非 RTL 验证）',
        'checks': CHECKS,
        'new_self_check': {'legal_structures': len(structs7),
                           'research': len(research7), 'delivery': len(delivery7),
                           'total': len(research7) + len(delivery7)},
        'b7_caps': '0x%08X' % caps7(b7),
    }, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print('RESEARCH %s checks=%d failed=%d' %
          ('PASS' if not failed else 'FAIL', len(CHECKS), len(failed)), flush=True)
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
