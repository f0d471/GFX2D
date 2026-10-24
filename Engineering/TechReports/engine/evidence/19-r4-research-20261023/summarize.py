"""Audit saved research captures and produce the one archival research summary."""
import hashlib,json,math,re,statistics,subprocess,sys
from pathlib import Path
ROOT=Path.cwd();OUT=Path(__file__).resolve().parent
def read(name):return (OUT/name).read_text(encoding='utf-8-sig')
def rows(kind,text):return [[int(x) for x in s.split()] for s in re.findall('^'+kind+r' (.+)$',text,re.M)]
def audit(body):
    marker='# --- 程序已加载，以下才是它的输出 ---'
    if marker not in body or '# serial lost' in body:raise ValueError('capture boundary/loss')
    body=body.split(marker,1)[1]
    if re.findall(r'^fail (\d+)\s*$',body,re.M)!=['0'] or not re.search(r'^done\s*$',body,re.M):raise ValueError('completion')
    if rows('CAPS',body)!=[[0xd46f]]:raise ValueError('B7 capability')
    for k in ['WORK','CRC']:
        if rows(k,body)!=rows(k,read('host.log')):raise ValueError(k+' host/target mismatch')
    samples=rows('SAMPLE',body);cad=rows('CADENCE',body);health=rows('HEALTH',body)
    if len(samples)!=24 or len(cad)!=120 or [r[0] for r in cad]!=list(range(120)):raise ValueError('missing or duplicate rows')
    if any(b[2]-a[2]!=2 or b[4]-a[4]!=1 for a,b in zip(cad,cad[1:])):raise ValueError('cadence schedule/swap sequence')
    if len(health)!=1 or any(health[0][2:]):raise ValueError('display error')
    return body,samples,cad
body,samples,cad=audit(read('serial.log'))
negative={
    'boundary':read('serial.log').replace('# --- 程序已加载，以下才是它的输出 ---',''),
    'lost':read('serial.log')+'\n# serial lost\n',
    'missing-frame':read('serial.log').replace(re.search(r'^CADENCE 50 .*$',read('serial.log'),re.M)[0],''),
    'crc':read('serial.log').replace('CRC 0 1 2781210212','CRC 0 1 2781210213'),
    'health':read('serial.log').replace('HEALTH 3816 347 0 0 0','HEALTH 3816 347 0 1 0'),
}
for label,text in negative.items():
    try:audit(text)
    except ValueError:continue
    raise RuntimeError('accepted corrupted capture: '+label)
summary=[]
for dense,count,*_ in rows('WORK',body):
    selected=[r for r in samples if r[:2]==[dense,count]]
    cpu=statistics.median(r[3] for r in selected);hw=statistics.median(r[4] for r in selected)
    summary.append(dict(dense=bool(dense),count=count,cpu_ms=cpu/100000,
        submit_wait_ms=hw/100000,combined_ms=(cpu+hw)/100000,
        rast_ms=statistics.median(r[5] for r in selected)/100000))
times=sorted(r[1] for r in cad)
data=dict(status='PASS',scope='research sampling only; no R4 implementation or acceptance',
    sample_medians=summary,cadence=dict(frames=120,p50_ms=times[59]/100000,
        p99_ms=times[118]/100000,max_ms=times[-1]/100000,
        exact_due=sum(r[2]==r[3] for r in cad),late=sum(r[3]>r[2] for r in cad),
        over_33_333ms=sum(r[1]>3333333 for r in cad)),
    parser_corruptions_rejected=list(negative),crc_cases=8,
    exclusions=['actual scene and game logic','actual cup model','L2 texture load in this probe',
                'full HUD and mode switches','60-second performance acceptance','Flash','human HDMI review'])
(OUT/'summary.json').write_text(json.dumps(data,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
asm=read('target/r4Probe-O2.asm')
begin=asm.index(' <fx16_round_div>:')
end=asm.index(' <fx16_sat>:',begin)
(OUT/'round-div-disassembly.txt').write_text(asm[begin:end],encoding='utf-8')
print(json.dumps(data,ensure_ascii=False,indent=2))
