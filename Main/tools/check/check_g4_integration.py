#!/usr/bin/env python3
"""G4：真实游戏 v1/v2 对拍、批量分桶差分、综合和板上证据复核。"""
import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
from pathlib import Path

MAIN = Path(__file__).resolve().parents[2]
ROOT = MAIN.parent
SIM = MAIN / 'sim/engine/g4'
from check_run import paths
BUILD, LOGS = paths('g4', 'g4')
APP = MAIN / 'sw/apps/game/gfx2dGame'


def need(ok, message):
    if not ok:
        raise RuntimeError(message)


def run(name, args, cwd=ROOT, env=None):
    p = subprocess.run(list(map(str, args)), cwd=cwd, env=env,
                       capture_output=True, text=True)
    (LOGS / (name + '.log')).write_text(p.stdout + p.stderr, encoding='utf-8')
    need(p.returncode == 0, name + ' failed; see log')
    return p.stdout + p.stderr


def body(path, banner=None):
    text = Path(path).read_text(encoding='utf-8-sig', errors='replace')
    need('# serial lost' not in text, str(path) + ': serial lost')
    lines = [x.strip() for x in text.splitlines() if x.strip()]
    if banner:
        starts = [i for i, x in enumerate(lines) if x == banner]
        need(starts, str(path) + ': banner missing')
        lines = lines[starts[-1]:]
    need('fail 0' in lines and 'done' in lines, str(path) + ': fail/done missing')
    need(not any(x.startswith(('err ', 'engine err ', 'compare PERF mismatch')) for x in lines),
         str(path) + ': program error')
    return lines


def compare(reference, actual, samples=False, hardware=False):
    ref = {int(x.split()[1]): x for x in body(reference) if x.startswith('g ')}
    got = body(actual, 'gfx2dGame STG' if hardware else None)
    rows = [x for x in got if x.startswith('g ')]
    expected = list(range(0, 9208, 600)) + [9207] if samples else list(range(9208))
    need([int(x.split()[1]) for x in rows] == expected, 'frame coverage/order mismatch')
    for row in rows:
        need(ref.get(int(row.split()[1])) == row, 'same-frame mismatch: ' + row)
    need('end frames 9208 hits 4 over 0' in got, 'replay end differs')
    if samples and hardware:
        crc = [x.split() for x in got if x.startswith('compare ') and not x.startswith('compare tex ')]
        need([int(x[1]) for x in crc] == expected, 'sample CRC coverage mismatch')
        need(all(len(x) == 6 and x[3] == x[5] for x in crc), 'sample CRC mismatch')
    return {'frames': len(rows), 'full_replay': not samples, 'differences': 0}


def replay_progress(reference, actual):
    ref=[x for x in body(reference) if x.startswith('g ')]
    text=Path(actual).read_text(encoding='utf-8',errors='replace')
    lines=text.split('\n')[:-1] # 正在写入的最后一行不作判据。
    starts=[i for i,x in enumerate(lines) if x.strip()=='gfx2dGame STG']
    if not starts:
        print('G4 replay: waiting for program banner')
        return
    lines=[x.strip() for x in lines[starts[-1]:]]
    need(not any(x.startswith(('err ','engine err ')) for x in lines),'live replay program error')
    rows=[x for x in lines if x.startswith('g ')]
    need(len(rows)<=len(ref),'live replay extra frames')
    for i,row in enumerate(rows):
        need(row==ref[i],'live replay differs at frame '+str(i))
    print('G4 live replay: %d/9208 complete frames, differences=0 (not final acceptance)'%len(rows))


def host():
    import os
    libs = [MAIN / 'sw/lib' / x for x in
            ('gfx/list.c', 'gfx/strip.c', 'gfx/strip_plan.c', 'gfx/check.c', 'core/format.c')]
    run('batch_compile', ['gcc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                         '-fsanitize=address,undefined', '-I' + str(MAIN / 'sw/lib'),
                         SIM / 'test_batch.c', *libs, '-o', BUILD / 'test_batch'])
    result = run('test_batch', [BUILD / 'test_batch'])
    need('PASS: 10000' in result, 'batch test missing evidence')
    flags = '-DSTG_FIELD=4 -DSTG_JUDGE_REPLAY=1 -DGFX2D_HEADLESS=1'
    env = dict(os.environ, GFX2D_UNPACED='1')
    for name, extra in [('flat', ''), ('strip', '-DG4_STRIP_RENDER=1')]:
        obj = 'build_g4_verify_' + name
        run('host_' + name + '_compile', ['make', 'PLATFORM=host', 'OBJDIR=' + obj,
            '-j8', 'EXTRA_CFLAGS=' + flags + ' ' + extra], APP)
        run('host_' + name, [APP / obj / 'gfx2dGame'], APP, env)
    data = compare(LOGS / 'host_flat.log', LOGS / 'host_strip.log')
    data['batch_scalar_scenes_sanitized'] = 10000
    (LOGS / 'host_results.json').write_text(json.dumps(data, indent=2) + '\n')
    print('G4 HOST PASS:', data, flush=True)


