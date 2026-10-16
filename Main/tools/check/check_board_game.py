#!/usr/bin/env python3
"""当前主工程位流上的生产 HW/DDR 游戏短运行；不重搜性能或重跑关卡录像。"""
import os
from pathlib import Path
import re
import shlex
import struct
import sys

from check_run import IncompleteCheck, ROOT, command, session, write_json
from check_board_smoke import uart_port


def main():
    current = session()
    if os.name != 'nt':
        raise IncompleteCheck('生产游戏板测使用 Windows Efinity Python')
    logs = current.logs('board-game')
    build = current.build('board-game') / 'app'
    app = ROOT / 'Main/sw/apps/game/gfx2dGame'
    relative = os.path.relpath(build, app).replace('\\', '/')
    extra = '-DGFX2D_START_MODE=2 -DGFX2D_START_BACKEND=1 -DGFX2D_FRAMES=240'
    script = ('toolchain="$(find "$HOME/opt" -path "*/bin/riscv-none-elf-gcc" -print -quit)"; '
              'test -n "$toolchain" || exit 3; make -C Main/sw/apps/game/gfx2dGame -j8 '
              'PERF=1 OBJDIR=' + shlex.quote(relative) + ' EXTRA_CFLAGS=' + shlex.quote(extra)
              + ' RISCV_BIN="${toolchain%gcc}"')
    command(['wsl.exe', '--exec', 'bash', '-c', script], logs / 'build.log')
    binary = build / 'gfx2dGame-O2-perf.bin'
    bit = ROOT / 'Main/fpga/gfx2d_ti60/outflow/gfx2d_ti60.bit'
    pak = ROOT / 'Main/assets/build/assets.pak'
    sources = [*(ROOT / 'Main/rtl').rglob('*.v'), *(ROOT / 'Main/rtl').rglob('*.vh'),
               *(ROOT / 'Main/fpga/gfx2d_ti60/sdc').glob('*.sdc')]
    if not bit.exists() or bit.stat().st_mtime_ns < max(p.stat().st_mtime_ns for p in sources):
        raise IncompleteCheck('主工程位流早于源码；先综合')
    for artifact in (binary, bit, pak): current.artifact(artifact)
    port = uart_port()
    control = command([sys.executable, ROOT / 'Main/tools/board/board_run.py',
                       '--bit', bit, '--bin', binary, '--port', port, '--seconds', '120',
                       '--stop-line', 'done', '--log', logs / 'serial.log'],
                      logs / 'control.log', timeout=300)
    if str(binary.stat().st_size) + ' bytes written at address' not in control:
        raise RuntimeError('缺少本次生产程序写入字节数')
    body = (logs / 'serial.log').read_text(encoding='utf-8-sig')
    boundary = '# --- 程序已加载，以下才是它的输出 ---'
    if boundary not in body: raise RuntimeError('缺少本次加载边界')
    current_body = body.split(boundary, 1)[1]
    if '# serial lost' in current_body or re.search(r'^(?:err |engine err |fail [1-9])', current_body, re.M):
        raise RuntimeError('本次游戏报错或串口丢失')
    with pak.open('rb') as handle: header = handle.read(32)
    total, pack_crc = struct.unpack_from('<II', header, 16)
    if f'pak {total} {pack_crc:08x}' not in current_body:
        raise RuntimeError('实际 Flash 素材与当前素材包头不符')
    scenes = re.findall(r'^scene (\d+) scrc ([0-9a-f]{8})\s*$', current_body, re.M)
    if len(scenes) != 3 or {int(row[0]) for row in scenes} != {0, 1, 2}:
        raise RuntimeError('缺少三组启动场景检查')
    if 'mode 2 enter frame 0' not in current_body:
        raise RuntimeError('未进入生产游戏模式')
    rows = [line.split(',') for line in current_body.splitlines() if line.startswith('csv,')]
    if not rows or any(row[2:4] != ['game', 'hw'] for row in rows):
        raise RuntimeError('游戏未持续使用 HW/DDR')
    sums = re.findall(r'^csvsum,([^\r\n]+)', current_body, re.M)
    if len(sums) != 1 or int(sums[0].split(',')[0]) != 240:
        raise RuntimeError('不是完整 240 帧生产运行')
    command([sys.executable, ROOT / 'Main/tools/check/check_sw9_perf.py', logs / 'serial.log',
             '--fields-min', '240', '--warmup-rows', '1', '--steady-drops-max', '0'],
            logs / 'validation.log')
    write_json(logs / 'result.json', {'status': 'PASS', 'frames': 240, 'backend': 'HW/DDR',
               'port': port, 'startup_scene_checks': scenes,
               'scope': 'startup scene checks and 240 production frames; no per-frame replay or N_max search'})
    print('BOARD GAME PASS frames=240 backend=HW/DDR port=' + port, flush=True)


if __name__ == '__main__':
    try:
        main()
    except IncompleteCheck as error:
        print('INCOMPLETE ' + str(error), file=sys.stderr)
        sys.exit(3)
