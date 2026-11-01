"""连接正在运行的 gfx2d 游戏，显示操作说明与板上状态并转发按键。"""

import argparse
import codecs
import ctypes
import json
import re
import time
from pathlib import Path
import tkinter as tk
from tkinter import ttk

import serial

from input_forward import InputForwarder, packet


MODE_NAMES = {"menu":"应用菜单", "about":"系统信息", "fault":"故障恢复", "flight":"山脊飞行",
              "demo": "1 · 图形演示", "game": "2 · 弹幕游戏", "press": "3 · 压力测试", "r3d": "4 · 三维演示"}
MODE_IDS = {"1": "demo", "2": "game", "3": "press", "4": "r3d", "5": "flight"}
KEY_NAMES = ("上", "下", "左", "右", "炸弹", "慢速", "切后端", "2D [1]", "弹幕 [2]", "压力 [3]", "确认", "返回", "场景")


class BoardState:
    """从板上日志更新状态；未收到反馈时保留未知值。"""

    def __init__(self):
        self.mode = None
        self.backend = None
        self.fps = None
        self.bullets = None
        self.triangles = None
        self.ladder = None
        self.scene = None
        self.render_fps = None
        self.last_csv = 0.0
        self.headers = None
        self.protocol = 1
        self.app_id = None
        self.errors = 0
        self.last_error = None
        self.catalog = {}
        self.catalog_hash = None
        self.profile = None
        self.capabilities = None
        self._catalog_pending = None
        self.menu_index = None
        self._content_manifest = None

    def label(self):
        entry=self.catalog.get(self.app_id)
        return entry['title'] if entry else MODE_NAMES.get(self.mode,self.mode or '等待板上反馈')

    def seed_content(self, content, caps=None):
        if content.get('version')!=1:return
        self._content_manifest=content
        entries={}
        for a in content['apps']:
            mask=sum(1<<content['capability_bits'][k] for k in set(a['target_requires']))
            entries[a['id']]={**a,'available':None if caps is None else (caps&mask)==mask}
        self.catalog=entries;self.catalog_hash=content['catalog_sha256'];self.profile=content['profile']

    def _load_local_content(self, fingerprint):
        # A bare panel may attach after the boot catalog. Match the live identity;
        # never infer a directory from mode IDs or from the latest source alone.
        try:
            from console_catalog import load,resolve
            data=load()
            for name in data['profiles']:
                selected=resolve(data,name)
                if selected['catalog_sha256']==fingerprint:
                    self.seed_content(selected,self.capabilities);return
        except (ImportError,OSError,ValueError,KeyError):pass

    def consume(self, line):
        before = (self.mode, self.backend)
        if line=='console boot':
            self.catalog={};self.catalog_hash=None;self.profile=None;self._catalog_pending=None
            self.menu_index=None;self.mode=None;self.app_id=None
        elif line.startswith('console menu '):
            values=line.split()[2:];data=dict(zip(values[::2],values[1::2]))
            try:self.menu_index=int(data['index'])
            except (KeyError,ValueError):return False
        elif line.startswith('console catalog begin '):
            values=line.split()[3:];data=dict(zip(values[::2],values[1::2]))
            try:
                if data.get('v')!='1' or not re.fullmatch('[0-9a-f]{64}',data['hash']):return False
                self._catalog_pending=(data,{})
            except KeyError:self._catalog_pending=None
        elif line.startswith('console catalog app '):
            if self._catalog_pending is None:return False
            values=line.split()[3:];data=dict(zip(values[::2],values[1::2]))
            try:
                entry=dict(id=int(data['id']),key=data['key'],kind=data['kind'],graphics=data['graphics'],
                           shortcut=int(data['shortcut']),available=bool(int(data['available'])),title=bytes.fromhex(data['title']).decode('ascii'))
                if not (0<entry['id']<0xffffffff and entry['shortcut'] in range(5) and data['available'] in ('0','1')
                        and entry['kind'] in ('game','demo','benchmark','system') and entry['graphics'] in ('2d','3d')
                        and 0<len(entry['title'])<=80 and re.fullmatch(r'[A-Za-z_]\w{0,47}',entry['key'],re.ASCII)):
                    raise ValueError('invalid entry')
                entries=self._catalog_pending[1]
                if entry['id'] in entries or any(e['key']==entry['key'] for e in entries.values()):raise ValueError('duplicate entry')
                entries[entry['id']]=entry
            except (KeyError,ValueError,UnicodeError):self._catalog_pending=None
        elif line=='console catalog end':
            if self._catalog_pending is not None:
                header,entries=self._catalog_pending;self._catalog_pending=None
                try:
                    if len(entries)!=int(header['count']):return False
                    self.catalog=entries;self.profile=header['profile'];self.catalog_hash=header['hash']
                except (KeyError,ValueError):return False
        elif line.startswith("console protocol 2"):
            self.protocol=2
        elif line.startswith("console status "):
            values=line.split()[2:]
            data=dict(zip(values[::2],values[1::2]))
            try:
                if data.get("v")!="2":return False
                self.protocol=2;self.app_id=int(data["id"])
                self.mode={"stg":"game","r4":"r3d"}.get(data["app"],data["app"])
                self.backend=None if data["backend"]=="none" else data["backend"]
                self.fps=int(data["fps_milli"])/1000.0
                self.errors=int(data["errors"])
                self.scene=int(data.get("scene",0));self.ladder=int(data.get("ladder",0))
                if 'catalog' in data:
                    self.capabilities=int(data['caps'],16)
                    if self._content_manifest and self._content_manifest['catalog_sha256']==data['catalog']:
                        self.seed_content(self._content_manifest,self.capabilities)
                    if self.catalog_hash!=data['catalog']:
                        self.catalog={};self.catalog_hash=None;self._load_local_content(data['catalog'])
                self.last_csv=time.monotonic()
                if self.mode!="r3d":self.triangles=None;self.render_fps=None
                if self.mode not in ("game","press"):self.bullets=None
            except (KeyError,ValueError):return False
        elif line.startswith("console enter "):
            app=line.split()[-1]
            self.protocol=2;self.mode={"stg":"game","r4":"r3d"}.get(app,app)
            self.app_id=next((i for i,e in self.catalog.items() if e['key']==app),None)
            if app=='menu':self.menu_index=None
            self.backend=None;self.fps=None;self.last_csv=time.monotonic()
            if self.mode!="r3d":self.triangles=None;self.render_fps=None
        elif line.startswith("console error ") or line.startswith("console fatal "):
            self.last_error=line;self.mode="fault";self.fps=None;self.backend=None
        elif line.startswith("csvh,"):
            self.headers = line.split(",")[1:]
        elif line.startswith("csv,"):
            fields = line.split(",")
            if len(fields) >= 9 and fields[2] in MODE_NAMES and fields[3] in ("hw", "hw-tex", "hw-strip", "sw"):
                try:
                    row = dict(zip(self.headers, fields[1:])) if self.headers else {}
                    milli = row.get("fps_milli")
                    if milli is None and fields[-6] in ("serial", "pipeline"):
                        milli = fields[-1]
                    fps = float(milli) / 1000 if milli is not None else float(fields[4])
                    bullets = int(row.get("n_bullet", fields[7]))
                except (ValueError, IndexError):
                    return False
                self.mode, self.backend = fields[2], fields[3]
                self.fps, self.bullets = fps, bullets
                self.last_csv = time.monotonic()
        elif line.startswith("backend "):
            backend = line.split()[1]
            if backend in ("hw", "hw-tex", "hw-strip", "sw"):
                self.backend = backend
                self.fps = None
        elif line.startswith("r4 enter ladder/hw "):
            self.mode="r3d"
            self.backend="hw" if line.split()[-1]=="1" else "sw"
            self.ladder=int(line.split()[-2])
            self.fps=None
        elif line.startswith("R4STAT "):
            try:
                fields=[int(x) for x in line.split()[1:]]
                if len(fields)!=6:return False
                self.mode="r3d";self.fps=fields[0];self.backend="hw" if fields[4] else "sw"
                self.triangles=fields[2]
                self.render_fps=fields[1]/1000
                self.ladder=fields[3];self.scene=fields[5]
                self.last_csv=time.monotonic();self.bullets=None
            except (ValueError,IndexError):return False
        else:
            match = re.match(r"mode ([1234]) enter frame ", line)
            if match:
                self.mode = MODE_IDS[match[1]]
                self.fps = None
        return before != (self.mode, self.backend)


