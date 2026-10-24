import hashlib,json,re,sys,time
from pathlib import Path
import serial
ROOT=Path.cwd();OUT=Path(__file__).resolve().parent
sys.path.insert(0,str(ROOT/'Main/tools/board'))
from input_forward import packet
cases=[]
for keys in range(1024):
    a,b=packet(keys);decoded=(a&127)|((b&7)<<7)
    if decoded!=keys or b>>3!=((decoded^(decoded>>4)^(decoded>>8))&15):raise RuntimeError('old packet changed')
cases=dict(old_bitmaps_roundtrip=1024,mode4_combo=0x380,mode4_bytes=list(packet(0x380)),
    bit10_bytes=list(packet(1024)),explanation='mode4 is a reserved existing-bit combination; bit10 is discarded')
(OUT/'packet-audit.json').write_text(json.dumps(cases,indent=2)+'\n')
print('passive serial capture, no reset',flush=True)
raw=bytearray();end=time.monotonic()+12
with serial.Serial('COM11',115200,timeout=0.25) as port:
    while time.monotonic()<end:raw.extend(port.read(port.in_waiting or 1))
(OUT/'restore-followup.serial.log').write_bytes(raw)
body=raw.decode('utf-8','replace');print(body[-1600:],flush=True)
if not re.search(r'csv,\d+,game,hw-tex,',body):raise RuntimeError('normal HW/TEX game not yet confirmed')
binary=OUT/'restore/gfx2dGame-O2-perf.bin'
result=dict(status='PASS',scope='restore current production mode2 HW/TEX only, no game acceptance or new performance claim',
    binary=dict(path=binary.relative_to(ROOT).as_posix(),bytes=binary.stat().st_size,
                sha256=hashlib.sha256(binary.read_bytes()).hexdigest()),
    passive_capture_bytes=len(raw),flags='O2 PERF=1 GFX2D_START_BACKEND=2')
(OUT/'restore.json').write_text(json.dumps(result,indent=2)+'\n')
