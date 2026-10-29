from pathlib import Path
import hashlib,json,re,subprocess
R=Path(__file__).resolve().parents[4]
entries=[('console','Main/sw/apps/console/build_sw16_target/console-O2-perf.bin','target-cross-final.log'),
         ('sdk-pixels','Main/sw/apps/examples/pixels/build_sw16_target/sdk-pixels-O2.bin','sdk-target-cross.log')]
items=[]
for name,rel,log in entries:
    p=R/rel;body=(R/'Main/sim/build/sw16'/log).read_text()
    if not p.is_file() or 'error:' in body or not re.search(r'ram:\s+\d+ B',body):raise RuntimeError('Missing successful cross build: '+name)
    items.append(dict(name=name,path=rel,bytes=p.stat().st_size,sha256=hashlib.sha256(p.read_bytes()).hexdigest(),
        ram_bytes=int(re.search(r'ram:\s+(\d+) B',body)[1])))
source=['Main/sw/ports/ti60/app_port.c','Main/sw/rtos/rtos_irq.c','Main/sw/bsp/ti60/board.c',
        'Main/sw/bsp/ti60/gfx_engine.c','Main/sw/mk/target.mk','Main/sw/firmware/console/FreeRTOSConfig.h']
value=dict(status='PASS',scope='PC cross compilation and link only',board=False,flash=False,
    outputs=items,sources={x:hashlib.sha256((R/x).read_bytes()).hexdigest() for x in source})
(R/'Main/sim/build/sw16/cross-result.json').write_text(json.dumps(value,indent=2)+'\n')
print('CROSS BUILD EVIDENCE PASS',[(x['name'],x['bytes']) for x in items])
