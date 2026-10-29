"""Strict, side-effect-free console protocol/evidence validation."""
import re

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

def completed(body):
    result=[]
    for line in body.splitlines():
        if line.startswith('console complete '):
            d=fields(line,'console complete ')
            require(d.get('app') in APP_IDS,'unknown completed application')
            require(integer(d,'id')==APP_IDS[d['app']],'application ID mismatch')
            for key in ('frames','presents','errors'):integer(d,key)
            require(integer(d,'errors')==0,'completion reported errors')
            result.append(d)
    return result

def validate_host(body):
    clean(body)
    require('console protocol 2 ' in body and 'menu crc ' in body,'missing product boot/menu')
    done=completed(body)
    for name in ('demo','stg','press','r4'):
        require(any(d['app']==name and integer(d,'frames')>0 and integer(d,'presents')>0 for d in done),
                'application did not render and present: '+name)
    return done

def validate_board(body,full=True):
    clean(body)
    require('console protocol 2 tick_hz 1000' in body,'wrong firmware/protocol/tick config')
    require('menu crc ' in body,'menu has no pixel evidence')
    status=[]
    for line in body.splitlines():
        if not line.startswith('console status '):continue
        d=fields(line,'console status ')
        require(d.get('v')=='2' and d.get('app') in APP_IDS,'invalid product state')
        require(integer(d,'id')==APP_IDS[d['app']],'status application ID mismatch')
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
        done=completed(body)
        for name in ('demo','stg','press','r4','about'):
            require(any(d['app']==name and integer(d,'frames')>=10 and integer(d,'presents')>=10 for d in done),
                    'application missing real completed frames: '+name)
        for name,minimum in [('menu',55000),('stg',50000),('press',50000)]:
            require(any(d['app']==name and integer(d,'fps_milli')>=minimum for d in status),
                    'steady presentation cadence missing: '+name)
        require(any(d['app']=='r4' and integer(d,'scene')==0 and integer(d,'fps_milli')>=28000 for d in status),
                '3D corridor did not reach measured 30Hz cadence')
        cup=[d for d in status if d['app']=='r4' and integer(d,'scene')==1]
        require(len(cup)>=2 and integer(cup[-1],'frames')>integer(cup[0],'frames') and
                integer(cup[-1],'presents')>integer(cup[0],'presents'),
                'private scene command did not produce continuing cup frames')
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
