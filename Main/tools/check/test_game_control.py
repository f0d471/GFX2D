#!/usr/bin/env python3
"""离线核对控制面板：后端确认切换、CSV 弹数、硬件/软件按钮、手柄切应用与按键表，不开 GUI/串口。"""
import sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'board'))
from game_control import BoardState, ControlPanel

state=BoardState()
assert state.consume('backend hw-tex frame 20') and state.backend=='hw-tex'
state.consume('csvh,t,mode,exec,fps,drop,n_active,n_bullet,overflow,fps_milli')
state.consume('csv,0,game,hw-tex,60,0,100,90,7,60000')
assert state.backend=='hw-tex' and state.fps==60 and state.bullets==90
panel=ControlPanel.__new__(ControlPanel)
panel.state=BoardState(); panel.state.backend='hw'
panel.pending_backend='sw'; panel.pending_steps=1
panel.restart_pending=False; panel.ensure_hw=False
sent=[]
panel.send_action=lambda keys: sent.append(keys) or True
panel.notify=lambda message: None
panel.process_line('backend hw-tex frame 20')
assert sent==[64] and panel.pending_backend=='sw'
panel.process_line('backend hw-strip frame 21')
assert sent==[64,64] and panel.pending_backend=='sw'
panel.process_line('backend sw frame 22')
assert panel.pending_backend is None
panel.pending_backend='hw-tex'; panel.pending_steps=4
panel.process_line('backend hw frame 22')
assert panel.pending_backend is None and sent==[64,64]
state.consume('csv,1,game,hw-strip,60,0,101,91,0,60000')
assert state.backend=='hw-strip' and state.bullets==91
state.consume('R4STAT 30 35234 96 2 1 0')
assert state.mode=='r3d' and state.backend=='hw' and state.fps==30
assert state.triangles==96 and state.ladder==2 and state.scene==0 and state.render_fps==35.234
state.consume('R4STAT 1 813 202 1 0 1')
assert state.backend=='sw' and state.scene==1 and state.ladder==1
panel.pending_backend=None;panel.state.mode='r3d';panel.state.backend='sw'
panel.set_backend('hw-tex');assert panel.pending_backend=='hw'
print('Control panel: TEX CSV, bullet count, confirmed transitions and unsupported target OK')
state=BoardState()
state.consume('R4STAT 30 35234 96 2 1 0')
state.consume('console protocol 2 tick_hz 1000')
state.consume('console enter menu')
assert state.mode=='menu' and state.backend is None and state.fps is None
state.consume('console status v 2 app menu id 0 backend hw fps_milli 59980 frames 60 presents 60 errors 0 scene 0 ladder 0')
assert state.protocol==2 and state.app_id==0 and state.mode=='menu' and state.fps==59.98
state.consume('console enter stg')
assert state.mode=='game' and state.backend is None
state.consume('console status v 2 app stg id 2 backend hw-tex fps_milli 60000 frames 120 presents 119 errors 0 scene 0 ladder 0')
assert state.app_id==2 and state.backend=='hw-tex' and state.fps==60
state.consume('console enter about')
assert state.mode=='about' and state.fps is None and state.triangles is None
state.consume('console error enter total 1')
assert state.mode=='fault' and state.backend is None
print('Control panel protocol2: menu/game/about/error and stale-backend reset OK')
panel=ControlPanel.__new__(ControlPanel)
panel.state=BoardState();panel.state.protocol=2;panel.state.mode='game';panel.state.backend='hw'
panel.pending_backend='hw-tex';panel.pending_steps=1
panel.restart_pending=False;panel.ensure_hw=False
sent=[];panel.send_action=lambda keys: sent.append(keys) or True;panel.notify=lambda message:None
panel.process_line('console enter r4')
assert panel.pending_backend is None and sent==[]
# 面板只分硬件/软件：任一硬件后端下「硬件」不再按 Tab，「软件」才发一次切换。
panel.state.mode='game';panel.state.backend='hw-tex'
panel.choose_backend('hw');assert sent==[]
panel.state.backend='hw';panel.choose_backend('sw');assert sent==[64] and panel.pending_backend=='sw'
from types import SimpleNamespace
from unittest.mock import Mock
import tempfile,json
with tempfile.TemporaryDirectory() as directory:
    view=ControlPanel.__new__(ControlPanel)
    for name in ('status','apps_label','root'):
        setattr(view,name,Mock())
    view.state=BoardState();view.state.mode='r3d';view.state.backend='hw';view.state.fps=30
    view.forwarder=SimpleNamespace(user32=SimpleNamespace(GetForegroundWindow=lambda:1),focus=1,last=0x380)
    view.port=None
    view.notice_text='';view.notice_until=0;view.last_status_write=0
    view.args=SimpleNamespace(status_file=str(Path(directory)/'status.json'),port='COM11')
    view.refresh()
    assert json.loads(Path(view.args.status_file).read_text(encoding='utf-8'))['keys']==0x380
    text=view.status.configure.call_args.kwargs['text']
    assert '三维走廊' in text and '30 FPS' in text and '硬件' in text and '未连接 COM11' in text
