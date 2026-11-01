#!/usr/bin/env python3
"""Bounded software canvases, analytic coverage, projection and atomic rejection."""
import os
import subprocess
import sys
from check_run import ROOT, IncompleteCheck, command, session, write_json


def main():
    if os.name == 'nt':
        raise IncompleteCheck('pc-wide numerical checks use WSL; pc-window uses the Windows desktop')
    current = session()
    logs, out = current.logs('pc-wide'), current.build('pc-wide')
    sources = ['Main/sim/host/test_wide.c',
        'Main/sw/apps/game/gfx2dGame/src/r4_scene.c',
        'Main/sw/apps/game/gfx2dGame/src/r4_geometry.c',
        'Main/sw/lib/gfx/rast_setup.c', 'Main/sw/lib/gfx/rast_ref.c',
        'Main/sw/lib/core/fixmath.c', 'Main/sw/lib/core/sin_tab.c',
        'Main/sw/lib/core/xorshift32.c', 'Main/sw/lib/core/crc32.c']
    base = ['gcc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
        '-I', 'Main/sw/lib', '-I', 'Main/sw/api', '-I', 'Main/sw/services/gfx', '-I', 'Main/sw/ports/myriad', '-I', 'Main/sw/bsp/include',
        '-I', 'Main/sw/apps/game/gfx2dGame/src']
    bodies = []
    for name, flags in [('64', ['-fsanitize=address,undefined', '-fno-sanitize-recover=all']),
                        ('32', ['-m32'])]:
        binary = out / ('wide-' + name)
        command([*base, *flags, *sources, '-o', binary], logs / (name + '.build.log'))
        bodies.append(command([binary], logs / (name + '.log')))
        current.artifact(binary)
    if bodies[0] != bodies[1] or 'PC WIDE PASS' not in bodies[0]:
        raise RuntimeError('wide 64/32 outputs differ or incomplete')
    # Mutate a copy: the original failure (clamping at x=639) must be detected
    # by the analytic expected depth/coverage, independently of record validity.
    source = ROOT / 'Main/sw/lib/gfx/rast_setup.c'
    normal = source.read_text()
    old = 'int32_t max = (int32_t)(k ? frame->height : frame->width) - 1;'
    if normal.count(old) != 1:
        raise RuntimeError('width mutation site changed')
    mutant = out / 'rast_setup_fixed_width.c'
    mutant.write_text(normal.replace(old, '(void)frame; int32_t max = k ? RAST_HEIGHT - 1 : RAST_WIDTH - 1;'))
    binary = out / 'wide-fixed-width'
    command([*base, *[mutant if p == 'Main/sw/lib/gfx/rast_setup.c' else p for p in sources],
             '-o', binary], logs / 'mutation.build.log')
    result = subprocess.run([binary], cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, timeout=120)
    body = result.stdout
    (logs / 'mutation.log').write_text(body)
    if result.returncode != 1 or 'WIDE CHECK FAIL depth coverage' not in body:
        raise RuntimeError('fixed-width mutation failed outside the expected coverage check')
    current.verify()
    write_json(logs / 'result.json', dict(status='PASS', widths=[640,768,854,1120,1707,1792],
        checks=['analytic clipped coverage and depth', 'independent bin order and count',
                'stride canaries', 'atomic rejection', 'aspect-correct square projection',
                'world state invariance', '64/32 parity and ASan/UBSan'],
        mutation='EXPECTED_RED: old 640-wide bbox clamp loses wide-region coverage',
        scope='software canvas only; legacy hardware ABI remains 640x480'))
    print('PC WIDE PASS', flush=True)


if __name__ == '__main__':
    try:
        main()
    except IncompleteCheck as error:
        print('INCOMPLETE ' + str(error))
        sys.exit(3)
