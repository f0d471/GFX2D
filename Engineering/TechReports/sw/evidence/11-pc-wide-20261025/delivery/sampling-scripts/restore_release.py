import hashlib,json,os,re,subprocess,sys,time
from pathlib import Path
folder=Path(__file__).resolve().parent;root=folder.parents[3]
board_run=root/'Main/sim/build/check-runs/20261025-184825-fcfcb370'
passed=json.loads((board_run/'logs/r4-board/result.json').read_text())
assert passed['status']=='PASS'
bit=root/passed['b7_tree']/'fpga/gfx2d_ti60/outflow/gfx2d_ti60.bit'
binary=folder/'target-release/gfx2dGame-O2-perf.bin'
out=folder/('board-release-'+time.strftime('%Y%m%d-%H%M%S'));out.mkdir()
serial=out/'serial.log'
command=[sys.executable,str(root/'Main/tools/board/board_run.py'),'--bit',str(bit),
    '--bin',str(binary),'--port',passed['port'],'--seconds','120','--stop-on','R4STAT ',
    '--log',str(serial)]
with (out/'deploy.log').open('w',encoding='utf-8') as log:
    result=subprocess.run(command,cwd=root,stdout=log,stderr=subprocess.STDOUT)
text=serial.read_text(encoding='utf-8-sig')
control=(out/'deploy.log').read_text(encoding='utf-8')
boundary='# --- 程序已加载，以下才是它的输出 ---'
assert result.returncode==0 and boundary in text
live=text.split(boundary,1)[1]
assert str(binary.stat().st_size)+' bytes written at address' in control
rows=re.findall(r'^R4STAT ([\d ]+)\s*$',live,re.M)
assert rows and list(map(int,rows[-1].split()))[3:]==[2,1,0]
assert 'r4 enter ladder/hw 2 1' in live and '# serial lost' not in live
summary=dict(status='PASS',port=passed['port'],deployment='JTAG only; no Flash write',
    bit_sha256=hashlib.sha256(bit.read_bytes()).hexdigest(),
    bin_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),bin_bytes=binary.stat().st_size,
    state=list(map(int,rows[-1].split())),scope='unlimited mode4/L2/HW startup; distinct from finite performance BIN')
(out/'result.json').write_text(json.dumps(summary,indent=2)+'\n')
print('BOARD RELEASE RUNNING '+str(out),flush=True)
print(json.dumps(summary),flush=True)