print('Control panel: cross-app pending cancelled; one-line status keeps raw key telemetry')

# Sparse application IDs keep their catalog labels.
state=BoardState()
state.consume('console catalog begin v 1 profile custom hash '+'a'*64+' count 2')
state.consume('console catalog app id 42 key extra kind game graphics 3d shortcut 0 available 1 title 4558545241')
state.consume('console catalog app id 5 key about kind system graphics 2d shortcut 0 available 1 title 53595354454d')
state.consume('console catalog end')
state.consume('console enter extra')
assert state.app_id==42 and state.label()=='EXTRA'
broken=BoardState()
broken.consume('console catalog begin v 1 profile bad hash '+'b'*64+' count 2')
broken.consume('console catalog app id 42 key extra kind game graphics 3d shortcut 0 available 1 title 4558545241')
broken.consume('console catalog end')
assert not broken.catalog
print('Control panel: versioned directory and sparse-ID labels PASS')

# Shortcut 5 (flight) is a valid catalog entry; the gamepad shoulders walk shortcuts 1..5.
state=BoardState()
state.consume('console catalog begin v 1 profile full hash '+'c'*64+' count 5')
for i,(key,title) in enumerate([('demo','DEMO'),('stg','DANMAKU'),('press','STRESS'),('r4','CORRIDOR'),('flight','RIDGE')],1):
    state.consume(f'console catalog app id {i} key {key} kind game graphics 2d shortcut {i} available 1 title {title.encode().hex()}')
state.consume('console catalog end')
assert sorted(e['shortcut'] for e in state.catalog.values())==[1,2,3,4,5]
panel.state=state
sent=[];panel.send_action=lambda keys:sent.append(keys) or True
for app,step,expected in [(2,1,1<<9),(5,1,1<<7),(0,-1,1<<13),(3,1,0x380),(1,-1,1<<13)]:
    state.app_id=app;panel.switch_app(step);assert sent[-1]==expected,(app,step,sent)
print('Control panel: shortcut 5 catalog and gamepad previous/next application OK')

# Forwarded keys: digits, Enter, arrows, Esc, Space, Shift; the gamepad uses the same table.
from input_forward import InputForwarder
class Keys:
    def __init__(self,held):self.held=set(held)
    def GetForegroundWindow(self):return 1
    def GetAsyncKeyState(self,k):return 0x8000 if k in self.held else 0
class Pad:
    def __init__(self,buttons,trigger=0):self.buttons,self.trigger=buttons,trigger
    def XInputGetState(self,index,ref):
        ref._obj.pad.buttons=self.buttons;ref._obj.pad.left_trigger=self.trigger;return 0
def sample(held=(),pad=None):
    f=InputForwarder.__new__(InputForwarder)
    f.user32=Keys(held);f.focus=1;f.xinput=pad;f.app_step=0;f.pad_prev=0
    return f.sample(),f
assert sample([0x09,ord('C'),ord('W'),0x08])[0]==0
assert [sample([k])[0] for k in (0x20,0x10,0x0d,0x1b,ord('1'),ord('4'),ord('5'))]==[16,32,1024,2048,128,0x380,1<<13]
assert sample([0x26,0x27])[0]==9
assert [sample(pad=Pad(b))[0] for b in (0x1000,0x0010,0x2000,0x0020,0x4000,0x0001)]==[1024,1024,2048,2048,16,1]
assert sample(pad=Pad(0,200))[0]==32 and sample(pad=Pad(0x8000))[0]==0
bits,f=sample(pad=Pad(0x0200));assert bits==0 and f.app_step==1
bits,f=sample(pad=Pad(0x0100));assert bits==0 and f.app_step==-1
print('Input forwarding: trimmed keyboard and matching gamepad table OK')
