"""连接正在运行的 gfx2d 游戏：一行状态、应用编号、硬件/软件切换，并转发键盘与手柄。"""

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


MODE_NAMES = {"menu":"应用菜单", "about":"系统信息", "fault":"故障恢复", "flight":"5 · 滑翔",
              "demo": "1 · 演示", "game": "2 · 弹幕", "press": "3 · 压力测试", "r3d": "4 · 三维走廊"}
MODE_IDS = {"1": "demo", "2": "game", "3": "press", "4": "r3d", "5": "flight", "6": "sh"}
MODE_SHORTCUTS = {mode: int(key) for key, mode in MODE_IDS.items()}
APP_NAMES = {"demo": "演示", "stg": "弹幕", "press": "压力测试", "r4": "三维走廊", "flight": "滑翔", "sh": "凝时", "about": "系统信息"}
SHORTCUT_BITS = {1: 1 << 7, 2: 1 << 8, 3: 1 << 9, 4: 0x380, 5: 1 << 13, 6: 1 << 14}
BACKEND_NAMES = {"hw": "硬件", "hw-tex": "硬件", "hw-strip": "硬件", "sw": "软件"}
APP_LINE_DEFAULT = "1 演示　2 弹幕　3 压力测试　4 三维走廊　5 滑翔　6 凝时"
KEY_LINE = "Enter 确认　Esc 返回　方向键 移动/选择　空格 炸弹　Shift 慢速"
BG, FONT = "#eef2f7", "Microsoft YaHei UI"


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
                if not (0<entry['id']<0xffffffff and entry['shortcut'] in range(7) and data['available'] in ('0','1')
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
        self.closed = False
        self.last_status_write = 0.0
        self.notice_text = ""
        self.notice_until = 0.0
        self.log = None
        if args.log:
            path = Path(args.log)
            path.parent.mkdir(parents=True, exist_ok=True)
            self.log = path.open("a", encoding="utf-8", buffering=1)
        self._build()
        root.update()
        self.forwarder = InputForwarder()
        user32 = self.forwarder.user32
        user32.GetAncestor.argtypes = (ctypes.c_void_p, ctypes.c_uint)
        user32.GetAncestor.restype = ctypes.c_void_p
        self.forwarder.focus = user32.GetAncestor(root.winfo_id(), 2)
        root.protocol("WM_DELETE_WINDOW", self.close)
        root.bind("<Tab>", lambda event: "break")
        root.after(100, self.connect)
        root.after(20, self.poll)

    def _build(self):
        # 只留一行状态（出问题时多一行红字）、一行应用编号、一行按键说明和三个按钮（2026-11-02 精简）。
        root = self.root
        root.title("gfx2d 控制")
        root.configure(bg=BG)
        root.geometry("660x240+50+20")
        root.minsize(560, 220)
        style = ttk.Style(root)
        style.theme_use("clam")
        style.configure("TFrame", background=BG)
        style.configure("TButton", font=(FONT, 11), padding=(16, 6))
        outer = ttk.Frame(root, padding=16)
        outer.pack(fill="both", expand=True)
        self.status = tk.Label(outer, text="等待连接", anchor="w", justify="left", wraplength=620,
                               bg=BG, fg="#17243b", font=(FONT, 16, "bold"))
        self.status.pack(fill="x")
        self.apps_label = tk.Label(outer, text=APP_LINE_DEFAULT, anchor="w", bg=BG, fg="#17243b", font=(FONT, 12))
        self.apps_label.pack(fill="x", pady=(10, 2))
        tk.Label(outer, text=KEY_LINE, anchor="w", bg=BG, fg="#53657e", font=(FONT, 11)).pack(fill="x")
        buttons = ttk.Frame(outer)
        buttons.pack(fill="x", pady=(14, 0))
        ttk.Button(buttons, text="硬件", command=lambda: self.choose_backend("hw")).pack(side="left", padx=(0, 6))
        ttk.Button(buttons, text="软件", command=lambda: self.choose_backend("sw")).pack(side="left")
        ttk.Button(buttons, text="重新连接", command=self.connect).pack(side="right")

    def notify(self, message):
        self.notice_text = message
        self.notice_until = time.monotonic() + 4.0

    def app_name(self):
        entry = self.state.catalog.get(self.state.app_id)
        if entry:
            return APP_NAMES.get(entry['key'], entry['title'])
        return MODE_NAMES.get(self.state.mode, self.state.mode or "等待板上反馈")

    def app_line(self):
        entries = sorted((e for e in self.state.catalog.values()
                          if e.get('shortcut') in SHORTCUT_BITS and e.get('available') is not False),
                         key=lambda e: e['shortcut'])
        if not entries:
            return APP_LINE_DEFAULT
        parts = []
        for e in entries:
            text = f"{e['shortcut']} {APP_NAMES.get(e['key'], e['title'])}"
            parts.append(f"[{text}]" if e['id'] == self.state.app_id else text)
        return "　".join(parts)

    def connect(self):
        if self.port is not None:
            self.release_keys()
            if self.port is not None:
                self.port.close()
            self.port = None
        self.state = BoardState()
        self.pending_backend = None
        self.buffer = ""
        self.decoder.reset()
        try:
            self.port = serial.Serial(self.args.port, self.args.baud, timeout=0, write_timeout=0.2)
            self.port.reset_input_buffer()
            self.forwarder.last = None
            self.ensure_hw = self.args.ensure_hw
            self.notify("已连接，等待板上反馈。")
            if self.log:
                self.log.write(f"\n# control panel connected {time.strftime('%Y-%m-%d %H:%M:%S')}\n")
        except serial.SerialException as error:
            self.notify(f"串口打不开：{error}")
        self.refresh()

    def send_action(self, mask):
        if self.port is None:
            self.notify("串口未连接；先点「重新连接」。")
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
        name = BACKEND_NAMES.get(target, target)
        if self.state.protocol>=2 and self.state.mode in (None,"menu","about","fault"):
            self.notify("先进入应用，再切换硬件/软件。")
            return
        if self.state.mode == "r3d" and target.startswith("hw"):
            target = "hw"
        if self.pending_backend is not None:
            self.notify("正在等板上确认上一次切换。")
        elif self.state.backend is None:
            self.notify("还没收到板上状态，稍等再切。")
        elif self.state.backend == target:
            self.notify(f"已是{name}。")
        elif self.send_action(1 << 6):
            self.pending_backend = target
            self.pending_steps = 1
            self.pending_since = time.monotonic()
            self.notify(f"正在切到{name}……")

    def choose_backend(self, target):
        # 面板只分硬件/软件：已在任一硬件后端时「硬件」不再按 Tab 轮换。
        if target == "hw" and (self.state.backend or "").startswith("hw"):
            self.notify("已是硬件。")
            return
        self.set_backend(target)

    def switch_app(self, step):
        """手柄 LB/RB：切到上一个/下一个应用，等价于按对应数字键。"""
        shortcuts = sorted(e['shortcut'] for e in self.state.catalog.values()
                           if e.get('shortcut') in SHORTCUT_BITS and e.get('available') is not False)
        order = shortcuts or sorted(SHORTCUT_BITS)
        entry = self.state.catalog.get(self.state.app_id)
        current = entry.get('shortcut') if entry else MODE_SHORTCUTS.get(self.state.mode)
        if current in order:
            target = order[(order.index(current) + step) % len(order)]
        else:
            target = order[0] if step > 0 else order[-1]
        self.send_action(SHORTCUT_BITS[target])

    def release_keys(self):
        if self.port is not None:
            try:
                self.port.write(packet(0))
                self.forwarder.last = None
            except serial.SerialException as error:
                self.disconnect(error)

    def disconnect(self, error):
        if self.port is not None:
            self.port.close()
        self.port = None
        self.notify(f"串口断开：{error}。插好 USB 后点「重新连接」。")
        if self.log:
            self.log.write(f"# serial lost: {error}\n")

    def process_line(self, line):
        before_backend = self.state.backend
        self.state.consume(line)
        if line.startswith("console enter "):
            # A backend confirmation belongs to one application. Never replay
            # its remaining Tab actions into the newly selected application.
            self.pending_backend=None
        if line.startswith("console error ") or line.startswith("console fatal "):
            self.pending_backend=None
            self.notify("板上报错："+line)
            return
        if self.pending_backend == self.state.backend:
            self.pending_backend = None
        elif self.pending_backend and before_backend != self.state.backend:
            if self.pending_steps >= 4:
                self.pending_backend = None
                self.notify("当前应用没有这个模式，已停止切换。")
            elif self.send_action(1 << 6):
                self.pending_steps += 1
                self.pending_since = time.monotonic()
        if self.ensure_hw and self.state.backend is not None:
            self.ensure_hw = False
            if self.state.backend == "sw":
                self.set_backend("hw")

    def refresh(self):
        focus = self.forwarder.user32.GetForegroundWindow() == self.forwarder.focus
        keys = self.forwarder.last or 0
        backend = self.state.backend
        fps = f"{self.state.fps:.0f} FPS" if self.state.fps is not None else "— FPS"
        mode = BACKEND_NAMES.get(backend, "—")
        name = self.app_name()
        connected = self.port is not None
        lines = [" · ".join((name, fps, mode, "已连接" if connected else "未连接 " + self.args.port))]
        problems = []
        if connected and self.state.errors:
            problems.append(f"板上错误 {self.state.errors}")
        if connected and not focus:
            problems.append("键盘没对准面板：点一下面板")
        if problems:
            lines.append("；".join(problems))
        if self.notice_text and time.monotonic() < self.notice_until:
            lines.append(self.notice_text)
        alert = not connected or bool(problems) or backend == "sw"
        self.status.configure(text="\n".join(lines), fg="#b42318" if alert else "#167144")
        self.apps_label.configure(text=self.app_line())
        self.root.title(f"gfx2d 控制 · {name} · {mode} · {fps}")
        if self.args.status_file and time.monotonic() - self.last_status_write >= 1:
            path = Path(self.args.status_file)
            path.parent.mkdir(parents=True, exist_ok=True)
            snapshot = dict(mode=self.state.mode, backend=backend, fps=self.state.fps, bullets=self.state.bullets,
                            protocol=self.state.protocol,app_id=self.state.app_id,errors=self.state.errors,
                            catalog=self.state.catalog,content_hash=self.state.catalog_hash,profile=self.state.profile,caps=self.state.capabilities,
                            triangles=self.state.triangles,ladder=self.state.ladder,scene=self.state.scene,
                            render_fps=self.state.render_fps,
                            connected=connected, keyboard_focus=focus, keys=keys,
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
                self.forwarder.send(self.port, time.monotonic())
                step = self.forwarder.app_step
                if step:
                    self.forwarder.app_step = 0
                    self.switch_app(1 if step > 0 else -1)
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
            self.notify("10 秒没等到板上确认，没切成。")
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