def synthesis(folder, tag):
    out = folder / 'outflow'
    flow = (out / 'g4_synthesis.log').read_text(encoding='utf-8-sig')
    need(all(re.search(r'\b' + s + r'\s*:\s*PASS', flow) for s in ('map','interface','pnr','pgm')),
         tag + ': four-step flow incomplete')
    timing = (out / 'gfx2d_ti60.timing.rpt').read_text()
    slacks = re.findall(r'^\s*(\w+)\s+\1\s+[-\d.]+\s+([-\d.]+)\s+\(', timing, re.M)
    need(slacks and all(float(v) >= 0 for _, v in slacks), tag + ': negative slack')
    core = [float(v) for c, v in slacks if c == 'core_clk']
    place = (out / 'gfx2d_ti60.place.rpt').read_text()
    data = {k: int(re.search(pat, place).group(1)) for k, pat in
            [('xlr', r'XLRs:\s*(\d+)'), ('ram', r'Memory Blocks:\s*(\d+)'),
             ('dsp', r'DSP Blocks:\s*(\d+)')]}
    data.update(setup_ns=core[0], hold_ns=core[1], min_slack_ns=min(float(v) for _,v in slacks))
    for suffix in ('place.rpt','timing.rpt','hier_util.rpt'):
        raw = (out / ('gfx2d_ti60.' + suffix)).read_bytes()
        dest = LOGS / (tag + '_' + suffix)
        dest.write_bytes(raw)
        need(dest.read_bytes() == raw, 'report copy mismatch')
    (LOGS / (tag + '_synthesis.log')).write_text(flow)
    (LOGS / (tag + '_synthesis.json')).write_text(json.dumps(data, indent=2) + '\n')
    print('G4 SYN PASS:', tag, data, flush=True)


def register(paths):
    dest = LOGS / 'artifacts.json'
    items = json.loads(dest.read_text()) if dest.exists() else {}
    for path in paths:
        path = path.resolve()
        key = path.relative_to(ROOT).as_posix()
        stat = path.stat()
        old = items.get(key, {})
        if old.get('size') == stat.st_size and old.get('mtime_ns') == stat.st_mtime_ns:
            continue
        items[key] = {'size':stat.st_size, 'mtime_ns':stat.st_mtime_ns,
                      'sha256':hashlib.sha256(path.read_bytes()).hexdigest()}
    dest.write_text(json.dumps(items, indent=2) + '\n')
    print('G4 artifacts registered:', len(items), flush=True)


def rtl():
    game = APP / 'src'
    lib = MAIN / 'sw/lib'
    run('game_dump_compile', ['gcc', '-O2', '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
        *['-I' + str(p) for p in (lib, MAIN/'sw/bsp/include', game, MAIN/'assets/build')],
        MAIN/'tools/check/g4_game_dump.c', MAIN/'sw/ports/myriad/stg_modes.c',
        *sorted((game/'game').glob('stg_*.c')), *sorted((lib/'gfx').glob('*.c')),
        *sorted((lib/'core').glob('*.c')), '-o', BUILD/'game_dump'])
    result = run('game_golden', [BUILD/'game_dump', BUILD/'game',
                               MAIN/'assets/build/assets.pak', '1024'])
    need('G4 GAME GOLDEN PASS' in result, 'game fixture missing evidence')
    sources = [SIM/'tb_g4_frame.v', *sorted((MAIN/'rtl/engine').glob('**/*.v')),
        MAIN/'rtl/vendor/verilog-axi/axi_dma_rd.v', MAIN/'rtl/vendor/verilog-axi/axi_dma_wr.v',
        MAIN/'sim/axi_probe/axi_ram_model.v']
    result = {}
    for name, define in [('game_frame', None), ('game_bad_local_y', 'INJ_STRIP_LOCAL_Y'),
                         ('game_bad_header', 'INJ_STRIP_HEADER_OFFSET'),
                         ('game_bad_perf', 'INJ_STRIP_PERF')]:
        exe = BUILD/(name+'.vvp')
        args = ['iverilog', '-g2012', '-s', 'tb_g4_frame', '-I'+str(MAIN/'rtl/engine/include')]
        if define:
            args += ['-D'+define]
        run(name+'_compile', [*args, '-o', exe, *sources])
        output = run(name, ['vvp', '-n', exe, '+case='+str(BUILD/'game')])
        expected = 'RESULT: FAIL' if define else 'RESULT: PASS'
        need(expected in output and 'WATCHDOG' not in output, name+': unexpected RTL result')
        result[name] = {'expected': expected, 'observed': expected}
    (LOGS/'rtl_results.json').write_text(json.dumps(result, indent=2)+'\n')
    print('G4 RTL PASS: real game frame and three mutations red', flush=True)


