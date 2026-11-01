"""Strict, side-effect-free console protocol/evidence validation."""
import re
from feature_config import CAPS_BIT

APP_IDS={'menu':0,'demo':1,'stg':2,'press':3,'r4':4,'about':5}
BAD=re.compile(r'(?m)^(?:console (?:error|fatal|detail)\b|err\b|.*\berr(?:or)?\b|fail [1-9]|# serial lost)')

def require(condition,message):
    if not condition:raise RuntimeError(message)

def fields(line,prefix):
    words=line[len(prefix):].split()
    require(len(words)%2==0,'malformed telemetry: '+line)
    data=dict(zip(words[::2],words[1::2]))
    require(len(data)*2==len(words),'duplicate telemetry keys')
    return data

def integer(data,key):
    require(key in data,'missing telemetry field '+key)
    try:value=int(data[key])
    except ValueError:raise RuntimeError('invalid integer '+key) from None
    require(value>=0,'negative telemetry field '+key)
    return value

def clean(body):
    require(not BAD.search(body),'application/transport reported an error')
    for line in body.splitlines():
        if line.startswith('R4HEALTH '):
            try:values=list(map(int,line.split()[1:]))
            except ValueError:raise RuntimeError('malformed R4 health') from None
            require(len(values)==5 and values[2:]==[0,0,0],'R4 display health failed')

def catalog(body,expected=None,caps=None):
    header=None;entries=[];ended=False
    for line in body.splitlines():
        if line.startswith('console catalog begin '):
            require(header is None,'duplicate catalog header');header=fields(line,'console catalog begin ')
            require(header.get('v')=='1' and re.fullmatch('[0-9a-f]{64}',header.get('hash','')),'invalid catalog header')
        elif line.startswith('console catalog app '):
            require(header is not None and not ended,'catalog entry outside transaction')
            d=fields(line,'console catalog app ')
            require(d.get('key') and d['key'] not in ('menu','fault'),'invalid catalog key')
            require(d.get('kind') in ('game','demo','benchmark','system') and d.get('graphics') in ('2d','3d'),'invalid catalog category')
            require(0<integer(d,'id')<0xffffffff and integer(d,'available') in (0,1) and integer(d,'shortcut') in range(6),'invalid catalog value')
            try:title=bytes.fromhex(d['title']).decode('ascii')
            except (KeyError,ValueError,UnicodeError):raise RuntimeError('invalid catalog title') from None
            require(0<len(title)<=80,'invalid title length')
            require(not any(e['key']==d['key'] or e['id']==d['id'] for e in entries),'duplicate catalog application')
            entries.append(d)
        elif line=='console catalog end':
            require(header is not None and not ended,'invalid catalog end');ended=True
    if header is None:
        require(expected is None,'required application catalog missing');return None
    require(ended and integer(header,'count')==len(entries),'incomplete catalog')
    if expected is not None:
        require(header['hash']==expected['catalog_sha256'],'wrong content identity')
        require([e['key'] for e in entries]==[a['key'] for a in expected['apps']],'wrong compiled application set/order')
        for e,a in zip(entries,expected['apps']):
            require(int(e['id'])==a['id'] and e['graphics']==a['graphics'] and e['kind']==a['kind'],'catalog metadata mismatch')
            require(bytes.fromhex(e['title']).decode('ascii')==a['title'] and int(e['shortcut'])==a['shortcut'],'catalog label/shortcut mismatch')
            mask=sum(1<<CAPS_BIT[x] for x in set(a['target_requires']))
            want=a['host'] if caps is None else (caps&mask)==mask
            require(int(e['available'])==int(want),'incorrect capability filter: '+a['key'])
    return entries


def application_ids(body,expected=None,caps=None):
    entries=catalog(body,expected,caps)
    return {'menu':0,**({e['key']:int(e['id']) for e in entries} if entries is not None else APP_IDS)}


def completed(body,ids=None):
    ids=application_ids(body) if ids is None else ids
    result=[]
    for line in body.splitlines():
        if line.startswith('console complete '):
            d=fields(line,'console complete ')
            require(d.get('app') in ids,'unknown completed application')
            require(integer(d,'id')==ids[d['app']],'application ID mismatch')
            for key in ('frames','presents','errors'):integer(d,key)
            require(integer(d,'errors')==0,'completion reported errors')
            result.append(d)
    return result

def validate_host(body,expected=None):
    clean(body)
    require('console protocol 2 ' in body and 'menu crc ' in body,'missing product boot/menu')
    ids=application_ids(body,expected)
    done=completed(body,ids)
    required=[a['key'] for a in expected['apps'] if a['kind']!='system' and a['host']] if expected else ('demo','stg','press','r4')
    for name in required:
        require(any(d['app']==name and integer(d,'frames')>0 and integer(d,'presents')>0 for d in done),
                'application did not render and present: '+name)
    return done

