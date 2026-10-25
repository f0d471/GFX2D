import hashlib, json, sys
from pathlib import Path

folder = Path(__file__).resolve().parent
destination = folder / (sys.argv[1] if len(sys.argv)>1 else 'fixed-current')
destination.mkdir(exist_ok=False)
source = (folder / 'capture_baseline.py').read_text()
source = source.replace("base = Path(__file__).resolve().parent / 'baseline'",
                        "base = Path(__file__).resolve().parent / " + repr(destination.name))
namespace = {'__file__': str(folder / 'capture_baseline.py')}
exec(compile(source, str(folder / 'capture_baseline.py'), 'exec'), namespace)
before = json.loads((folder / 'baseline/frames.json').read_text())
after = json.loads((destination / 'frames.json').read_text())
if before != after:
    raise RuntimeError('fixed canvas pixel or record CRC changed')
identities = {}
for stage in ('baseline', destination.name):
    identities[stage] = {}
    for scene in ('l1', 'l2', 'cup'):
        binary = folder / stage / ('host-' + scene) / 'gfx2dGame-perf'
        identities[stage][scene] = dict(bytes=binary.stat().st_size,
            sha256=hashlib.sha256(binary.read_bytes()).hexdigest())
result = dict(status='PASS', frames_per_scene=60, scenes=list(before),
              compared=['frame number', 'full-frame pixel CRC', '128B record CRC'],
              parameters='PERF=1 START_MODE=4 R4_HW=0 R4_JUDGE=1 FRAMES=60; no replay override',
              binaries=identities)
(destination / 'comparison.json').write_text(json.dumps(result, indent=2) + '\n')
print('FIXED BASELINE PASS 180 frames: pixels and setup records unchanged', flush=True)
