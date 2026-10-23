#!/usr/bin/env python3
"""R3 research corrections. No production RTL/configuration is changed.

Run from repository root under WSL:
  python3 Engineering/TechReports/engine/evidence/18-r3-review-20261023/audit.py \
    --out Main/sim/build/r3-review-20261023/audit
Source observations, exhaustive policy models, real C ABI experiments and an
rlst cancellation contract witness have separate names in the output.
"""
import argparse
import ctypes as C
import hashlib
import itertools
import json
from pathlib import Path
import re
import statistics
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[5]
sys.path.insert(0, str(ROOT / 'Main/tools/check'))
import feature_config as fc
from check_rast import SOURCES


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--board-log', type=Path)
    a = p.parse_args()
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    checks, inputs = [], {}

    def read(rel):
        data = (ROOT / rel).read_bytes()
        inputs[rel] = hashlib.sha256(data).hexdigest()
        return data.decode('utf-8-sig')

    def check(name, ok, detail):
        checks.append(dict(name=name, passed=bool(ok), detail=detail))
        print(('PASS ' if ok else 'FAIL ') + name + ' ' + str(detail), flush=True)

    def run(cmd, name):
        r = subprocess.run(list(map(str, cmd)), cwd=ROOT, text=True,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (out / name).write_text(r.stdout, encoding='utf-8')
        if r.returncode:
            raise RuntimeError(name + ': ' + r.stdout[-2000:])
        return r.stdout

    for rel in SOURCES + ['Main/tools/check/feature_config.py',
                           'Main/tools/check/check_rast.py',
                           'Main/sw/mk/target.mk', 'Main/sw/bsp/ti60/board.c',
                           'Main/sw/bsp/ti60/gfx_engine.c',
                           'Main/sw/lib/gfx/rast_setup.h',
                           'Main/rtl/engine/engine_feature.vh']:
        read(rel)

    old = json.loads(read('Engineering/TechReports/engine/evidence/'
                         '18-r3-research-20261023/research.json'))
    check('historical_false_confidence', all(x['pass'] for x in old['checks']),
          'historical 13/13 PASS is reproducible; it is not a closure proof')
    names = fc.STRUCT_NAMES + ('RAST', 'RAST_TEX')
    dep = dict(fc.DEPENDS, RAST=('STRIP',), RAST_TEX=('RAST', 'TEX'))

    def deps(c):
        return all(not c[n] or all(c[d] for d in needs) for n, needs in dep.items())

    structures = [dict(zip(names, bits)) for bits in itertools.product((False, True), repeat=7)]
    missing_mutex = [c for c in structures if deps(c) and c['PB'] and c['RAST']]
    legal = [c for c in structures if deps(c) and not (c['PB'] and c['RAST'])]
    research, delivery = [], []
    for s in legal:
        for bits in itertools.product((False, True), repeat=3):
            for d in (False, True):
                c = dict(s, **dict(zip(fc.PIXEL_NAMES, bits)), DELIVERY=d)
                if d and not (c['ALPHA'] and c['KEY']):
                    continue
                (delivery if d else research).append(c)
    check('full_configuration_enumeration',
          (len(missing_mutex), len(legal), len(research), len(delivery)) == (3, 22, 176, 44),
          dict(structures=22, research=176, delivery=44, total=220,
               illegal_structures=106, illegal_full=1828, missed_pb_rast=3))

    def mode(c):
        return 'speed2' if all(c[n] for n in fc.STRUCT_NAMES) or c['RAST'] else 'speed'

    legacy = fc.legal_configurations()
    mode_changes = [fc.name_of(c) for c in legacy
                    if mode(dict(c, RAST=False)) != fc.synthesis_mode(c)]
    pb_policy_changes = [fc.name_of(c) for c in legacy
                         if ('speed2' if c['PB'] else 'speed') != fc.synthesis_mode(c)]
    check('preserve_all_legacy_modes', not mode_changes and bool(pb_policy_changes),
          dict(legacy_configurations=len(legacy), correct_changes=len(mode_changes),
               wrong_pb_implies_speed2_changes=len(pb_policy_changes),
               rule='all(original five structure bits) OR RAST -> speed2'))
    b7 = fc.caps('B3') | 0xC000
    check('b7_caps', b7 == 0xD46F, hex(b7))

    rlst = read('Main/rtl/engine/engine_rlst.v')
    cmd = read('Main/rtl/engine/engine_cmd.v')
    rast = read('Main/rtl/engine/engine_rast.v')
    band = read('Main/rtl/engine/engine_band.v')
    mover = read('Main/rtl/engine/engine_mover.v')
    etop = read('Main/rtl/engine/engine_top.v')
    tex = read('Main/rtl/engine/engine_tex.v')
    check('fixed_30_strips', "strip == 5'd29" in rlst and 'lstrips>30' in cmd
          and "input  wire [4:0]  lstrips" not in rlst,
          '3D nonzero count requires BAND=1, LSTRIPS=30 and 640x480 surfaces; '
          'TRI_COUNT=0 retains existing 1..30 strip and direct modes')
    pulses = ('loaded <= 0;' in band and 'loaded <= 1;' in band and
              "z_clear_done <= 1'b0;" in rast and "z_clear_done <= 1'b1;" in rast)
    sticky_good, naive_misses = 0, 0
    for load, clear in itertools.product(range(8), repeat=2):
        ls = zs = False
        naive = sticky = False
        for t in range(9):
            lp, zp = t == load, t == clear
            naive |= lp and zp
            ls |= lp
            zs |= zp
            sticky |= ls and zs
        sticky_good += sticky
        naive_misses += not naive
    check('completion_pulse_barrier', pulses and sticky_good == 64 and naive_misses == 56,
          dict(sticky_pass=sticky_good, naive_and_misses=naive_misses, clear_writes=2560,
               clear_accept_to_done_edges=2561,
               requirement='clear load_seen/z_seen per accepted band_start, then sticky join'))
    suppressed = ('assign fsr_tvalid' in mover and
                  re.search(r'assign fsr_tvalid\s*=.*!rd_drain_help', mover) is not None)
    check('mover_drain_is_not_rlst_drain', suppressed and
          's_recv == s_beats || s_last_seen' in rlst and
          'drain_done = draining && !rd_busy && !wr_busy && !tx_busy && !band_busy' in mover,
          'mover drain consumes/discards raster FS; canceled rlst otherwise waits forever; '
          'sticky mover_done permits transaction-local rlst reset, then join raster/TEX/z idle')
    check('error_and_terminal_are_distinct', "frame_done <= !canceled && !cancel;" in rlst and
          "err_valid <= 1'b0;" in rast and 'err_hold' in rast,
          'rlst frame_done may accompany error and is suppressed on cancel; latch first error, '
          'cancel both units, recover err_hold before a subsequent frame')
    check('fd_and_error_plumbing_missing', '.fd_raster(' not in etop and '.fsr_tdata(' not in etop
          and '.err_stage(4\'d0)' in etop,
          'R3 must wire all conditional mover raster/BAND and TEX sample ports, '
          'arbitrate CMD/INST/RAST by accepted descriptor, and select full error tuple')
    check('tex_not_frame_locked', 'apb_wr_en' in tex and
          "assign pslverr = apb_access && !apb_aligned;" in tex,
          'port arbitration does not freeze atlas data for a frame; '
          '3D frame busy must block TEX APB side effects; shadow CSRs remain writable')

    library = out / 'reference.so'
    run(['gcc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror', '-fPIC', '-shared',
         '-IMain/sw/lib', *SOURCES, '-o', library], 'c-build.log')
    lib = C.CDLL(str(library))

    class Vertex(C.Structure):
        _fields_ = [('x', C.c_int32), ('y', C.c_int32), ('z', C.c_uint16),
                    ('u', C.c_uint16), ('v', C.c_uint16), ('color', C.c_uint32)]

    record = (C.c_uint8 * (128 * 12))()
    vertices = (Vertex * 3)(Vertex(0, 0, 10000, 0, 0, 0xff804020),
                            Vertex(639*16, 0, 10000, 0, 0, 0xff804020),
                            Vertex(0, 479*16, 10000, 0, 0, 0xff804020))
    lib.rast_setup_polygon.argtypes = [C.POINTER(Vertex), C.c_uint, C.c_uint32,
                                      C.c_void_p, C.c_uint, C.c_void_p]
    lib.rast_bin.argtypes = [C.c_void_p, C.c_uint, C.c_void_p, C.c_size_t]
    assert lib.rast_setup_polygon(vertices, 3, 2, record, 12, None) == 1
    table = (C.c_uint8 * (128 * 1100)).from_buffer_copy(bytes(record[:128])*1100)
    small = (C.c_uint8 * 2340)(*([0xa5]*2340))
    small_result = lib.rast_bin(table, 1100, small, len(small))
    large = (C.c_uint8 * 66120)()
    size = lib.rast_bin(table, 1100, large, len(large))
    check('real_c_cross_strip_capacity', small_result == -2 and bytes(small) == b'\xa5'*2340
          and size == 66120,
          dict(records=1100, references=33000, sub_bytes=size, physical_bytes=(size+15)//16*16,
               main_bytes=140800, fetched_record_bytes=33000*128,
               max_sub_bytes_65535=120+2*30*65535))
    out_tb = out / 'cancel.vvp'
    tb = Path(__file__).with_name('tb_cancel_contract.v')
    run(['iverilog', '-g2012', '-Wportbind', '-IMain/rtl/engine', '-s', 'tb_cancel_contract',
         '-o', out_tb, tb, 'Main/rtl/engine/engine_rlst.v'], 'cancel-compile.log')
    body = run(['vvp', '-n', out_tb], 'cancel.log')
    check('actual_rlst_cancel_witness', 'RESULT: PASS' in body,
          'actual rlst: lost FS + mover completion leaves busy stuck; '
          'post-mover local reset recovers; normal forwarded cancel drains')
    display = read('Main/rtl/display/display_out.v')
    check('display_live_observation', 'underflow_beats <= underflow_beats + ' in display
          and 'wire vis_fb = de1 & src_eff;' in display,
          'require SRC=1, fresh SNAP_FRAME, zero/under/resperr before/during/after mixed load; '
          'standalone displayTest and PERF_WAIT_DDR do not prove mixed-load absence of underflow')
    scan = 640*480*1100
    check('scan_budget_is_not_framebuffer_size', scan == 337920000,
          dict(worst_bbox_tests=scan, ideal_scan_seconds_at_100mhz=scan/1e8,
               requirement='measure sum bbox intersections, references, fetch, init/drain, CPU and flush'))

    board = None
    if a.board_log:
        body = a.board_log.read_text(encoding='utf-8-sig')
        body = body.split('# --- 程序已加载，以下才是它的输出 ---', 1)[1]
        rows = re.findall(r'^(polygon|view|bin) cycles=(\d+) count=(\d+) per_call=(\d+)', body, re.M)
        board = {}
        for count in (16, 32, 64, 1100):
            board[str(count)] = {kind: statistics.median(int(cycles)/1e5 for k,cycles,n,_ in rows
                                                       if k == kind and int(n) == count)
                                 for kind in ('polygon', 'view', 'bin')}
            b = board[str(count)]
            b['view_plus_bin_ms'] = b['view'] + b['bin']
        check('board_probe_complete', len(rows) == 36 and
              re.findall(r'^fail \d+\r?$', body, re.M) == ['fail 0'] and
              re.search(r'^done\r?$', body, re.M) is not None and '# serial lost' not in body,
              board)
        check('old_1100_budget_falsified', board['1100']['view_plus_bin_ms'] > 33.333,
              'synthetic deterministic no-near-clip CPU probe, not R4 or B7 frame performance')

    inputs['audit.py'] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    inputs['tb_cancel_contract.v'] = hashlib.sha256(tb.read_bytes()).hexdigest()
    result = dict(scope='R3 predevelopment review, not R3 acceptance', checks=checks,
                  policy=dict(structures=22, research=176, delivery=44, total=220),
                  structure_bit_order=list(names),
                  legal_structures=[''.join(str(int(c[n])) for n in names) for c in legal],
                  incorrect_pb_mode_changes=pb_policy_changes,
                  inputs=inputs, board_probe_medians=board)
    (out / 'result.json').write_text(json.dumps(result, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
    if not all(x['passed'] for x in checks):
        raise SystemExit(1)
    print('AUDIT PASS checks=' + str(len(checks)), flush=True)


if __name__ == '__main__':
    main()