def validate_board(body,full=True,expected=None,caps=None):
    clean(body)
    require('console protocol 2 tick_hz 1000' in body,'wrong firmware/protocol/tick config')
    require('menu crc ' in body,'menu has no pixel evidence')
    ids=application_ids(body,expected,caps)
    status=[]
    for line in body.splitlines():
        if not line.startswith('console status '):continue
        d=fields(line,'console status ')
        require(d.get('v')=='2' and d.get('app') in ids,'invalid product state')
        require(integer(d,'id')==ids[d['app']],'status application ID mismatch')
        if expected:
            require(d.get('catalog')==expected['catalog_sha256'],'status content identity mismatch')
            require(caps is not None and int(d.get('caps','-1'),16)==caps,'live CAPS differs from selected bitstream')
        require(d.get('backend') in ('sw','hw','hw-tex','hw-strip'),'invalid backend')
        for key in ('fps_milli','frames','presents','tick_ms','irqA','heap','tasks','idle','monitor','scene','ladder'):
            integer(d,key)
        for key in ('errors','dropped','faults'):require(integer(d,key)==0,'nonzero '+key)
        require(integer(d,'heap')>=4096 and integer(d,'tasks')==4,'invalid RTOS heap/task state')
        status.append(d)
    require(len(status)>=4,'insufficient continuous RTOS evidence')
    require(integer(status[-1],'idle')>integer(status[0],'idle'),'idle task made no progress')
    require(integer(status[-1],'monitor')>integer(status[0],'monitor'),'background monitor made no progress')
    require(integer(status[-1],'tick_ms')-integer(status[0],'tick_ms')>=3000,'kernel clock did not advance')
    for a,b in zip(status,status[1:]):
        dt=integer(b,'tick_ms')-integer(a,'tick_ms')
        df=integer(b,'irqA')-integer(a,'irqA')
        require(dt>0 and df>0,'kernel/display counters are not monotonic')
        require(abs(dt*60-df*1000)<=5000,'tick is not independent 1kHz (or display cadence is broken)')
    if full:
        done=completed(body,ids)
        required=[a['key'] for a in expected['apps'] if (caps&sum(1<<CAPS_BIT[x] for x in set(a['target_requires'])))==sum(1<<CAPS_BIT[x] for x in set(a['target_requires']))] if expected else ('demo','stg','press','r4','about')
        for name in required:
            require(any(d['app']==name and integer(d,'frames')>=10 and integer(d,'presents')>=10 for d in done),
                    'application missing real completed frames: '+name)
        for name,minimum in [('menu',55000),('stg',50000),('press',50000)]:
            if name!='menu' and name not in required:continue
            require(any(d['app']==name and integer(d,'fps_milli')>=minimum for d in status),
                    'steady presentation cadence missing: '+name)
        if 'r4' in required:
            require(any(d['app']=='r4' and integer(d,'scene')==0 and integer(d,'fps_milli')>=28000 for d in status),
                    '3D corridor did not reach measured 30Hz cadence')
            # The cup scene was removed 2026-11-02; the session still presses C, which must stay inert.
            require(not any(d['app']=='r4' and integer(d,'scene')==1 for d in status),
                    'scene key must not switch the 3D corridor (cup removed)')
        require(status[-1]['app']=='menu','final state must be menu')
    return status

def check_negative_fixtures():
    rows=['console protocol 2 tick_hz 1000','menu crc 12345678 sel 0']
    for i in range(5):
        rows.append('console status v 2 app menu id 0 backend hw fps_milli 60000 frames 60 presents 60 '
                    f'errors 0 dropped 0 faults 0 tick_ms {1000+i*1000} irqA {60+i*60} heap 57920 '
                    f'tasks 4 idle {100+i} monitor {i} scene 0 ladder 0')
    good='\n'.join(rows)+'\n'
    validate_board(good,full=False)
    bad=[good+'r4 err present frame 0 0\n',good+'engine prepare err\n',good+'fb err dst==shown\n',
         good.replace('tasks 4','tasks 0'),good.replace('heap 57920','heap 0'),
         re.sub(r'tick_ms \d+','tick_ms 0',good),good.replace('dropped 0','dropped 1'),
         good.replace('monitor 4','monitor 0')]
    for text in bad:
        try:validate_board(text,full=False)
        except RuntimeError:pass
        else:raise RuntimeError('collector accepted a negative lifecycle/clock/error fixture')
    try:validate_board(good,full=True)
    except RuntimeError:pass
    else:raise RuntimeError('collector accepted missing applications/zero-frame evidence')
    return len(bad)+1

def validate_fault(body,stage):
    """An injected fault is a separate required case, never a normal PASS."""
    target='console error '+stage+' total 1'
    require(body.count(target)==1,'expected injected recovery event missing/repeated')
    errors=[line for line in body.splitlines() if line.startswith(('console error ','console fatal '))]
    require(errors==[target],'an unexpected error occurred in fault recovery')
    require('console detail injected '+stage+' code 1' in body,'wrong fault injection')
    allowed={target,'console detail injected '+stage+' code 1'}
    clean('\n'.join(line for line in body.splitlines() if line not in allowed))
    post=body.split(target,1)[1]
    require('console enter menu' in post,'fault did not recover to menu')
    states=[]
    for line in post.splitlines():
        if line.startswith('console status '):
            d=fields(line,'console status ')
            require(d.get('app')=='menu' and integer(d,'errors')==1,'failed app continued after fault')
            require(integer(d,'dropped')==0 and integer(d,'faults')==0,'recovery lost logs/interrupts')
            require(integer(d,'tasks')==4 and integer(d,'heap')>=4096,'recovery damaged RTOS state')
            states.append(d)
    require(len(states)>=3 and integer(states[-1],'frames')>=60,'no real menu frames after recovery')
    for key in ('tick_ms','irqA','idle','monitor'):
        require(integer(states[-1],key)>integer(states[0],key),'recovery stalled '+key)
    return states
