from pathlib import Path
import json
import sys
import time
import serial

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[4]
package = ROOT / 'Main/bit/board_release_console_20261028'
sys.path.insert(0, str(package / 'tools/board'))
from game_control import BoardState

state = BoardState()
raw = bytearray()
with serial.Serial('COM11', 115200, timeout=.2) as port:
    until = time.monotonic() + 8
    while time.monotonic() < until:
        raw.extend(port.read(8192))
(HERE / 'frozen-panel.serial.log').write_bytes(raw)
for line in raw.decode('utf-8', errors='replace').splitlines():
    state.consume(line)
result = dict(parser='frozen package BoardState', port='COM11', protocol=state.protocol,
              mode=state.mode, backend=state.backend, fps=state.fps, errors=state.errors,
              capture_closed=True, observation_time=time.strftime('%Y-%m-%d %H:%M:%S'))
(HERE / 'frozen-panel.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
assert state.protocol == 2 and state.mode == 'menu' and state.backend == 'hw', result
assert state.errors == 0 and state.fps is not None and 55 <= state.fps <= 65, result
print(json.dumps(result))