def performance():
    result = {}
    for tag in ('strip_4k','strip_16k','tex_4k'):
        path=LOGS/('nmax_'+tag+'.log')
        run('check_nmax_'+tag,[sys.executable,MAIN/'tools/check/check_sw9_nmax.py',path,'--status','OK'])
        run('check_perf_'+tag,[sys.executable,MAIN/'tools/check/check_sw9_perf.py',path,'--fields-min','3600'])
        lines=body(path,'gfx2dGame MODES')
        rows=[]
        for line in lines:
            m=re.match(r'nmax result OK n_max (\d+) knob (\d+) ft_max (\d+) ft_p99 (\d+)',line)
            if m:
                rows.append(dict(zip(('n_max','knob','ft_max','ft_p99'),map(int,m.groups()))))
        need(len(rows)==2,'N_max result count')
        diag=next(x for x in lines if x.startswith('diag ')).split()
        values=dict(zip(diag[1::2],map(int,diag[2::2])))
        result[tag]={'16x16':rows[0],'8x8':rows[1],'diag':values,
                     'log':path.relative_to(ROOT).as_posix()}
    for tag in ('4k','16k'):
        path=LOGS/('s1_strip_'+tag+'.log')
        run('check_s1_'+tag,[sys.executable,MAIN/'tools/check/check_sw9_perf.py',path,'--fields-min','3600'])
        lines=body(path,'gfx2dGame MODES')
        row=next(x for x in lines if x.startswith('csvsum,')).split(',')
        frames,fields,drops=map(int,row[1:4])
        diag=next(x for x in lines if x.startswith('diag ')).split()
        result['s1_'+tag]={'frames':frames,'fields':fields,'drops':drops,
            'fps':frames*60/fields,'diag':dict(zip(diag[1::2],map(int,diag[2::2]))),
            'log':path.relative_to(ROOT).as_posix()}
    (LOGS/'performance_results.json').write_text(json.dumps(result,indent=2)+'\n')
    print('G4 performance evidence verified',flush=True)


def collect_board():
    performance()
    result={'samples_4k':compare(LOGS/'host_flat.log',LOGS/'compare_samples_board.log',True,True),
            'samples_16k':compare(LOGS/'host_flat.log',LOGS/'compare_samples_16k.log',True,True),
            'replay_4k':compare(LOGS/'host_flat.log',LOGS/'replay_strip_4k.log',False,True)}
    for name in ('cache_coherency_4k','cache_coherency_16k','strip_diagnostics_16k'):
        body(LOGS/(name+'.log'))
        result[name]={'fail':0}
    run('check_game_4k',[sys.executable,MAIN/'tools/check/check_sw9_perf.py',
        LOGS/'game_strip_4k.log','--warmup-rows','1','--steady-drops-max','0'])
    switch=body(LOGS/'game_switch_4k.log','gfx2dGame MODES')
    names=[x.split()[1] for x in switch if x.startswith('backend ')]
    need(names==['sw','hw','hw-tex','hw-strip'],'four-backend transitions differ')
    result['backend_switch']={'sequence':names,'errors':0}
    inject=body(LOGS/'inject_skip_invalidate.log','gfx2dGame STG')
    rows=[x.split() for x in inject if x.startswith('compare ') and not x.startswith('compare tex ')]
    need(len(rows)==10 and all(x[3]!=x[5] for x in rows),'cache mutation not detected')
    cache=body(LOGS/'cache_inject_4k.log')
    need(sum('FAIL (inject)' in x for x in cache)==4,'four cache invalidation mutations missing')
    selftest=(LOGS/'cache_selftest_4k.log').read_text().splitlines()
    need('fail 4' in selftest and 'done' in selftest and
         sum('NOT_RED (inject)' in x for x in selftest)==4,'cache self-test must reject false red')
    guard=(LOGS/'ddrBurstProbe_guard.log').read_text()
    need('S1 probe removed' in guard and 'fail 1' in guard and 'done' in guard,
         'retired probe must stop explicitly')
    result['mutations']={'game_crc_red':10,'cache_invalidation_red':4,
                         'cache_false_red_rejected':4,'retired_probe_expected_absence':True}
    for name in ('sw7_regression','sw8_regression'):
        body(LOGS/(name+'.log'))
        result[name]={'fail':0}
    result['artifact_registry']='Main/sim/engine/g4/logs/artifacts.json'
    (LOGS/'board_results.json').write_text(json.dumps(result,indent=2)+'\n')
    print('G4 BOARD PASS: complete replay, CRC/PERF, switches, cache and mutations',flush=True)


