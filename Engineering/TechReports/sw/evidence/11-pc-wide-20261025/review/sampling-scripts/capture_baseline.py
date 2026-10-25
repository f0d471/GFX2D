import json, os, re, subprocess
from pathlib import Path

root = Path(__file__).resolve().parents[4]
base = Path(__file__).resolve().parent / 'baseline'
app = root / 'Main/sw/apps/game/gfx2dGame'
result = {}
for name, ladder, scene in [('l1', 1, 0), ('l2', 2, 0), ('cup', 1, 1)]:
    out = base / ('host-' + name)
    flags = (f'-DGFX2D_START_MODE=4 -DGFX2D_FRAMES=60 -DGFX2D_R4_LADDER={ladder}'
             f' -DGFX2D_R4_SCENE={scene} -DGFX2D_R4_HW=0 -DGFX2D_R4_JUDGE=1')
    with (base / (name + '.build.log')).open('w') as log:
        subprocess.run(['make', '-C', str(app), '-j8', 'PLATFORM=host', 'PERF=1',
                        'OBJDIR=' + str(out), 'EXTRA_CFLAGS=' + flags], stdout=log,
                       stderr=subprocess.STDOUT, check=True)
    body = subprocess.check_output([str(out / 'gfx2dGame-perf')], stderr=subprocess.STDOUT,
        env={**os.environ, 'SDL_VIDEODRIVER': 'dummy', 'SDL_AUDIODRIVER': 'dummy', 'GFX2D_UNPACED': '1'}).decode()
    (base / (name + '.log')).write_text(body)
    rows = re.findall(r'^R4CAD (\d+) .*? ([0-9a-f]{8}) ([0-9a-f]{8})\s*$', body, re.M)
    assert len(rows) == 60 and re.findall(r'^fail (\d+)\s*$', body, re.M) == ['0']
    result[name] = rows
    print('BASELINE PASS', name, len(rows), flush=True)
(base / 'frames.json').write_text(json.dumps(result, indent=2) + '\n')
