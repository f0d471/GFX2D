#!/usr/bin/env python3
"""同一功能 BIN 在七代表档部署，逐案验证独立 CRC、错误三元组与收尾。"""
import argparse
import json
import os
from pathlib import Path
import re
import shlex
import struct
import sys
import zlib

from check_board_smoke import uart_port
from check_run import IncompleteCheck, ROOT, command, session, write_json
import feature_config as fc
from feature_synthesis import collect


def expected_crc(name):
    src = (96, 64, 32)
    dst = (16, 32, 48)
    rounded = lambda n: (n + 127) // 255
    if name == 'fill':
        color = 0x00a0b0c0
    elif name == 'alpha':
        rgb = [min(255, s + rounded(d * 127)) for s, d in zip(src, dst)]
        color = (rgb[0] << 16) | (rgb[1] << 8) | rgb[2]
    elif name == 'add':
        rgb = [min(255, s + d) for s, d in zip(src, dst)]
        color = (rgb[0] << 16) | (rgb[1] << 8) | rgb[2]
    elif name in ('const', 'mul', 'straight'):
        rgb = [rounded(s * 64 + d * 191) if name == 'const' else
               rounded(s * d) if name == 'mul' else rounded(s * 128 + d * 127)
               for s, d in zip(src, dst)]
        color = (rgb[0] << 16) | (rgb[1] << 8) | rgb[2]
    elif name in ('key', 'opcode', 'inst-absent'):
        color = 0x00102030
    else:
        color = 0x00604020
    width, stride, height = (640, 1024, 16) if name in ('pb', 'strip') else (64, 64, 4)
    words = [0x00deadbe if x >= width else color if 1 <= x < 7 and y < 3
             else 0x00102030 for y in range(height) for x in range(stride)]
    return zlib.crc32(struct.pack('<' + 'I' * len(words), *words))


def validate_capture(body, expected_caps, expected_strip):
    boundary = '# --- 程序已加载，以下才是它的输出 ---'
    if boundary not in body:
        raise RuntimeError('缺少本次程序加载边界')
    body = body.split(boundary, 1)[1]
    if '# serial lost' in body or 'FAIL' in body:
        raise RuntimeError('串口丢失或功能断言失败')
    if re.findall(r'^fail \d+\s*$', body, re.M) != ['fail 0'] or not re.search(r'^done\s*$', body, re.M):
        raise RuntimeError('缺少唯一正常结束行')
    caps_rows = re.findall(r'^caps ([0-9a-f]{8})\s*$', body, re.M)
    if caps_rows != [f'{expected_caps:08x}']:
        raise RuntimeError('运行时 CAPS 与本轮构型不符')
    expected = {'fill', 'copy', 'alpha', 'add', 'const', 'mul', 'straight',
                'key', 'sequence', 'opcode', 'recovery'}
    expected.add('instance' if expected_caps & (1 << 12) else 'inst-absent')
    if expected_strip:
        expected.add('strip')
    elif 'STRIP absent ok' not in body:
        raise RuntimeError('缺少无 STRIP 的驱动拒绝判据')
    if expected_caps & (1 << 6):
        expected.add('tex')
    elif 'TEX absent ok' not in body:
        raise RuntimeError('缺少无 TEX 的驱动拒绝判据')
    if expected_caps & (1 << 13):
        expected.add('pb')
    elif 'PB absent ok' not in body:
        raise RuntimeError('缺少无 PB 的范围声明')
    rows = re.findall(r'^CASE ([\w-]+) crc ([0-9a-f]{8}) code ([0-9a-f]{8}) '
                      r'index ([0-9a-f]{8}) tag ([0-9a-f]{8}) bad ([0-9a-f]{8})\s*$', body, re.M)
    if len(rows) != len(expected) or {r[0] for r in rows} != expected:
        raise RuntimeError('功能行数或唯一性不符')
    for name, crc, code, index, tag, bad in rows:
        wanted_code = 1 if name in ('opcode', 'inst-absent') else 0
        if int(crc, 16) != expected_crc(name) or int(code, 16) != wanted_code or int(bad, 16):
            raise RuntimeError('独立 CRC / 错误码 / 保护字不符: ' + name)
        if wanted_code and (int(index, 16) != 0 or int(tag, 16) != 0x5a123456):
            raise RuntimeError('错误三元组不符: ' + name)
    return {'status': 'PASS', 'cases': len(rows), 'caps': f'0x{expected_caps:08X}'}


