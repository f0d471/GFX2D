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


MODE_NAMES = {"demo": "1 · 图形演示", "game": "2 · 弹幕游戏", "press": "3 · 压力测试"}
MODE_IDS = {"1": "demo", "2": "game", "3": "press"}
KEY_NAMES = ("上", "下", "左", "右", "炸弹", "慢速", "切后端", "模式1", "模式2", "模式3")


class BoardState:
    """从板上日志更新状态；未收到反馈时保留未知值。"""

    def __init__(self):
        self.mode = None
        self.backend = None
        self.fps = None
        self.bullets = None
        self.last_csv = 0.0
        self.headers = None

    def consume(self, line):
        before = (self.mode, self.backend)
        if line.startswith("csvh,"):
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
        else:
            match = re.match(r"mode ([123]) enter frame ", line)
            if match:
                self.mode = MODE_IDS[match[1]]
                self.fps = None
        return before != (self.mode, self.backend)


class ControlPanel:
    def __init__(self, root, args):
        self.root, self.args = root, args
        self.state = BoardState()
        self.port = None
        self.decoder = codecs.getincrementaldecoder("utf-8")("replace")
        self.buffer = ""
        self.ensure_hw = args.ensure_hw
        self.pending_backend = None
        self.pending_since = 0.0
        self.pending_steps = 0
        self.restart_pending = False
        self.restart_since = 0.0
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
        root.bind("<Escape>", self.toggle_input)
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
        self.fps_label = self._status_box(status, "实际帧率 / 敌弹数", "— FPS / — 颗", 2)
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
        ttk.Button(actions, text="重开本局（1 → 2）", command=self.restart_game).pack(side="left", padx=(0, 6))
        ttk.Button(actions, text="重新连接", command=self.connect).pack(side="right")
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
            ("空格（每次按下）", "使用一颗炸弹，消除敌弹并获得无敌"),
            ("射击", "自动射击，无需按射击键"),
            ("1 / 2 / 3", "图形演示 / 弹幕游戏 / 压力测试"),
            ("Tab", "HW ↔ SW；SW 由板上 CPU 绘图，会很慢"),
            ("重开游戏", "先按 1，待切换后再按 2；或点重开按钮"),
            ("Esc", "启用 / 停止输入转发，板上游戏继续运行"),
        ))
        self._rules(right, (
            ("手柄方向键 / 左摇杆", "移动飞船（第一个 XInput 手柄）"),
            ("手柄 A / LB / Start", "炸弹 / 慢速 / 切换 HW、SW"),
            ("板上 SW0 / SW1", "左 / 下"),
            ("板上 SW2 / SW3", "上 / 右"),
            ("上 + 下，保持 8 帧", "切到下一个模式：1 → 2 → 3 → 1"),
            ("左 + 右，保持 8 帧", "切换 HW、SW；键盘 A+D 同样会触发"),
            ("组合键再次触发", "这一对键都松开后，才能再次触发"),
            ("模式 1 / 模式 3", "演示或压力测试，不是可操作的游戏"),
        ))
        warning = tk.Label(outer, text="误触提醒：A+D / 左右同时按 8 帧会切换绘图后端；W+S / 上下会切换模式。\n"
                           "在 60 FPS 下，8 帧只需约 0.13 秒。切换后看上方当前状态，卡顿时点“回到 HW”。",
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
        ttk.Checkbutton(outer, text="启用键盘与手柄转发（Esc 切换；停止转发不会暂停游戏）",
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
        if self.state.mode is None:
            self.notify("尚未收到当前模式；等待板上反馈后再重开。")
            return
        if self.state.mode == "demo":
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
        if changed:
            mode = MODE_NAMES.get(self.state.mode, "模式未知")
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
        if self.restart_pending and self.state.mode == "demo":
            self.restart_pending = False
            self.send_action(1 << 8)
        if self.ensure_hw and self.state.backend is not None:
            self.ensure_hw = False
            if self.state.backend == "sw":
                self.set_backend("hw")

    def refresh(self):
        self.mode_label.configure(text=MODE_NAMES.get(self.state.mode, "等待板上反馈"))
        backend = self.state.backend
        self.backend_label.configure(text={"hw": "HW/DDR · FPGA 加速", "hw-tex": "HW/TEX · 片上纹理", "hw-strip": "HW/STRIP · 条带渲染", "sw": "SW · CPU 绘图"}.get(backend, "等待板上反馈"),
                                     fg="#167144" if backend in ("hw", "hw-tex", "hw-strip") else "#b42318" if backend == "sw" else "#17243b")
        fps = f"{self.state.fps:.1f}" if self.state.fps is not None else "—"
        bullets = str(self.state.bullets) if self.state.bullets is not None else "—"
        self.fps_label.configure(text=f"{fps} FPS / {bullets} 颗")
        self.notice.configure(bg="#ffe4e0" if backend == "sw" or self.port is None else "#e0ecff",
                              fg="#b42318" if backend == "sw" or self.port is None else "#164582")
        focus = self.forwarder.user32.GetForegroundWindow() == self.forwarder.focus
        keys = self.forwarder.last or 0
        pressed = "、".join(name for index, name in enumerate(KEY_NAMES) if keys & (1 << index)) or "无"
        self.input_label.configure(text=("输入转发已停止（游戏继续）" if not self.enabled.get() else
                                  "键盘前台可用；手柄可用" if focus else "键盘已松开：点击面板可恢复；手柄可用") + f" · 当前按键：{pressed}")
        if self.port is not None and self.state.last_csv:
            age = time.monotonic() - self.state.last_csv
            self.connection.configure(text=f"已连接 {self.args.port} · 最近 FPS 反馈 {age:.1f} 秒前" +
                                      (" · 正在等待新统计" if age > 5 else ""))
        self.root.title(f"gfx2d 控制 · {MODE_NAMES.get(self.state.mode, '等待反馈')} · {(backend or '未知').upper()} · {fps} FPS")
        if self.args.status_file and time.monotonic() - self.last_status_write >= 1:
            path = Path(self.args.status_file)
            path.parent.mkdir(parents=True, exist_ok=True)
            snapshot = dict(mode=self.state.mode, backend=backend, fps=self.state.fps, bullets=self.state.bullets,
                            connected=self.port is not None, keyboard_focus=focus, keys=keys,
                            notice=self.notice_text, updated=time.strftime("%Y-%m-%d %H:%M:%S"))
            temporary = path.with_suffix(path.suffix + ".tmp")
            temporary.write_text(json.dumps(snapshot, ensure_ascii=False), encoding="utf-8")
            temporary.replace(path)
            self.last_status_write = time.monotonic()

    def poll(self):
        if self.closed:
            return
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
        self.root.after(20, self.poll)

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
    args = ap.parse_args()
    root = tk.Tk()
    ControlPanel(root, args)
    root.after(300, lambda: (root.deiconify(), root.lift(), root.focus_force()))
    root.mainloop()


if __name__ == "__main__":
    main()
