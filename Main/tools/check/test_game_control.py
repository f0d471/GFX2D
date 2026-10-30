#!/usr/bin/env python3
"""离线核对控制面板的四个后端、CSV 弹数和确认后的连续切换，不开 GUI/串口。"""
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
panel.restart_pending=True
panel.process_line('console enter menu')
assert panel.restart_pending is False and sent==[256]
from types import SimpleNamespace
from unittest.mock import Mock
import tempfile,json
with tempfile.TemporaryDirectory() as directory:
    view=ControlPanel.__new__(ControlPanel)
    for name in ('mode_label','backend_label','fps_label','scene_label','notice','input_label','connection','root'):
        setattr(view,name,Mock())
    view.state=BoardState();view.state.mode='r3d';view.state.backend='hw';view.state.fps=30
    view.forwarder=SimpleNamespace(user32=SimpleNamespace(GetForegroundWindow=lambda:1),focus=1,last=0x380)
    view.enabled=SimpleNamespace(get=lambda:True);view.port=None
    view.notice_text='';view.last_status_write=0
    view.args=SimpleNamespace(status_file=str(Path(directory)/'status.json'),port='COM11')
    view.refresh()
    assert json.loads(Path(view.args.status_file).read_text(encoding='utf-8'))['keys']==0x380
    assert '三维 [4]' in view.input_label.configure.call_args.kwargs['text']
print('Control panel: cross-app pending commands cancelled; displayed keys preserve raw telemetry')

# A menu-selected sparse-ID application can restart through observed menu
# navigation, without allocating another launch bit or assuming four games.
state=BoardState()
state.consume('console catalog begin v 1 profile custom hash '+'a'*64+' count 2')
state.consume('console catalog app id 42 key extra kind game graphics 3d shortcut 0 available 1 title 4558545241')
state.consume('console catalog app id 5 key about kind system graphics 2d shortcut 0 available 1 title 53595354454d')
state.consume('console catalog end')
state.consume('console enter extra')
assert state.app_id==42 and state.label()=='EXTRA'
panel.state=state;panel.restart_pending=False
sent=[];panel.send_action=lambda keys:sent.append(keys) or True
panel.restart_game();assert sent==[2048] and panel.restart_target==42
panel.process_line('console enter menu')
panel.process_line('console menu index 1 id 5 count 2')
assert sent==[2048,2] and panel.restart_pending
panel.process_line('console menu index 0 id 42 count 2')
assert sent==[2048,2,1024] and not panel.restart_pending
broken=BoardState()
broken.consume('console catalog begin v 1 profile bad hash '+'b'*64+' count 2')
broken.consume('console catalog app id 42 key extra kind game graphics 3d shortcut 0 available 1 title 4558545241')
broken.consume('console catalog end')
assert not broken.catalog
print('Control panel: versioned directory, sparse-ID labels and generic restart PASS')