def parser_negatives(body, caps, expected_strip):
    first = re.search(r'^CASE .*$', body, re.M)[0]
    variants = {
        'missing-boundary': body.replace('# --- 程序已加载，以下才是它的输出 ---', ''),
        'serial-loss': body + '\n# serial lost\n',
        'duplicate-case': body + '\n' + first + '\n',
        'missing-case': body.replace(first, ''),
        'nonzero-fail': body.replace('fail 0', 'fail 1'),
        'wrong-crc': re.sub(r'(?m)^(CASE \w+ crc )[0-9a-f]{8}', r'\g<1>ffffffff', body, count=1),
        'wrong-error-tag': re.sub(r'(?m)^(CASE opcode .* tag )[0-9a-f]{8}', r'\g<1>00000000', body),
        'wrong-error-code': re.sub(r'(?m)^(CASE opcode .* code )[0-9a-f]{8}', r'\g<1>00000000', body),
        'wrong-caps': re.sub(r'(?m)^caps [0-9a-f]{8}', 'caps 00000000', body),
    }
    detected = []
    for name, changed in variants.items():
        if changed == body:
            raise RuntimeError('解析器反例未改变输入: ' + name)
        try:
            validate_capture(changed, caps, expected_strip)
        except RuntimeError:
            detected.append(name)
        else:
            raise RuntimeError('板测解析器漏判: ' + name)
    try:
        validate_capture(body,caps,not expected_strip)
    except RuntimeError:
        detected.append('wrong-strip-configuration')
    else:
        raise RuntimeError('板测解析器漏判 STRIP 构型')
    return detected


def main():
    current = session()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tree-root', help='默认使用本轮 feature-synthesis 生成的七档源树')
    parser.add_argument('--only', nargs='+', choices=tuple(fc.PRESETS))
    parser.add_argument('--port')
    args = parser.parse_args()
    if os.name != 'nt':
        raise IncompleteCheck('构型板测使用 Windows Efinity Python')
    root = Path(args.tree_root).resolve() if args.tree_root else current.build('feature-presets')
    logs = current.logs('feature-board')
    app = ROOT / 'Main/sw/apps/test/featureTest'
    build = current.build('feature-board') / 'app'
    relative = os.path.relpath(build, app).replace('\\', '/')
    script = ('toolchain="$(find "$HOME/opt" -path "*/bin/riscv-none-elf-gcc" -print -quit)"; '
              'test -n "$toolchain" || exit 3; make -C Main/sw/apps/test/featureTest -j8 '
              'OBJDIR=' + shlex.quote(relative) + ' RISCV_BIN="${toolchain%gcc}"')
    command(['wsl.exe', '--exec', 'bash', '-c', script], logs / 'build.log')
    binary = build / 'featureTest-O2.bin'
    current.artifact(binary)
    port = args.port or uart_port()
    rows = []
    for preset in args.only or tuple(fc.PRESETS):
        tree = root / preset
        record = json.loads((tree / 'config.json').read_text(encoding='utf-8'))
        collect(tree)
        bit = tree / 'fpga/gfx2d_ti60/outflow/gfx2d_ti60.bit'
        inputs = [* (tree / 'rtl').rglob('*.v'), * (tree / 'rtl').rglob('*.vh'),
                  * (tree / 'fpga/gfx2d_ti60/sdc').glob('*.sdc')]
        if not bit.exists() or bit.stat().st_mtime_ns < max(p.stat().st_mtime_ns for p in inputs):
            raise IncompleteCheck('位流早于本轮构型源码: ' + preset)
        current.artifact(bit)
        output = logs / (preset + '.serial.log')
        control = command([sys.executable, ROOT / 'Main/tools/board/board_run.py',
                           '--bit', bit, '--bin', binary, '--port', port,
                           '--seconds', '90', '--stop-line', 'done', '--log', output],
                          logs / (preset + '.control.log'), timeout=300)
        if str(binary.stat().st_size) + ' bytes written at address' not in control:
            raise RuntimeError('缺少本次程序写入字节数: ' + preset)
        body = output.read_text(encoding='utf-8-sig')
        result = validate_capture(body, int(record['caps'], 16), bool(record['flags']['STRIP']))
        if not rows:
            write_json(logs / 'parser-negatives.json', parser_negatives(body, int(record['caps'], 16),
                                                                       bool(record['flags']['STRIP'])))
        rows.append({'preset': preset, 'port': port, **result})
        write_json(logs / 'result.json', {'variants': rows, 'complete': False})
        print('FEATURE BOARD PASS ' + preset + ' cases=' + str(result['cases']), flush=True)
    current.verify()
    write_json(logs / 'result.json', {'variants': rows, 'complete': True, 'profile': current.profile})
    print('FEATURE BOARD PASS presets=' + str(len(rows)), flush=True)


if __name__ == '__main__':
    try:
        main()
    except IncompleteCheck as error:
        print('INCOMPLETE ' + str(error), file=sys.stderr)
        sys.exit(3)
