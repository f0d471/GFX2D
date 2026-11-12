"""Inspect production APB and memory while briefly halted; no Flash writes."""
import argparse
from contextlib import redirect_stdout, redirect_stderr
import hashlib
import json
from pathlib import Path
import re
import sys

ROOT = next(p for p in Path(__file__).resolve().parents
            if (p/'Main/tools/board/board_run.py').is_file())
sys.path.insert(0, str(ROOT / 'Main/tools/board'))
import board_run


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--firmware', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--attached', action='store_true')
    args = ap.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    binary = args.firmware.resolve()
    symbols = {}
    for line in (binary.parent / 'symbols.txt').read_text().splitlines():
        parts = line.split()
        if len(parts) == 3:
            symbols[parts[2]] = int(parts[0], 16)
    needed = ['gfx_engine_submit_raster', 's_pipe_cur', 's_pipe_rec', 's_pipe_buf']
    assert all(n in symbols for n in needed)
    original_ocd = board_run.run_openocd
    def fast_ocd(ocd, cfg, workspace, commands, timeout=90):
        return original_ocd(ocd, cfg, workspace, ['adapter speed 6000', *commands], timeout=timeout)
    board_run.run_openocd = fast_ocd
    saved_argv = sys.argv
    sys.argv = ['board_run',
        '--bin', str(binary), '--no-program', '--precise-boundary', '--port', 'COM11',
        '--seconds', '35', '--input-script', '300:16384,700:17',
        '--input-script-after', 'console enter menu', '--log', str(out/'launch.serial.log')]
    try:
        if not args.attached:
            with (out/'launch.loader.log').open('w',encoding='utf-8') as output:
                with redirect_stdout(output), redirect_stderr(output):
                    deploy_status=board_run.main()
    finally:
        sys.argv=saved_argv
        board_run.run_openocd=original_ocd
    if not args.attached:
        assert deploy_status == 0
        body = (out/'launch.serial.log').read_text(encoding='utf-8')
        assert 'console enter sh' in body
    ocd = board_run.find_openocd(board_run.find_efinity())
    cfg = str(ROOT/'Main/tools/board/gfx2d_debug.cfg')
    commands = ['adapter speed 6000', 'halt']
    for i in range(16):
        commands += ['resume', 'sleep 83', 'halt',
            'set params [read_memory 0xf8101008 32 4]',
            'set status [lindex [read_memory 0xf810000c 32 1] 0]',
            f'set cur [lindex [read_memory 0x{symbols["s_pipe_cur"]:x} 32 1] 0]',
            f'set records [read_memory 0x{symbols["s_pipe_rec"]:x} 32 2]',
            'set front [lindex [read_memory 0xf810e00c 32 1] 0]',
            'set back [lindex [read_memory 0xf810e010 32 1] 0]',
            'set dst [expr {[lindex [read_memory 0xf8100034 32 1] 0]&15}]',
            'set dstbase [lindex [read_memory [expr {0xf8100040+12*$dst}] 32 1] 0]',
            f'echo [format "SW23PROBE {i} %u %08x %08x %08x %08x %u %u %08x %08x %08x %08x" $cur [lindex $records 0] [lindex $records 1] [lindex $params 0] [lindex $params 1] [lindex $params 3] $dst $dstbase $front $back $status]',
        ]
    commands += ['resume', 'shutdown']
    ok, output = board_run.run_openocd(ocd, cfg, str(ROOT/'Main/sdk'), commands, timeout=60)
    (out/'apb.log').write_text(output, encoding='utf-8')
    rows = []
    for match in re.finditer(r'(?m)^SW23PROBE (\d+) (\d+) ([0-9a-f]+) ([0-9a-f]+) ([0-9a-f]+) ([0-9a-f]+) (\d+) (\d+) ([0-9a-f]+) ([0-9a-f]+) ([0-9a-f]+) ([0-9a-f]+)\s*$', output):
        g = match.groups()
        row = dict(index=int(g[0]), cur=int(g[1]), record_a=int(g[2],16), record_b=int(g[3],16),
            tri_base=int(g[4],16), sub_base=int(g[5],16), tri_count=int(g[6]), dst=int(g[7]),
            dst_base=int(g[8],16), front_base=int(g[9],16), back_base=int(g[10],16), status=int(g[11],16))
        use_b=row['sub_base'] in (row['record_a']+0x3e0000,row['record_a']+0x3f0000)
        row['subtable_buffer']='B' if use_b else 'A'
        base = row['record_b'] if use_b else row['record_a']
        row['wrong_record_buffer'] = not base <= row['tri_base'] < base + 2048*128
        row['destination_is_front'] = row['dst_base'] == row['front_base']
        row['busy_front_seen'] = bool(row['status'] & 1) and row['destination_is_front']
        rows.append(row)
    bit = ROOT/'Main/bit/snapshots/B7/gfx2d_ti60.bit'
    sources = ['Main/sw/ports/myriad/r4_runtime.c', 'Main/sw/bsp/ti60/gfx_raster.c',
        'Main/sw/bsp/ti60/board.c', 'Main/rtl/display/display_fetch.v']
    result = dict(scope='Unmodified production BIN; halt/read/resume perturbs timing; no performance or HDMI observation claim',
        binary=str(binary.relative_to(ROOT)), sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
        bitstream=dict(path=bit.relative_to(ROOT).as_posix(),sha256=hashlib.sha256(bit.read_bytes()).hexdigest()),
        source_sha256={n:hashlib.sha256((ROOT/n).read_bytes()).hexdigest() for n in sources},
        symbols={n:hex(symbols[n]) for n in needed}, target_examined=ok, samples=rows,
        wrong_record_buffer=any(r['wrong_record_buffer'] for r in rows),
        destination_is_front_seen=any(r['destination_is_front'] for r in rows),
        busy_front_seen=any(r['busy_front_seen'] for r in rows))
    (out/'result.json').write_text(json.dumps(result, indent=2)+'\n', encoding='utf-8')
    print(json.dumps({k:v for k,v in result.items() if k not in ('samples','source_sha256','symbols')}, indent=2))
    print('samples='+str(len(rows)))
    assert ok and len(rows)==16, 'incomplete APB capture'


if __name__ == '__main__':
    main()