class ControlPanel:
    def __init__(self, root, args):
        self.root, self.args = root, args
        self.state = BoardState()
        if getattr(args,'content',None):
            self.state.seed_content(json.loads(Path(args.content).read_text(encoding='utf-8')))
        if getattr(args,'bootstrap_log',None):
            for line in Path(args.bootstrap_log).read_text(encoding='utf-8-sig',errors='replace').splitlines():
                self.state.consume(line)
        self.port = None
        self.decoder = codecs.getincrementaldecoder("utf-8")("replace")
        self.buffer = ""
        self.ensure_hw = args.ensure_hw
        self.pending_backend = None
        self.pending_since = 0.0
        self.pending_steps = 0
        self.restart_pending = False
        self.restart_since = 0.0
        self.restart_target = None
        self.closed = False
        self.last_status_write = 0.0
        self.notice_text = "连接后从板上读取当前模式、后端和 FPS。"
        self.log = None
        if args.log:
            path = Path(args.log)
            path.parent.mkdir(parents=True, exist_ok=True)
            self.log = path.open("a", encoding="utf-8", buffering=1)
        self.enabled = tk.BooleanVar(value=True)
        self._build()
        root.update()
        self.forwarder = InputForwarder()
        user32 = self.forwarder.user32
        user32.GetAncestor.argtypes = (ctypes.c_void_p, ctypes.c_uint)
        user32.GetAncestor.restype = ctypes.c_void_p
        self.forwarder.focus = user32.GetAncestor(root.winfo_id(), 2)
        root.protocol("WM_DELETE_WINDOW", self.close)
        root.bind("<Tab>", lambda event: "break")
        root.bind("<F8>", self.toggle_input)
        root.after(100, self.connect)
        root.after(20, self.poll)

    def _build(self):
        root = self.root
        root.title("gfx2d 游戏控制面板")
        root.configure(bg="#eef2f7")
        root.geometry("1060x960+50+20")
        root.minsize(1000, 930)
        style = ttk.Style(root)
        style.theme_use("clam")
        style.configure("TFrame", background="#eef2f7")
        style.configure("TLabel", background="#eef2f7", font=("Microsoft YaHei UI", 10))
        style.configure("TLabelframe", background="#eef2f7")
        style.configure("TLabelframe.Label", background="#eef2f7", font=("Microsoft YaHei UI", 11, "bold"))
        style.configure("TButton", font=("Microsoft YaHei UI", 10), padding=(10, 6))
        style.configure("TCheckbutton", background="#eef2f7", font=("Microsoft YaHei UI", 10))
        outer = ttk.Frame(root, padding=18)
        outer.pack(fill="both", expand=True)
        ttk.Label(outer, text="gfx2d · 游戏控制", font=("Microsoft YaHei UI", 22, "bold")).pack(anchor="w")
        ttk.Label(outer, text="画面在板子的 HDMI 屏幕；保持此面板在前台，用键盘控制。", padding=(0, 4, 0, 10)).pack(anchor="w")
        status = ttk.Frame(outer)
        status.pack(fill="x")
        self.mode_label = self._status_box(status, "当前模式", "等待板上反馈", 0)
        self.backend_label = self._status_box(status, "当前绘图后端", "等待板上反馈", 1)
        self.fps_label = self._status_box(status, "实际帧率 / 场景数量", "— FPS / —", 2)
        self.notice = tk.Label(outer, text=self.notice_text, anchor="w", justify="left", wraplength=985,
                               bg="#e0ecff", fg="#164582", padx=12, pady=10,
                               font=("Microsoft YaHei UI", 11, "bold"))
        self.notice.pack(fill="x", pady=(10, 8))
        actions = ttk.Frame(outer)
        actions.pack(fill="x", pady=(0, 8))
        ttk.Button(actions, text="HW/DDR", command=lambda: self.set_backend("hw")).pack(side="left", padx=(0, 6))
        ttk.Button(actions, text="HW/TEX（推荐）", command=lambda: self.set_backend("hw-tex")).pack(side="left", padx=(0, 6))
        ttk.Button(actions, text="HW/STRIP 条带", command=lambda: self.set_backend("hw-strip")).pack(side="left", padx=(0, 6))
        ttk.Button(actions, text="SW 软件对比", command=lambda: self.set_backend("sw")).pack(side="left", padx=(0, 6))
        ttk.Button(actions, text="返回菜单 [Esc]", command=lambda:self.send_action(1<<11)).pack(side="left", padx=(0,6))
        ttk.Button(actions, text="确认 [Enter]", command=lambda:self.send_action(1<<10)).pack(side="left", padx=(0,6))
        ttk.Button(actions, text="重开当前应用", command=self.restart_game).pack(side="left", padx=(0, 6))
        ttk.Button(actions, text="重新连接", command=self.connect).pack(side="right")
        three_d=ttk.Frame(outer);three_d.pack(fill="x",pady=(0,8))
        ttk.Button(three_d,text="菜单向上",command=lambda:self.send_action(1)).pack(side="left",padx=(0,6))
        ttk.Button(three_d,text="菜单向下",command=lambda:self.send_action(2)).pack(side="left",padx=(0,6))
        ttk.Button(three_d,text="场景 [C]",command=lambda:self.send_action((1<<12) if self.state.protocol>=2 else 0x380)).pack(side="left",padx=(0,6))
        ttk.Button(three_d,text="山脊 [5]",command=lambda:self.send_action((1<<13) if self.state.protocol>=2 else 0)).pack(side="left",padx=(0,6))
        ttk.Button(three_d,text="L1 / L2 [Shift]",command=lambda:self.send_action(1<<5)).pack(side="left",padx=(0,6))
        ttk.Button(three_d,text="炸弹 [Space]",command=lambda:self.send_action(1<<4)).pack(side="left",padx=(0,6))
        self.scene_label=ttk.Label(three_d,text="三维场景：等待板上反馈")
        self.scene_label.pack(side="left",padx=12)
        self.catalog_label=ttk.Label(outer,text='应用目录：等待固件目录；上下选择，Enter 确认',wraplength=980)
        self.catalog_label.pack(fill='x',pady=(0,8))
        self.connection = ttk.Label(outer, text="串口：等待连接", foreground="#53657e")
        self.connection.pack(anchor="w")
        self.input_label = ttk.Label(outer, text="按键：等待连接", foreground="#53657e")
        self.input_label.pack(anchor="w", pady=(2, 8))
        rules = ttk.Frame(outer)
        rules.pack(fill="both", expand=True)
        rules.columnconfigure(0, weight=1, uniform="rules")
        rules.columnconfigure(1, weight=1, uniform="rules")
        left = ttk.LabelFrame(rules, text="键盘操作", padding=10)
        left.grid(row=0, column=0, sticky="nsew", padx=(0, 7))
        right = ttk.LabelFrame(rules, text="手柄与板上按键", padding=10)
        right.grid(row=0, column=1, sticky="nsew", padx=(7, 0))
        self._rules(left, (
            ("W A S D / 方向键", "移动飞船；斜向可以同时按两方向"),
            ("左 Shift（按住）", "慢速移动，并显示判定点"),
            ("空格（每次按下）", "使用一颗炸弹；三维也可清弹并获得短暂无敌"),
            ("射击", "自动射击，无需按射击键"),
            ("菜单上下 / Enter", "选择并进入本固件提供的应用；数字键为可选快捷入口"),
            ("Enter / Esc / C", "确认 / 返回菜单 / 切三维场景"),
            ("三维 Shift / Tab", "L1、L2阶梯 / 板上CPU、硬件光栅对照"),
            ("Tab", "HW ↔ SW；SW 由板上 CPU 绘图，会很慢"),
            ("重开游戏", "先返回菜单再进入游戏；或点重开按钮"),
            ("F8", "启用 / 停止输入转发，板上游戏继续运行"),
        ))
        self._rules(right, (
            ("手柄方向键 / 左摇杆", "移动飞船（第一个 XInput 手柄）"),
            ("手柄 A / LB / Start", "炸弹 / 慢速 / 确认"),
            ("手柄 Back / RB / B", "返回 / 场景 / 切后端"),
            ("板上 SW0 / SW1", "左 / 下"),
            ("板上 SW2 / SW3", "上 / 右"),
            ("板上物理上 + 下", "返回菜单"),
            ("板上物理左 + 右", "确认选择；游戏中为炸弹"),
            ("键盘方向组合", "保持为方向输入，不再切应用或后端"),
            ("模式 1 / 模式 3", "演示或压力测试，不是可操作的游戏"),
        ))
        warning = tk.Label(outer, text="产品前端：数字键始终启动同一个应用；Esc 返回菜单，C 切三维场景。\n"
                           "菜单、系统页与应用状态都由板上反馈；软件参考后端较慢，可用 Tab 切回硬件。",
                           bg="#fff0d8", fg="#854b00", anchor="w", justify="left", padx=12, pady=9,
                           font=("Microsoft YaHei UI", 10, "bold"))
        warning.pack(fill="x", pady=(10, 8))
        play = ttk.LabelFrame(outer, text="游戏规则与输入状态", padding=10)
        play.pack(fill="x")
        self._rules(play, (
            ("开局与结束", "标题显示 120 帧后自动开始；初始 3 残机、3 炸弹；残机用完回标题，通关后自动新一局。"),
            ("受击与炸弹", "有炸弹时受击自动消耗一颗；没有炸弹时扣残机并补回 3 炸弹。"),
            ("自动驾驶", "连续 600 帧无输入后接管（60 FPS 下约 10 秒）；任意游戏按键立即接管回来。"),
            ("窗口与退出", "切到其它窗口时停止键盘输入，手柄仍可用；关闭面板会松开转发键，板上游戏继续。"),
        ))
        ttk.Checkbutton(outer, text="启用键盘与手柄转发（F8 切换；停止转发不会暂停游戏）",
                        variable=self.enabled, command=self.release_keys).pack(anchor="w", pady=(8, 0))

    @staticmethod
    def _status_box(parent, title, value, column):
        parent.columnconfigure(column, weight=1, uniform="status")
        frame = tk.Frame(parent, bg="white", padx=12, pady=10)
        frame.grid(row=0, column=column, sticky="nsew", padx=(0 if column == 0 else 8, 0))
        tk.Label(frame, text=title, bg="white", fg="#64748b", font=("Microsoft YaHei UI", 10)).pack(anchor="w")
        label = tk.Label(frame, text=value, bg="white", fg="#17243b", font=("Microsoft YaHei UI", 17, "bold"))
        label.pack(anchor="w", pady=(4, 0))
        return label

    @staticmethod
    def _rules(parent, rows):
        for index, (key, action) in enumerate(rows):
            ttk.Label(parent, text=key, font=("Microsoft YaHei UI", 9, "bold")).grid(row=index, column=0, sticky="nw", pady=4, padx=(0, 12))
            ttk.Label(parent, text=action, font=("Microsoft YaHei UI", 9), wraplength=310 if len(rows) == 8 else 780).grid(row=index, column=1, sticky="nw", pady=4)

    def notify(self, message):
        self.notice_text = message
        self.notice.configure(text=message)

    def connect(self):
        if self.port is not None:
            self.release_keys()
            if self.port is not None:
                self.port.close()
            self.port = None
        self.state = BoardState()
        self.pending_backend = None
        self.restart_pending = False
        self.buffer = ""
        self.decoder.reset()
        try:
            self.port = serial.Serial(self.args.port, self.args.baud, timeout=0, write_timeout=0.2)
            self.port.reset_input_buffer()
            self.forwarder.last = None
            self.ensure_hw = self.args.ensure_hw
            self.connection.configure(text=f"已连接 {self.args.port} · 等待板上反馈", foreground="#167144")
            self.notify("已连接；保持面板在前台，开始控制。正在读取板上的模式与绘图后端。")
            if self.log:
                self.log.write(f"\n# control panel connected {time.strftime('%Y-%m-%d %H:%M:%S')}\n")
        except serial.SerialException as error:
            self.connection.configure(text=f"无法连接 {self.args.port}：{error}", foreground="#b42318")
            self.notify("串口未连接。检查 USB 与串口占用，然后点击“重新连接”。")
        self.refresh()

    def send_action(self, mask):
        if self.port is None:
            self.notify("串口未连接；先点击“重新连接”。")
            return False
        try:
            self.port.write(packet(mask))
            self.port.write(packet(0))
            self.forwarder.last = None
            return True
        except serial.SerialException as error:
            self.disconnect(error)
            return False

    def set_backend(self, target):
        if self.state.protocol>=2 and self.state.mode in (None,"menu","about","fault"):
            self.notify("先进入应用，再切换绘图后端。")
            return
        if self.state.mode == "r3d" and target.startswith("hw"):
            target = "hw"
        if self.pending_backend is not None:
            self.notify("正在等待上一次后端切换的板上确认。")
        elif self.state.backend is None:
            self.notify("尚未收到当前后端；等待板上反馈后再切换。")
        elif self.state.backend == target:
            self.notify(f"板上当前已是 {target.upper()} 后端。")
        elif self.send_action(1 << 6):
            self.pending_backend = target
            self.pending_steps = 1
            self.pending_since = time.monotonic()
            self.notify(f"已请求切到 {target.upper()}，等待板上确认。")

    def restart_game(self):
        if self.restart_pending:
            return
        if self.state.catalog:
            entry=self.state.catalog.get(self.state.app_id)
            if not entry or entry['kind']=='system':
                self.notify('先进入要重开的应用。');return
            self.restart_target=entry['id']
            if self.send_action(1<<11):
                self.restart_pending=True;self.restart_since=time.monotonic()
                self.notify('返回菜单后按应用目录重开 '+entry['title'])
            return
        if self.state.mode is None:
            self.notify("尚未收到当前模式；等待板上反馈后再重开。")
            return
        if self.state.protocol>=2:
            if self.state.mode=="menu":
                self.send_action(1<<8)
            elif self.send_action(1<<11):
                self.restart_pending=True
                self.restart_since=time.monotonic()
                self.notify("返回菜单后重开弹幕游戏。")
        elif self.state.mode == "demo":
            if self.send_action(1 << 8):
                self.notify("已请求进入游戏模式，等待板上确认。")
        elif self.send_action(1 << 7):
            self.restart_pending = True
            self.restart_since = time.monotonic()
            self.notify("正在重开游戏：先切到演示，收到板上确认后再进入游戏。")

    def release_keys(self):
        if self.port is not None:
            try:
                self.port.write(packet(0))
                self.forwarder.last = None
            except serial.SerialException as error:
                self.disconnect(error)

    def toggle_input(self, event=None):
        self.enabled.set(not self.enabled.get())
        self.release_keys()
        return "break"

    def disconnect(self, error):
        if self.port is not None:
            self.port.close()
        self.port = None
        self.connection.configure(text=f"串口已断开：{error}", foreground="#b42318")
        self.notify("USB / 串口连接已断开。上方保留的是最后一次反馈；重连后点击“重新连接”。")
        if self.log:
            self.log.write(f"# serial lost: {error}\n")

    def process_line(self, line):
        before_backend = self.state.backend
        changed = self.state.consume(line)
        if line.startswith("console enter "):
            # A backend confirmation belongs to one application. Never replay
            # its remaining Tab actions into the newly selected application.
            self.pending_backend=None
            if self.restart_pending and self.state.mode!="menu":
                self.restart_pending=False
                self.notify("应用已切换，取消尚未完成的重开请求。")
        if line.startswith("console error ") or line.startswith("console fatal "):
            self.pending_backend=None;self.restart_pending=False
            self.notify("板端报告故障："+line)
            return
        if changed:
            mode = self.state.label()
            backend = (self.state.backend or "未知").upper()
            self.notify(f"{time.strftime('%H:%M:%S')} 板上反馈：{mode} · {backend} 后端。" +
                        (" SW 软件绘图会很慢，正常游玩请回到 HW。" if self.state.backend == "sw" else ""))
        if self.pending_backend == self.state.backend:
            self.pending_backend = None
        elif self.pending_backend and before_backend != self.state.backend:
            if self.pending_steps >= 4:
                self.pending_backend = None
                self.notify("板上未进入请求的后端；当前程序可能不提供该后端，已停止切换。")
            elif self.send_action(1 << 6):
                self.pending_steps += 1
                self.pending_since = time.monotonic()
        if self.restart_pending and self.state.mode == ("menu" if self.state.protocol>=2 else "demo"):
            if self.state.catalog:
                if line.startswith('console menu '):
                    available=[i for i,e in self.state.catalog.items() if e.get('available') is not False]
                    target=getattr(self,'restart_target',None)
                    if target not in available:
                        self.restart_pending=False;self.notify('当前固件无法启动该应用。')
                    elif available.index(target)==self.state.menu_index:
                        self.restart_pending=False;self.send_action(1<<10)
                    else:self.send_action(2) # Wait for the next observed selection before moving again.
            else:
                self.restart_pending = False
                self.send_action(1 << 8)
        if self.ensure_hw and self.state.backend is not None:
            self.ensure_hw = False
            if self.state.backend == "sw":
                self.set_backend("hw")

    def refresh(self):
        self.mode_label.configure(text=self.state.label())
        if self.state.catalog:
            names=[e['title'] for e in self.state.catalog.values() if e.get('available') is not False]
            text=' / '.join(names[:6])+(' …' if len(names)>6 else '')
            self.catalog_label.configure(text=f"应用目录 [{self.state.profile}]：{text}；上下选择，Enter 确认")
        backend = self.state.backend
        self.backend_label.configure(text={"hw": "HW/DDR · FPGA 加速", "hw-tex": "HW/TEX · 片上纹理", "hw-strip": "HW/STRIP · 条带渲染", "sw": "SW · CPU 绘图"}.get(backend, "等待板上反馈"),
                                     fg="#167144" if backend in ("hw", "hw-tex", "hw-strip") else "#b42318" if backend == "sw" else "#17243b")
        if self.state.mode=="r3d":
            self.backend_label.configure(text="等待三维反馈" if backend is None else
                                         "HW · 三维光栅" if backend=="hw" else "SW · CPU 参考")
        fps = f"{self.state.fps:.1f}" if self.state.fps is not None else "—"
        bullets = str(self.state.bullets) if self.state.bullets is not None else "—"
        self.fps_label.configure(text=f"{fps} FPS / {bullets} 颗")
        if self.state.mode=="r3d":self.fps_label.configure(text=f"{fps} FPS / {self.state.triangles or '—'} 三角形")
        if self.state.mode=="r3d":
            self.scene_label.configure(text=f"{'茶杯 396 面' if self.state.scene else '走廊 · 世界弹幕'} · L{self.state.ladder or '—'}")
        else:self.scene_label.configure(text="场景操作由当前应用提供")
        self.notice.configure(bg="#ffe4e0" if backend == "sw" or self.port is None else "#e0ecff",
                              fg="#b42318" if backend == "sw" or self.port is None else "#164582")
        focus = self.forwarder.user32.GetForegroundWindow() == self.forwarder.focus
        keys = self.forwarder.last or 0
        display_keys=keys
        names=[]
        if display_keys&0x380==0x380:
            names.append("三维 [4]");display_keys&=~0x380
        names.extend(name for index,name in enumerate(KEY_NAMES) if display_keys&(1<<index))
        pressed="、".join(names) or "无"
        self.input_label.configure(text=("输入转发已停止（游戏继续）" if not self.enabled.get() else
                                  "键盘前台可用；手柄可用" if focus else "键盘已松开：点击面板可恢复；手柄可用") + f" · 当前按键：{pressed}")
        if self.port is not None and self.state.last_csv:
            age = time.monotonic() - self.state.last_csv
            self.connection.configure(text=f"已连接 {self.args.port} · 最近 FPS 反馈 {age:.1f} 秒前" +
                                      (" · 正在等待新统计" if age > 5 else ""))
        self.root.title(f"gfx2d 控制 · {self.state.label()} · {(backend or '未知').upper()} · {fps} FPS")
        if self.args.status_file and time.monotonic() - self.last_status_write >= 1:
            path = Path(self.args.status_file)
            path.parent.mkdir(parents=True, exist_ok=True)
            snapshot = dict(mode=self.state.mode, backend=backend, fps=self.state.fps, bullets=self.state.bullets,
                            protocol=self.state.protocol,app_id=self.state.app_id,errors=self.state.errors,
                            catalog=self.state.catalog,content_hash=self.state.catalog_hash,profile=self.state.profile,caps=self.state.capabilities,
                            triangles=self.state.triangles,ladder=self.state.ladder,scene=self.state.scene,
                            render_fps=self.state.render_fps,
                            connected=self.port is not None, keyboard_focus=focus, keys=keys,
                            notice=self.notice_text, updated=time.strftime("%Y-%m-%d %H:%M:%S"))
            temporary = path.with_suffix(path.suffix + ".tmp")
            temporary.write_text(json.dumps(snapshot, ensure_ascii=False), encoding="utf-8")
            temporary.replace(path)
            self.last_status_write = time.monotonic()

    def poll(self):
        # 整个循环体包自愈守卫：这里靠末尾 after() 自我续排，回调里抛任何
        # 未捕获异常都会让轮询静默死亡（窗口还在、串口还在，但收发全停，
        # 现象就是"面板卡住"——2026-10-28 检阅时实际发生）。任何异常都打印
        # 并继续调度；串口层面的断开仍走 disconnect 的用户提示。
        try:
            self._poll_body()
        except Exception:
            import traceback
            traceback.print_exc()
            if self.log:
                self.log.write("# poll exception (recovered)\n")
        if not self.closed:
            self.root.after(20, self.poll)

    def _poll_body(self):
        if self.port is not None:
            try:
                if self.enabled.get():
                    self.forwarder.send(self.port, time.monotonic())
                elif time.monotonic() - self.forwarder.last_sent >= 0.1:
                    self.port.write(packet(0))
                    self.forwarder.last, self.forwarder.last_sent = 0, time.monotonic()
                count = self.port.in_waiting
                if count:
                    chunk = self.decoder.decode(self.port.read(count))
                    if self.log:
                        self.log.write(chunk)
                    self.buffer += chunk
                    while "\n" in self.buffer:
                        line, self.buffer = self.buffer.split("\n", 1)
                        self.process_line(line.strip())
                    self.buffer = self.buffer[-8192:]
            except serial.SerialException as error:
                self.disconnect(error)
        if self.pending_backend and time.monotonic() - self.pending_since > 10:
            self.pending_backend = None
            self.notify("10 秒内没有确认后端切换。查看当前状态，必要时重新请求切换。")
        if self.restart_pending and time.monotonic() - self.restart_since > 10:
            self.restart_pending = False
            self.notify("10 秒内没有确认演示模式，重开未继续。查看模式状态后再操作。")
        self.refresh()

    def close(self):
        self.closed = True
        self.release_keys()
        if self.port is not None:
            self.port.close()
        if self.log:
            self.log.write(f"\n# control panel closed {time.strftime('%Y-%m-%d %H:%M:%S')}\n")
            self.log.close()
        self.root.destroy()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", default="COM11")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--ensure-hw", action="store_true", help="连接后根据板上反馈恢复硬件后端")
    ap.add_argument("--log", help="追加板上原始日志")
    ap.add_argument("--status-file", help="写出面板当前状态 JSON")
    ap.add_argument('--content',type=Path,help='冻结的应用清单')
    ap.add_argument('--bootstrap-log',type=Path,help='本次精确加载边界后的启动日志')
    args = ap.parse_args()
    root = tk.Tk()
    ControlPanel(root, args)
    root.after(300, lambda: (root.deiconify(), root.lift(), root.focus_force()))
    root.mainloop()


if __name__ == "__main__":
    main()
