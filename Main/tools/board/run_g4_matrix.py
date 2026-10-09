#!/usr/bin/env python3
"""G4 的板测矩阵：同份应用比较缓存构型，恢复主线后完成直通/条带与回放。"""
import argparse
import os
from pathlib import Path
import subprocess
import sys

MAIN = Path(__file__).resolve().parents[2]
ROOT = MAIN.parent
APP = MAIN/'sw/apps/game/gfx2dGame'
LOGS = MAIN/'sim/engine/g4/logs'
BOARD = Path(__file__).with_name('board_run.py')


def case(name, binary, seconds, port, bit=None, inputs=None, expected_fail=0):
    args = [sys.executable, str(BOARD), '--bin', str(binary), '--port', port,
            '--seconds', str(seconds), '--stop-on', 'done', '--log', str(LOGS/(name+'.log'))]
    args += ['--bit', str(bit)] if bit else ['--no-program']
    if inputs:
        args += ['--input-script', inputs, '--input-script-after', 'mode 2 enter']
    print('G4 BOARD START '+name, flush=True)
    p = subprocess.run(args, cwd=ROOT, capture_output=True)
    (LOGS/(name+'_control.log')).write_bytes(p.stdout+p.stderr)
    if p.returncode:
        raise RuntimeError(name+' board_run failed; see control log')
    text = (LOGS/(name+'.log')).read_text(encoding='utf-8',errors='replace')
    # 程序横幅以前可能是 Flash 自启动的旧程序。
    marker = text.rfind('# --- 程序已加载')
    if marker < 0 or '# serial lost' in text or ('fail '+str(expected_fail)) not in text[marker:] or 'done' not in text[marker:]:
        raise RuntimeError(name+' fail/done/provenance mismatch')
    print('G4 BOARD DONE '+name, flush=True)


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('phase', choices=('16k-follow','16k-cache','4k-perf','replay'))
    ap.add_argument('--port', default='COM11')
    a=ap.parse_args()
    LOGS.mkdir(parents=True,exist_ok=True)
    binary=lambda obj,perf=False: APP/obj/('gfx2dGame-O2-perf.bin' if perf else 'gfx2dGame-O2.bin')
    candidate=MAIN/'sim/engine/g4/build/cache16/gfx2d_ti60/outflow/gfx2d_ti60.bit'
    if a.phase=='16k-follow':
        case('compare_samples_16k',binary('build_g4_compare_samples'),420,a.port,candidate)
        case('s1_strip_16k',binary('build_g4_s1',True),180,a.port)
        case('strip_diagnostics_16k',MAIN/'sw/apps/test/stripTest/build/stripTest-O2.bin',120,a.port)
        case('ddrAliasTest_16k',MAIN/'sw/apps/probe/ddrAliasTest/build/ddrAliasTest.bin',120,a.port)
    if a.phase in ('16k-follow','16k-cache'):
        case('cache_coherency_16k',MAIN/'sw/apps/test/cacheCoherencyTest/build_g4_fixed/cacheCoherencyTest-O2.bin',120,a.port,
             candidate if a.phase=='16k-cache' else None)
        case('ddrBurstProbe_guard',MAIN/'sw/apps/probe/ddrBurstProbe/build/ddrBurstProbe.bin',120,a.port,expected_fail=1)
    elif a.phase=='4k-perf':
        bit=MAIN/'fpga/gfx2d_ti60/outflow/gfx2d_ti60.bit'
        case('nmax_tex_4k',binary('build_g4_nmax_tex',True),1800,a.port,bit)
        case('s1_strip_4k',binary('build_g4_s1',True),180,a.port)
        case('game_strip_4k',binary('build_g4_game',True),180,a.port,
             inputs='1200:64,2400:0,4500:64,5700:0,7500:64,8700:0,10000:64,11200:0')
        case('inject_skip_invalidate',APP/'build_g4_inject/gfx2dGame-O2-inject.bin',180,a.port)
    else:
        prior = LOGS/'game_strip_4k.log'
        if prior.exists():
            prior.replace(LOGS/'game_mode_input_4k.log')
        case('game_strip_4k',binary('build_g4_game',True),180,a.port)
        case('game_switch_4k',binary('build_g4_game',True),180,a.port,
             inputs='1200:64,2400:0,4500:64,5700:0,7500:64,8700:0,10000:64,11200:0')
        case('cache_coherency_4k',MAIN/'sw/apps/test/cacheCoherencyTest/build_g4_fixed/cacheCoherencyTest-O2.bin',120,a.port)
        case('cache_inject_4k',MAIN/'sw/apps/test/cacheCoherencyTest/build_g4_inject/cacheCoherencyTest-O2-inject.bin',120,a.port)
        case('cache_selftest_4k',MAIN/'sw/apps/test/cacheCoherencyTest/build_g4_selftest/cacheCoherencyTest-O2-inject.bin',120,a.port,expected_fail=4)
        case('replay_strip_4k',binary('build_g4_replay'),10800,a.port,
             MAIN/'fpga/gfx2d_ti60/outflow/gfx2d_ti60.bit')


if __name__=='__main__':
    main()
