#!/usr/bin/env python3
"""离线核对控制面板的三个后端、CSV 弹数和确认后的连续切换，不开 GUI/串口。"""
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
panel.process_line('backend sw frame 21')
assert panel.pending_backend is None
panel.pending_backend='hw-tex'; panel.pending_steps=3
panel.process_line('backend hw frame 22')
assert panel.pending_backend is None and sent==[64]
print('Control panel: TEX CSV, bullet count, confirmed transitions and unsupported target OK')
