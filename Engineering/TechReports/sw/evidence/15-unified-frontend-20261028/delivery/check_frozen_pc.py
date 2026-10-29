from pathlib import Path
import os
import re
import subprocess

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[4]
pc = ROOT / 'Main/bit/board_release_console_20261028/pc'
env = {**os.environ, 'SDL_VIDEODRIVER': 'dummy', 'SDL_AUDIODRIVER': 'dummy',
       'GFX2D_UNPACED': '1'}
command = [str(pc / 'gfx2dGame.exe'), '--frames', '80', '--input-script',
           '5:128,6:0,20:256,21:0,35:512,36:0,50:896,51:0,70:2048,71:0']
with (HERE / 'frozen-pc-runtime.log').open('wb') as log:
    process = subprocess.run(command, cwd=pc, env=env, stdout=log,
                             stderr=subprocess.STDOUT, timeout=180)
body = (HERE / 'frozen-pc-runtime.log').read_text(encoding='utf-8', errors='replace')
assert process.returncode == 0, body
assert re.findall(r'console enter (\w+)', body) == ['menu', 'demo', 'stg', 'press', 'r4', 'menu'], body
records = re.findall(r'console complete app (\w+) id (\d+) frames (\d+) presents (\d+) errors (\d+)', body)
assert len(records) == 6, body
assert all(int(frames) > 0 and int(presents) > 0 and int(errors) == 0
           for _, _, frames, presents, errors in records), body
print('FROZEN PC PASS: menu, demo, stg, press, r4, Back; all presented, errors 0')