def build_target(suffix):
    riscv=os.environ.get('RISCV_BIN')
    need(riscv,'先设置 RISCV_BIN，取值见开发环境第 1 节')
    replay='-DSTG_FIELD=4 -DSTG_JUDGE_REPLAY=1 '
    search='-DGFX2D_START_MODE=3 -DGFX2D_PRESS_CLOCK=1 -DGFX2D_PRESS_EXIT=1 '
    flavors=[
        ('compare_samples',False,None,replay+'-DSW9_COMPARE=1 -DG4_STRIP_COMPARE=1 -DG4_SAMPLE_STEP=600'),
        ('replay',False,None,replay+'-DSW9_REPLAY_HW=1 -DG4_STRIP_RENDER=1'),
        ('nmax',True,None,search+'-DGFX2D_START_BACKEND=3'),
        ('nmax_tex',True,None,search+'-DGFX2D_START_BACKEND=2'),
        ('s1',True,None,'-DGFX2D_START_MODE=3 -DGFX2D_START_BACKEND=3 -DGFX2D_PRESS_CLOCK=1 -DGFX2D_PRESS_KNOB=1744 -DGFX2D_FIELDS_MAX=3600'),
        ('game',True,None,'-DGFX2D_START_MODE=2 -DGFX2D_START_BACKEND=3 -DGFX2D_FRAMES=600'),
        ('interactive',True,None,'-DGFX2D_START_MODE=2 -DGFX2D_START_BACKEND=3'),
        ('inject',False,'SW9_SKIP_INVALIDATE',replay+'-DSTG_FRAMES=10 -DSW9_COMPARE=1 -DG4_STRIP_COMPARE=1'),
    ]
    for name,perf,inject,flags in flavors:
        args=['make','-j8','RISCV_BIN='+riscv,'OBJDIR=build_g4_'+name+suffix,
              'EXTRA_CFLAGS='+flags]
        if perf:
            args+=['PERF=1']
        if inject:
            args+=['INJECT='+inject]
        run('target_'+name+suffix+'_compile',args,APP)
    for name,inject in [('fixed',None),('inject','1'),('selftest','2')]:
        args=['make','-j8','RISCV_BIN='+riscv,'OBJDIR=build_g4_'+name+suffix]
        if inject:
            args+=['INJECT='+inject]
        run('target_cache_'+name+suffix+'_compile',args,MAIN/'sw/apps/test/cacheCoherencyTest')
    print('G4 target programs built; register the actual deployment objects before board runs',flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--host', action='store_true')
    ap.add_argument('--rtl', action='store_true')
    ap.add_argument('--performance', action='store_true')
    ap.add_argument('--collect-board',action='store_true')
    ap.add_argument('--build-target',action='store_true')
    ap.add_argument('--target-suffix',default='',help='独立复现时追加到 OBJDIR，避免覆盖已登记板测对象')
    ap.add_argument('--synthesis', type=Path)
    ap.add_argument('--tag', default='4k')
    ap.add_argument('--register', nargs='+', type=Path)
    ap.add_argument('--samples', type=Path)
    ap.add_argument('--replay', type=Path)
    ap.add_argument('--replay-progress', type=Path)
    ap.add_argument('--reference', type=Path, default=LOGS / 'host_flat.log')
    a = ap.parse_args()
    BUILD.mkdir(parents=True, exist_ok=True)
    LOGS.mkdir(parents=True, exist_ok=True)
    if a.host:
        host()
    if a.rtl:
        rtl()
    if a.performance:
        performance()
    if a.collect_board:
        collect_board()
    if a.build_target:
        build_target(a.target_suffix)
    if a.synthesis:
        synthesis(a.synthesis, a.tag)
    if a.register:
        register(a.register)
    if a.samples:
        print('G4 SAMPLES PASS:', compare(a.reference, a.samples, True, True))
    if a.replay:
        print('G4 REPLAY PASS:', compare(a.reference, a.replay, False, True))
    if a.replay_progress:
        replay_progress(a.reference,a.replay_progress)


if __name__ == '__main__':
    main()
